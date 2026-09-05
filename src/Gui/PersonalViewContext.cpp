// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PersonalViewContext.h"

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

void PersonalViewContextStore::store(std::string actorId, PersonalViewContext context)
{
    requireActorId(actorId);
    std::lock_guard<std::mutex> guard(_contextsMutex);
    _contexts.insert_or_assign(std::move(actorId), std::move(context));
}

bool PersonalViewContextStore::update(std::string actorId, PersonalViewContext context)
{
    requireActorId(actorId);
    std::lock_guard<std::mutex> guard(_contextsMutex);
    const auto found = _contexts.find(actorId);
    if (found == _contexts.end()) {
        return false;
    }

    found->second = std::move(context);
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
    return found->second;
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
