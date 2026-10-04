// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GuiPythonGate.h"

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentObject.h>
#include <App/DocumentWouldBlock.h>
#include <Base/Interpreter.h>

#include "Utilities.h"
#include "ViewProvider.h"

#include <Python.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

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

/**
 * M6: Single long-lived GIL probe thread.
 *
 * Replaces the per-attempt OrphanGilProbe approach. Only one probe is in
 * flight at a time: concurrent callers receive false immediately instead of
 * piling up blocked threads.
 *
 * Handshake: the probe calls PyGILState_Ensure(), then immediately releases
 * and signals the GUI thread. The GUI calls PyGILState_Ensure() right after
 * waking, minimising the check-then-act window without needing CPython
 * internals.
 */
class GilProbeService
{
public:
    GilProbeService() : _thread([this] { threadMain(); }) {}

    ~GilProbeService()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _shutdown = true;
        }
        _cv.notify_one();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    /**
     * Probe whether the GIL is available within @p timeout. On success the
     * GIL is now held by the calling thread (*outState from PyGILState_Ensure).
     * Returns false if the probe timed out, the probe is busy, or Python is
     * not initialised.
     */
    bool tryAcquire(PyGILState_STATE* outState,
                    const std::chrono::milliseconds timeout) noexcept
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_busy || _shutdown) {
                return false;
            }
            _busy = true;
            _probeReady = false;
            _probeResult = false;
        }
        _cv.notify_one();

        // Hold the handoff across the probe and the GUI Ensure so another
        // PyGILStateLocker cannot take the GIL in the gap (M6).
        Base::GilGuiHandoff::reserve();
        struct ReleaseHandoff
        {
            ~ReleaseHandoff()
            {
                Base::GilGuiHandoff::release();
            }
        } releaseHandoff;

        bool signalled = false;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            signalled = _cv.wait_for(lock, timeout, [this] { return _probeReady; });
        }
        if (!signalled || !_probeResult) {
            std::lock_guard<std::mutex> lock(_mutex);
            _busy = false;
            _cv.notify_one();
            return false;
        }

        // Probe has acquired and released the GIL; call Ensure now on our thread.
        *outState = PyGILState_Ensure();

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _busy = false;
        }
        _cv.notify_one();
        return true;
    }

private:
    void threadMain()
    {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _cv.wait(lock, [this] { return _busy || _shutdown; });
                if (_shutdown) {
                    break;
                }
            }

            if (!Py_IsInitialized()) {
                // Python already finalized; cannot acquire GIL.
                std::lock_guard<std::mutex> lock(_mutex);
                _probeResult = false;
                _probeReady = true;
                _busy = false;
                _cv.notify_one();
                continue;
            }

            PyGILState_STATE probeState = PyGILState_Ensure();
            PyGILState_Release(probeState);

            {
                std::lock_guard<std::mutex> lock(_mutex);
                _probeResult = true;
                _probeReady = true;
            }
            _cv.notify_one();

            // Wait briefly for the caller to acquire before accepting a new request.
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _cv.wait_for(lock, std::chrono::milliseconds(50),
                             [this] { return !_busy || _shutdown; });
            }
        }
    }

    std::mutex _mutex;
    std::condition_variable _cv;
    std::thread _thread;
    bool _busy {false};
    bool _shutdown {false};
    bool _probeReady {false};
    bool _probeResult {false};
};

GilProbeService& gilProbeService()
{
    static GilProbeService instance;
    return instance;
}

bool tryAcquireGilWithoutBlocking(PyGILState_STATE* state) noexcept
{
    using namespace std::chrono_literals;
    return gilProbeService().tryAcquire(state, 2ms);
}

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
            if (!tryAcquireGilWithoutBlocking(&_state)) {
                return;
            }
            _owns = true;
            _usedEnsure = true;
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

void scheduleQueuedCallbackPump()
{
    // Owner-thread admissions enqueue work that must run on the GUI thread.
    // Kick a zero-delay pump so delivery is not stranded until an unrelated
    // MainWindow activity tick.
    scheduleGuiSingleShot(0, []() { static_cast<void>(GuiPythonGate::pumpQueuedCallbacks(64)); });
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
    {
        std::lock_guard lock(g_queueMutex);
        g_deferredObserverEvents.push_back(std::move(event));
    }
    // Always nudge a GUI pump. Off-thread admissions otherwise wait for an
    // unrelated MainWindow tick; on-thread GIL-busy queues need a later pass.
    scheduleQueuedCallbackPump();
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

    // Observer / UI Python wraps Gui::Document and ViewProviderDocumentObject
    // Python objects. Those getPyObject paths require the GUI thread; never
    // invoke them from the document owner. Queue pointer-free value events
    // (ObserverDelivery) or the callback itself (NonModel) for later pump.
    if (!App::DocumentWouldBlock::isGuiThread()) {
        if (kind == GuiPythonGateCallbackKind::ObserverDelivery) {
            return makeOutcome(
                GuiPythonGateAdmissionResult::Queued,
                "observer delivery deferred to the GUI thread");
        }
        if (kind == GuiPythonGateCallbackKind::NonModel) {
            queueCallback(std::move(callback));
            scheduleQueuedCallbackPump();
            return makeOutcome(
                GuiPythonGateAdmissionResult::Queued,
                "non-model Python callback deferred to the GUI thread");
        }
        // ModelTouching on an owner/worker thread may run with the GIL.
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
