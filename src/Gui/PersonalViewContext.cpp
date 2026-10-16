// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PersonalViewContext.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Gui
{

namespace
{

std::mutex& sharedRendererMutex()
{
    static std::mutex mutex;
    return mutex;
}

thread_local bool rendererInvocationActiveOnThisThread = false;

class RendererInvocationGuard
{
public:
    RendererInvocationGuard()
    {
        rendererInvocationActiveOnThisThread = true;
    }

    ~RendererInvocationGuard()
    {
        rendererInvocationActiveOnThisThread = false;
    }

    RendererInvocationGuard(const RendererInvocationGuard&) = delete;
    RendererInvocationGuard& operator=(const RendererInvocationGuard&) = delete;
};

}  // namespace

bool PersonalViewOverlay::operator==(const PersonalViewOverlay& other) const noexcept
{
    return identifier == other.identifier && kind == other.kind && payload == other.payload;
}

bool PersonalViewContext::operator==(const PersonalViewContext& other) const noexcept
{
    return camera == other.camera && projection == other.projection
        && selectionPaths == other.selectionPaths
        && preselectionPath == other.preselectionPath
        && expandedTreePaths == other.expandedTreePaths
        && treeHorizontalScroll == other.treeHorizontalScroll
        && treeVerticalScroll == other.treeVerticalScroll
        && activeDocument == other.activeDocument && activeView == other.activeView
        && activeWorkbench == other.activeWorkbench && editFocus == other.editFocus
        && temporaryOverlays == other.temporaryOverlays;
}

void PersonalViewContextStore::requireActorId(const std::string& actorId)
{
    if (actorId.empty()) {
        throw std::invalid_argument("personal view actor ID must not be empty");
    }
}

PersonalViewContextStore::StoredContext PersonalViewContextStore::bindSelections(
    PersonalViewContext context, const SelectionResolver& resolveSelection)
{
    StoredContext stored {std::move(context), {}};
    if (resolveSelection) {
        for (const auto& path : stored.value.selectionPaths) {
            stored.selections.emplace(path, resolveSelection(stored.value, path));
        }
        if (stored.value.preselectionPath) {
            const auto& path = *stored.value.preselectionPath;
            stored.selections.emplace(path, resolveSelection(stored.value, path));
        }
    }
    return stored;
}

void PersonalViewContextStore::store(std::string actorId, PersonalViewContext context,
                                    const SelectionResolver& resolveSelection)
{
    requireActorId(actorId);
    std::lock_guard<std::mutex> guard(_contextsMutex);
    _contexts.insert_or_assign(std::move(actorId),
                               bindSelections(std::move(context), resolveSelection));
}

bool PersonalViewContextStore::update(std::string actorId, PersonalViewContext context,
                                     const SelectionResolver& resolveSelection)
{
    requireActorId(actorId);
    std::lock_guard<std::mutex> guard(_contextsMutex);
    const auto found = _contexts.find(actorId);
    if (found == _contexts.end()) {
        return false;
    }

    found->second = bindSelections(std::move(context), resolveSelection);
    return true;
}

std::optional<PersonalViewContext> PersonalViewContextStore::snapshot(
    const std::string& actorId) const
{
    requireActorId(actorId);
    std::lock_guard<std::mutex> guard(_contextsMutex);
    const auto found = _contexts.find(actorId);
    if (found == _contexts.end()) {
        return std::nullopt;
    }
    return found->second.value;
}

bool PersonalViewContextStore::remove(const std::string& actorId)
{
    requireActorId(actorId);
    std::lock_guard<std::mutex> guard(_contextsMutex);
    return _contexts.erase(actorId) != 0;
}

bool PersonalViewContextStore::contains(const std::string& actorId) const
{
    requireActorId(actorId);
    std::lock_guard<std::mutex> guard(_contextsMutex);
    return _contexts.find(actorId) != _contexts.end();
}

void PersonalViewContextStore::retireObjectSelection(std::uintptr_t objectIdentity)
{
    if (!objectIdentity) {
        return;
    }
    std::lock_guard<std::mutex> guard(_contextsMutex);
    for (auto& [actor, stored] : _contexts) {
        const auto targetsObject = [&](const std::string& path) {
            const auto found = stored.selections.find(path);
            return found != stored.selections.end()
                && (found->second.first == objectIdentity
                    || found->second.second == objectIdentity);
        };
        std::erase_if(stored.value.selectionPaths, targetsObject);
        if (stored.value.preselectionPath && targetsObject(*stored.value.preselectionPath)) {
            stored.value.preselectionPath.reset();
        }
        std::erase_if(stored.selections, [&](const auto& entry) {
            return entry.second.first == objectIdentity || entry.second.second == objectIdentity;
        });
    }
}

std::size_t PersonalViewContextStore::actorCount() const
{
    std::lock_guard<std::mutex> guard(_contextsMutex);
    return _contexts.size();
}

PersonalViewRenderStatus PersonalViewContextStore::applyAndRender(
    const std::string& actorId,
    const PersonalViewRendererCallbacks& renderer) const
{
    requireActorId(actorId);
    if (!renderer.captureState || !renderer.applyState || !renderer.render
        || !renderer.restoreState) {
        throw std::invalid_argument("personal view renderer callbacks must all be provided");
    }
    if (rendererInvocationActiveOnThisThread) {
        throw std::logic_error("personal view rendering must not be invoked recursively");
    }

    const auto requested = snapshot(actorId);
    if (!requested) {
        return PersonalViewRenderStatus::ActorNotFound;
    }

    std::lock_guard<std::mutex> rendererGuard(sharedRendererMutex());
    RendererInvocationGuard invocationGuard;
    const PersonalViewContext prior = renderer.captureState();

    try {
        renderer.applyState(*requested);
        renderer.render();
    }
    catch (...) {
        renderer.restoreState(prior);
        throw;
    }

    renderer.restoreState(prior);
    return PersonalViewRenderStatus::Rendered;
}

}  // namespace Gui
