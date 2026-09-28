// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GuiPythonGate.h"

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentObject.h>
#include <App/DocumentWouldBlock.h>
#include <Base/Interpreter.h>

#include "ViewProvider.h"

#include <Python.h>

#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace Gui
{
namespace
{

std::mutex g_queueMutex;
std::deque<GuiPythonGateCallback> g_deferredCallbacks;
std::deque<GuiPythonObserverValueEvent> g_deferredObserverEvents;
GuiPythonObserverValueEventHandler g_observerHandler;

bool anyDocumentExecutionLaneBusy() noexcept
{
    for (auto* document : App::GetApplication().getDocuments()) {
        if (!document) {
            continue;
        }
        const auto* lane = document->executionLane();
        if (lane && !lane->isIdle()) {
            return true;
        }
    }
    return false;
}

#if PY_VERSION_HEX >= 0x030c0000
bool tryAcquireGilWithoutBlocking(PyGILState_STATE* state) noexcept
{
    std::promise<void> probeFinished;
    auto probeDone = probeFinished.get_future();
    std::thread probe([&probeFinished] {
        PyGILState_STATE probeState = PyGILState_Ensure();
        PyGILState_Release(probeState);
        probeFinished.set_value();
    });
    using namespace std::chrono_literals;
    if (probeDone.wait_for(2ms) != std::future_status::ready) {
        probe.detach();
        return false;
    }
    probe.join();
    *state = PyGILState_Ensure();
    return true;
}
#endif

/**
 * Non-blocking GIL ownership for GUI admission.
 *
 * On the GUI thread, never block on the GIL while dispatchToOwner is pumping
 * Qt (deadlock with BlockingQueued MainThreadSignal). When lanes are idle and
 * pumping is inactive, acquire via a non-blocking probe (Python 3.12+) or
 * PyEval_TryAcquireLock (older). Off the GUI thread, PyGILState_Ensure is used
 * only when every document lane is idle.
 */
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
            return;
        }
        if (!acquire) {
            return;
        }
        const bool onGui = App::DocumentWouldBlock::isGuiThread();
        if (onGui) {
            if (App::documentExecutionLaneGuiDispatchPumpActive()) {
                return;
            }
            if (anyDocumentExecutionLaneBusy()) {
                return;
            }
#if PY_VERSION_HEX < 0x030c0000
            if (!PyEval_TryAcquireLock()) {
                return;
            }
            _owns = true;
            _usedDeprecatedGilLock = true;
#else
            if (!tryAcquireGilWithoutBlocking(&_state)) {
                return;
            }
            _owns = true;
            _usedEnsure = true;
#endif
            return;
        }
        if (anyDocumentExecutionLaneBusy()) {
            return;
        }
        _state = PyGILState_Ensure();
        _owns = true;
        _usedEnsure = true;
    }

    ~GilTryLock()
    {
        if (!_owns) {
            return;
        }
        if (_usedDeprecatedGilLock) {
#if PY_VERSION_HEX < 0x030c0000
            PyEval_ReleaseLock();
#endif
            return;
        }
        if (_usedEnsure) {
            PyGILState_Release(_state);
        }
    }

    [[nodiscard]] bool owns() const noexcept
    {
        return _owns;
    }

    GilTryLock(const GilTryLock&) = delete;
    GilTryLock& operator=(const GilTryLock&) = delete;

private:
    PyGILState_STATE _state {};
    bool _owns {false};
    bool _usedEnsure {false};
    bool _usedDeprecatedGilLock {false};
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

void deliverObserverEvent(const GuiPythonObserverValueEvent& event)
{
    GuiPythonObserverValueEventHandler handler;
    {
        std::lock_guard lock(g_queueMutex);
        handler = g_observerHandler;
    }
    if (handler) {
        handler(event);
    }
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
    if (App::DocumentWouldBlock::isGuiThread()
        && App::documentExecutionLaneGuiDispatchPumpActive()) {
        return false;
    }
    return !anyDocumentExecutionLaneBusy();
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
    if (!object.requiresDocumentThreadExecutionDeclaration()) {
        return makeOutcome(GuiPythonGateAdmissionResult::Executed);
    }
    if (!object.declaresDocumentThreadExecution()) {
        return makeOutcome(
            GuiPythonGateAdmissionResult::RejectedUnsupported,
            "Python feature must declare supportsDocumentThreadExecution() before lane execution");
    }
    return makeOutcome(GuiPythonGateAdmissionResult::Executed);
}

GuiPythonGateAdmissionOutcome GuiPythonGate::verifyViewProviderPythonExecution(
    const ViewProvider& provider)
{
    if (!provider.requiresAsyncPresentationDeclaration()) {
        return makeOutcome(GuiPythonGateAdmissionResult::Executed);
    }
    if (!provider.declaresAsyncPresentation()) {
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

void GuiPythonGate::setObserverValueEventHandler(GuiPythonObserverValueEventHandler handler)
{
    std::lock_guard lock(g_queueMutex);
    g_observerHandler = std::move(handler);
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
            return makeOutcome(GuiPythonGateAdmissionResult::Queued,
                               "observer delivery deferred until the GIL is available");
        }
        GilTryLock gilLock(true);
        if (!gilLock.owns()) {
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
        GuiPythonObserverValueEvent event;
        {
            std::lock_guard lock(g_queueMutex);
            if (g_deferredObserverEvents.empty()) {
                break;
            }
            event = std::move(g_deferredObserverEvents.front());
            g_deferredObserverEvents.pop_front();
        }
        GilTryLock gilLock(true);
        if (!gilLock.owns()) {
            std::lock_guard lock(g_queueMutex);
            g_deferredObserverEvents.push_front(std::move(event));
            break;
        }
        deliverObserverEvent(event);
        ++processed;
    }

    return processed;
}

}  // namespace Gui
