// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace Gui
{

/** The state domain declared by one synchronous legacy GUI mutation. */
enum class CollaborationCompatibilityMutationKind
{
    Model,
    UnknownModel,
    SharedPresentation,
    PersonalCamera,
    PersonalSelection,
    PersonalTree,
    PersonalActiveView,
    PersonalContext
};

/**
 * Pointer-free declaration passed to the App-owned synchronous commit path.
 *
 * Model requires both objectName and stableObjectIdentity. UnknownModel and
 * SharedPresentation require both strings to be empty. SharedPresentation is
 * serialization-only and revision-neutral until Phase 6.
 */
struct GuiExport CollaborationCompatibilityMutationDeclaration
{
    CollaborationCompatibilityMutationKind kind {
        CollaborationCompatibilityMutationKind::UnknownModel};
    std::string objectName;
    std::string stableObjectIdentity;
};

enum class CollaborationCompatibilityMutationStatus
{
    Completed,
    RejectedPersonalContext,
    RejectedWrongThread,
    RejectedReentrant,
    InvalidDeclaration,
    MissingCommitDelegate,
    MissingMutationCallback,
    CommitRejected,
    CommitFailed
};

/** Complete value result produced by the synchronous App-owned commit callable. */
struct GuiExport CollaborationCompatibilityMutationOutcome
{
    CollaborationCompatibilityMutationStatus status {
        CollaborationCompatibilityMutationStatus::CommitRejected};
    std::string diagnostic;

    [[nodiscard]] bool completed() const noexcept
    {
        return status == CollaborationCompatibilityMutationStatus::Completed;
    }
};

using CollaborationCompatibilityMutationCallback = std::function<void()>;

/**
 * Integration-owned synchronous commit entry point.
 *
 * The App/Gui::Document integration owns document lifetime admission, the
 * recursive commit/recompute mutex, transaction/recompute/reservation,
 * rollback/poison handling, and revision publication. It receives the legacy
 * callback exactly once and returns the complete outcome. It must not retain
 * the declaration, callback, or any live GUI/App pointer after returning.
 */
using CollaborationCompatibilityCommit = std::function<
    CollaborationCompatibilityMutationOutcome(
        const CollaborationCompatibilityMutationDeclaration&,
        CollaborationCompatibilityMutationCallback&&)>;

/**
 * Validates and synchronously dispatches one already-declared legacy mutation.
 *
 * The adapter contains no document or GUI pointer and performs no queuing,
 * detached work, mutation, recompute, publication, or transaction handling.
 * Construct it on the document owner/GUI thread. Personal context operations
 * bypass this API and are rejected before the commit callable is entered.
 */
class GuiExport CollaborationCompatibilityAdapter
{
public:
    CollaborationCompatibilityAdapter();

    CollaborationCompatibilityAdapter(const CollaborationCompatibilityAdapter&) = delete;
    CollaborationCompatibilityAdapter& operator=(const CollaborationCompatibilityAdapter&) = delete;

    [[nodiscard]] CollaborationCompatibilityMutationOutcome execute(
        CollaborationCompatibilityMutationDeclaration declaration,
        CollaborationCompatibilityCommit commit,
        CollaborationCompatibilityMutationCallback callback);

    [[nodiscard]] static bool isPersonalContext(
        CollaborationCompatibilityMutationKind kind) noexcept;

    /** Empty means valid; otherwise contains a pointer-free diagnostic. */
    [[nodiscard]] static std::optional<std::string> validateDeclaration(
        const CollaborationCompatibilityMutationDeclaration& declaration);

private:
    const std::thread::id _ownerThread;
    bool _executing {false};
};

}  // namespace Gui
