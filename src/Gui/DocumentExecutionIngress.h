// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <App/DocumentCommand.h>
#include <App/DocumentCommandHandle.h>

#include <FCGlobal.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace App
{
class Document;
class DocumentObject;
}  // namespace App

namespace Gui
{

/** Tracks the one in-flight undo/redo command observed by passive completion. */
struct GuiExport UndoRedoCompletionAnchor
{
    std::atomic<bool> active {true};
    std::atomic<App::DocumentCommandId> inFlightCommandId {0};
};

/** Pointer-free recompute admission payload copied before trySubmit(). */
struct DocumentRecomputeSubmitRequest
{
    std::vector<std::string> featureIds;
    bool force = false;
    int options = 0;
};

/** Build one recompute command from stable document/object identities. */
GuiExport App::DocumentCommand makeDocumentRecomputeCommand(
    App::Document& document,
    const DocumentRecomputeSubmitRequest& request = {});

/** Build one pointer-free Save-As command for lane admission. */
GuiExport App::DocumentCommand makeDocumentSaveAsCommand(
    App::Document& document,
    std::string targetPath,
    bool overwrite,
    std::string expectedDestinationSha256 = {});

/** Admit Save As on the execution lane without blocking the GUI thread. */
GuiExport App::DocumentCommandSubmitOutcome submitDocumentSaveAs(
    App::Document& document,
    std::string targetPath,
    bool overwrite,
    std::string expectedDestinationSha256 = {});

/** Build one pointer-free kind-only command for Save, Undo, or Redo admission. */
GuiExport App::DocumentCommand makeDocumentKindCommand(
    App::Document& document,
    App::DocumentCommandKind kind,
    int steps = 1);

/** Admit one Save, Undo, or Redo through DocumentHandle::trySubmit(). */
GuiExport App::DocumentCommandSubmitOutcome submitDocumentKindCommand(
    App::Document& document,
    App::DocumentCommandKind kind,
    int steps = 1);

/** Poll command status without waiting and notify the GUI document when terminal. */
GuiExport void scheduleUndoRedoCommandCompletion(
    const char* appDocumentName,
    App::DocumentRevisionIdentityBinding documentIdentity,
    App::DocumentCommandId commandId,
    App::DocumentCommandKind kind,
    std::shared_ptr<UndoRedoCompletionAnchor> anchor);

/** Report that grouped undo/redo across documents is not supported on this path. */
GuiExport void reportGroupedUndoRedoUnsupported(App::Document& document, bool undo);

/**
 * Admit save on the execution lane without waiting.
 *
 * Returns true only after the Save command reaches a terminal Succeeded state
 * observed by a prior completion schedule. Immediate trySubmit Accepted alone
 * schedules completion and returns false so callers do not claim the write
 * finished before the lane runs it.
 */
GuiExport bool submitDocumentSave(App::Document& document);

/**
 * Admit save on the execution lane and poll the GUI event loop until the Save
 * command reaches a terminal state. Returns true only on Completed. Does not
 * schedule the passive completion timer used by submitDocumentSave().
 */
GuiExport bool submitDocumentSaveAwaitingCompletion(
    App::Document& document,
    std::string* failureDiagnostic = nullptr);

/** Poll Save command status without waiting; update GUI modified state on terminal. */
GuiExport void scheduleSaveCommandCompletion(
    const char* appDocumentName,
    App::DocumentRevisionIdentityBinding documentIdentity,
    App::DocumentCommandId commandId);

/** Surface a visible Busy or admission failure reason without waiting. */
GuiExport void reportDocumentCommandSubmitBlocked(
    App::Document& document,
    const App::DocumentCommandSubmitOutcome& outcome,
    bool quiet = false);

/** Report that save was deferred because model work is still running. */
GuiExport void reportDocumentSaveDeferred(App::Document& document);

/** Report that save was admitted and is executing on the document lane. */
GuiExport void reportDocumentSaveAdmitted(App::Document& document);

/** Admit document close on the GUI thread without blocking on the lane. */
GuiExport bool submitDocumentClose(App::Document& document);

/** True when the document execution lane is running model work. */
GuiExport bool documentExecutionLaneBusy(const App::Document& document);

/**
 * True when GUI readers must use DocumentPresentationCache instead of live
 * App::DocumentObject / Property pointers (lane busy or post-commit sticky
 * presentation until idle live updateData).
 */
GuiExport bool shouldReadCommittedPresentation(const App::Document& document);

/** Committed presentation display string for one property, if published. */
GuiExport std::optional<std::string> committedPresentationPropertyDisplayValue(
    const App::Document& document,
    const App::DocumentObject& object,
    const char* propertyName);

/**
 * Prepare one document for save on the GUI thread without waiting.
 *
 * Submits a pre-save recompute when needed. Returns true only when no model
 * work is active and save may proceed immediately. Never waits.
 */
GuiExport bool prepareDocumentForImmediateSave(App::Document& document, bool skipRecomputeIfAlreadyFlagged);

/**
 * Admit one recompute through DocumentHandle::trySubmit().
 *
 * Returns the full admission outcome. Passive status refresh may continue on a
 * timer when Accepted; status() is observation-only and never pumps work.
 */
GuiExport App::DocumentCommandSubmitOutcome submitDocumentRecompute(
    App::Document& document,
    const DocumentRecomputeSubmitRequest& request = {});

/** Convenience wrapper that reports Busy and other rejections immediately. */
GuiExport bool requestDocumentRecompute(
    App::Document& document,
    const std::vector<App::DocumentObject*>& objects = {},
    bool force = false,
    int options = 0,
    bool quiet = false);

}  // namespace Gui
