// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <App/DocumentRevisionIndex.h>

#include <FCGlobal.h>

#include <cstddef>
#include <functional>
#include <string>

namespace App
{
class Document;
class DocumentObject;
}  // namespace App

namespace Gui
{

class ViewProvider;

/** Classification for GUI-side Python admission. */
enum class GuiPythonGateCallbackKind
{
    /** UI-only work that never touches live model state. May be queued. */
    NonModel,
    /** Python document observer delivery using copied value payloads only. */
    ObserverDelivery,
    /** Any callback that may read or mutate live App model state. */
    ModelTouching
};

enum class GuiPythonGateAdmissionResult
{
    Executed,
    Queued,
    RejectedGilBusy,
    RejectedDocumentBusy,
    RejectedUnsupported,
    RejectedNotOnGuiThread
};

struct GuiExport GuiPythonGateAdmissionOutcome
{
    GuiPythonGateAdmissionResult result {GuiPythonGateAdmissionResult::RejectedUnsupported};
    std::string diagnostic;

    [[nodiscard]] bool executed() const noexcept
    {
        return result == GuiPythonGateAdmissionResult::Executed;
    }

    [[nodiscard]] bool queued() const noexcept
    {
        return result == GuiPythonGateAdmissionResult::Queued;
    }
};

/** Pointer-free queued observer notification (no live model references). */
struct GuiExport GuiPythonObserverValueEvent
{
    App::DocumentRevisionIdentityBinding document;
    std::string observerToken;
    std::string eventKind;
    std::string payloadJson;
};

using GuiPythonGateCallback = std::function<void()>;

/**
 * Central gate for GUI-thread Python entry.
 *
 * Checks document and GIL availability before acquisition. Non-model callbacks
 * may be queued when the GIL is held by document Python; model-touching and
 * undeclared observer work is rejected immediately. The native GUI event loop
 * never blocks waiting for the GIL.
 */
class GuiExport GuiPythonGate
{
public:
    [[nodiscard]] static GuiPythonGateAdmissionOutcome tryAdmit(
        GuiPythonGateCallbackKind kind,
        GuiPythonGateCallback callback,
        const App::Document* document = nullptr);

    /** Run deferred non-model callbacks when the GIL is available. */
    [[nodiscard]] static std::size_t pumpQueuedCallbacks(std::size_t maxCallbacks = 8);

    [[nodiscard]] static bool gilAvailableForNonBlockingAcquire() noexcept;

    [[nodiscard]] static bool documentModelIngressAvailable(
        const App::Document& document) noexcept;

    [[nodiscard]] static GuiPythonGateAdmissionOutcome verifyFeaturePythonExecution(
        const App::DocumentObject& object);

    [[nodiscard]] static GuiPythonGateAdmissionOutcome verifyViewProviderPythonExecution(
        const ViewProvider& provider);

    static void enqueueObserverValueEvent(GuiPythonObserverValueEvent event);
};

}  // namespace Gui
