// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborationCompatibilityAdapter.h"

#include <utility>

namespace Gui
{

namespace
{

class ExecuteScope final
{
public:
    explicit ExecuteScope(bool& executing)
        : _executing(executing)
    {
        _executing = true;
    }

    ~ExecuteScope()
    {
        _executing = false;
    }

private:
    bool& _executing;
};

}  // namespace

CollaborationCompatibilityAdapter::CollaborationCompatibilityAdapter()
    : _ownerThread(std::this_thread::get_id())
{}

bool CollaborationCompatibilityAdapter::isPersonalContext(
    CollaborationCompatibilityMutationKind kind) noexcept
{
    switch (kind) {
    case CollaborationCompatibilityMutationKind::PersonalCamera:
    case CollaborationCompatibilityMutationKind::PersonalSelection:
    case CollaborationCompatibilityMutationKind::PersonalTree:
    case CollaborationCompatibilityMutationKind::PersonalActiveView:
    case CollaborationCompatibilityMutationKind::PersonalContext:
        return true;
    case CollaborationCompatibilityMutationKind::Model:
    case CollaborationCompatibilityMutationKind::UnknownModel:
    case CollaborationCompatibilityMutationKind::SharedPresentation:
        return false;
    }

    return true;
}

std::optional<std::string> CollaborationCompatibilityAdapter::validateDeclaration(
    const CollaborationCompatibilityMutationDeclaration& declaration)
{
    switch (declaration.kind) {
    case CollaborationCompatibilityMutationKind::Model:
        if (declaration.objectName.empty()) {
            return "model compatibility mutation requires a nonempty object name";
        }
        if (declaration.stableObjectIdentity.empty()) {
            return "model compatibility mutation requires a nonempty stable object identity";
        }
        return std::nullopt;
    case CollaborationCompatibilityMutationKind::UnknownModel:
        if (!declaration.objectName.empty() || !declaration.stableObjectIdentity.empty()) {
            return "unknown-model compatibility mutation cannot declare object scope";
        }
        return std::nullopt;
    case CollaborationCompatibilityMutationKind::SharedPresentation:
        if (!declaration.objectName.empty() || !declaration.stableObjectIdentity.empty()) {
            return "shared-presentation compatibility mutation cannot declare model scope";
        }
        return std::nullopt;
    case CollaborationCompatibilityMutationKind::PersonalCamera:
    case CollaborationCompatibilityMutationKind::PersonalSelection:
    case CollaborationCompatibilityMutationKind::PersonalTree:
    case CollaborationCompatibilityMutationKind::PersonalActiveView:
    case CollaborationCompatibilityMutationKind::PersonalContext:
        return std::nullopt;
    }

    return "unknown compatibility mutation kind";
}

CollaborationCompatibilityMutationOutcome CollaborationCompatibilityAdapter::execute(
    CollaborationCompatibilityMutationDeclaration declaration,
    CollaborationCompatibilityCommit commit,
    CollaborationCompatibilityMutationCallback callback)
{
    if (const auto invalid = validateDeclaration(declaration)) {
        return {CollaborationCompatibilityMutationStatus::InvalidDeclaration, *invalid};
    }
    if (isPersonalContext(declaration.kind)) {
        return {CollaborationCompatibilityMutationStatus::RejectedPersonalContext,
                "personal GUI context is revision-neutral and bypasses compatibility mutation"};
    }
    if (std::this_thread::get_id() != _ownerThread) {
        return {CollaborationCompatibilityMutationStatus::RejectedWrongThread,
                "compatibility mutation must run on its document owner/GUI thread"};
    }
    if (_executing) {
        return {CollaborationCompatibilityMutationStatus::RejectedReentrant,
                "nested compatibility mutation is not admitted"};
    }
    if (!commit) {
        return {CollaborationCompatibilityMutationStatus::MissingCommitDelegate,
                "compatibility mutation requires a synchronous commit delegate"};
    }
    if (!callback) {
        return {CollaborationCompatibilityMutationStatus::MissingMutationCallback,
                "compatibility mutation requires a legacy mutation callback"};
    }

    ExecuteScope scope(_executing);
    // Intentionally do not catch: the integration-owned commit callable owns
    // the complete failure contract, and its exception must reach the caller.
    return commit(declaration, std::move(callback));
}

}  // namespace Gui
