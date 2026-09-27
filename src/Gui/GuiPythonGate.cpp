// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GuiPythonGate.h"

#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentWouldBlock.h>
#include <App/FeaturePython.h>
#include <Base/Interpreter.h>

#include "ViewProvider.h"
#include "ViewProviderFeaturePython.h"

#include <Python.h>

#include <deque>
#include <mutex>
#include <utility>

namespace Gui
{
namespace
{

std::mutex g_queueMutex;
std::deque<GuiPythonGateCallback> g_deferredCallbacks;
std::deque<GuiPythonObserverValueEvent> g_deferredObserverEvents;

class GilTryLock
{
public:
    explicit GilTryLock(const bool acquire)
    {
        if (!Py_IsInitialized()) {
            _owns = true;
            return;
        }
        if (PyGILState_Check()) {
            _owns = true;
            _releasedGil = false;
            return;
        }
        if (!acquire) {
            return;
        }
        if (PyEval_TryAcquireLock()) {
            _owns = true;
            _releasedGil = true;
        }
    }

    ~GilTryLock()
    {
        if (_owns && _releasedGil) {
            PyEval_ReleaseLock();
        }
    }

    [[nodiscard]] bool owns() const noexcept
    {
        return _owns;
    }

    GilTryLock(const GilTryLock&) = delete;
    GilTryLock& operator=(const GilTryLock&) = delete;

private:
    bool _owns {false};
    bool _releasedGil {false};
};

GuiPythonGateAdmissionOutcome makeOutcome(
    const GuiPythonGateAdmissionResult result,
    const char* diagnostic = nullptr)
{
    GuiPythonGateAdmissionOutcome outcome;
    outcome.result = result;
    if (diagnostic) {
        outcome.diagnostic = diagnostic;
    }
    return outcome;
}

void runWithGil(GuiPythonGateCallback& callback)
{
    if (!Py_IsInitialized()) {
        callback();
        return;
    }
    if (PyGILState_Check()) {
        callback();
        return;
    }
    Base::PyGILStateLocker lock;
    callback();
}

bool queueCallback(GuiPythonGateCallback callback)
{
    std::lock_guard lock(g_queueMutex);
    g_deferredCallbacks.push_back(std::move(callback));
    return true;
}

}  // namespace

bool GuiPythonGate::gilAvailableForNonBlockingAcquire() noexcept
{
    if (!Py_IsInitialized()) {
        return true;
    }
    if (PyGILState_Check()) {
        return true;
    }
    if (!PyEval_TryAcquireLock()) {
        return false;
    }
    PyEval_ReleaseLock();
    return true;
}

bool GuiPythonGate::documentModelIngressAvailable(const App::Document& document) noexcept
{
    const auto* lane = document.executionLane();
    if (!lane) {
        return true;
    }
    return lane->isIdle();
}

GuiPythonGateAdmissionOutcome GuiPythonGate::verifyFeaturePythonExecution(
    const App::DocumentObject& object)
{
    const auto* pythonFeature = dynamic_cast<const App::FeaturePython*>(&object);
    if (!pythonFeature) {
        return makeOutcome(GuiPythonGateAdmissionResult::Executed);
    }
    if (!pythonFeature->declaresDocumentThreadExecution()) {
        return makeOutcome(
            GuiPythonGateAdmissionResult::RejectedUnsupported,
            "Python feature must declare supportsDocumentThreadExecution() before lane execution");
    }
    return makeOutcome(GuiPythonGateAdmissionResult::Executed);
}

GuiPythonGateAdmissionOutcome GuiPythonGate::verifyViewProviderPythonExecution(
    const ViewProvider& provider)
{
    const auto* pythonProvider =
        dynamic_cast<const ViewProviderFeaturePython*>(&provider);
    if (!pythonProvider) {
        return makeOutcome(GuiPythonGateAdmissionResult::Executed);
    }
    if (!pythonProvider->declaresAsyncPresentation()) {
        return makeOutcome(
            GuiPythonGateAdmissionResult::RejectedUnsupported,
            "Python view provider must declare supportsAsyncPresentation() before async GUI "
            "presentation");
    }
    return makeOutcome(GuiPythonGateAdmissionResult::Executed);
}

void GuiPythonGate::enqueueObserverValueEvent(GuiPythonObserverValueEvent event)
{
    std::lock_guard lock(g_queueMutex);
    g_deferredObserverEvents.push_back(std::move(event));
}

GuiPythonGateAdmissionOutcome GuiPythonGate::tryAdmit(
    GuiPythonGateCallbackKind kind,
    GuiPythonGateCallback callback,
    const App::Document* document)
{
    if (!callback) {
        return makeOutcome(GuiPythonGateAdmissionResult::RejectedUnsupported,
                           "GuiPythonGate callback is empty");
    }

    if (!App::DocumentWouldBlock::isGuiThread()) {
        runWithGil(callback);
        return makeOutcome(GuiPythonGateAdmissionResult::Executed);
    }

    if (kind == GuiPythonGateCallbackKind::ModelTouching) {
        if (document && !documentModelIngressAvailable(*document)) {
            return makeOutcome(
                GuiPythonGateAdmissionResult::RejectedDocumentBusy,
                "document execution lane is busy");
        }
        GilTryLock gilLock(true);
        if (!gilLock.owns()) {
            return makeOutcome(
                GuiPythonGateAdmissionResult::RejectedGilBusy,
                "GUI Python cannot wait for the GIL while document Python is running");
        }
        callback();
        return makeOutcome(GuiPythonGateAdmissionResult::Executed);
    }

    if (kind == GuiPythonGateCallbackKind::ObserverDelivery) {
        if (!gilAvailableForNonBlockingAcquire()) {
            queueCallback(std::move(callback));
            return makeOutcome(GuiPythonGateAdmissionResult::Queued,
                               "observer delivery deferred until the GIL is available");
        }
        GilTryLock gilLock(true);
        if (!gilLock.owns()) {
            queueCallback(std::move(callback));
            return makeOutcome(GuiPythonGateAdmissionResult::Queued,
                               "observer delivery deferred until the GIL is available");
        }
        callback();
        return makeOutcome(GuiPythonGateAdmissionResult::Executed);
    }

    // NonModel
    if (!gilAvailableForNonBlockingAcquire()) {
        queueCallback(std::move(callback));
        return makeOutcome(GuiPythonGateAdmissionResult::Queued,
                           "non-model Python callback deferred until the GIL is available");
    }
    GilTryLock gilLock(true);
    if (!gilLock.owns()) {
        queueCallback(std::move(callback));
        return makeOutcome(GuiPythonGateAdmissionResult::Queued,
                           "non-model Python callback deferred until the GIL is available");
    }
    callback();
    return makeOutcome(GuiPythonGateAdmissionResult::Executed);
}

std::size_t GuiPythonGate::pumpQueuedCallbacks(const std::size_t maxCallbacks)
{
    if (!App::DocumentWouldBlock::isGuiThread()) {
        return 0;
    }
    if (!gilAvailableForNonBlockingAcquire()) {
        return 0;
    }

    std::size_t processed = 0;
    while (processed < maxCallbacks) {
        GuiPythonGateCallback callback;
        {
            std::lock_guard lock(g_queueMutex);
            if (g_deferredCallbacks.empty()) {
                break;
            }
            callback = std::move(g_deferredCallbacks.front());
            g_deferredCallbacks.pop_front();
        }
        if (!callback) {
            continue;
        }
        GilTryLock gilLock(true);
        if (!gilLock.owns()) {
            std::lock_guard lock(g_queueMutex);
            g_deferredCallbacks.push_front(std::move(callback));
            break;
        }
        callback();
        ++processed;
    }

    while (processed < maxCallbacks) {
        {
            std::lock_guard lock(g_queueMutex);
            if (g_deferredObserverEvents.empty()) {
                break;
            }
            g_deferredObserverEvents.pop_front();
        }
        ++processed;
    }

    return processed;
}

}  // namespace Gui
