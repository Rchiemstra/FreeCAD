// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Gui
{

/** Pointer-free description of one temporary renderer overlay. */
struct GuiExport PersonalViewOverlay
{
    std::string identifier;
    std::string kind;
    std::string payload;

    [[nodiscard]] bool operator==(const PersonalViewOverlay& other) const noexcept;
    [[nodiscard]] bool operator!=(const PersonalViewOverlay& other) const noexcept
    {
        return !(*this == other);
    }
};

/**
 * Complete copied personal GUI state for one actor.
 *
 * Paths and renderer data are deliberately opaque UTF-8 values at this
 * boundary.  The integration layer is responsible for translating them to
 * and from live GUI objects synchronously.  This type contains no GUI/App
 * pointer, revision, dirty flag, or persistence state.
 */
struct GuiExport PersonalViewContext
{
    std::string camera;
    std::string projection;

    std::vector<std::string> selectionPaths;
    std::optional<std::string> preselectionPath;

    std::vector<std::string> expandedTreePaths;
    std::int64_t treeHorizontalScroll {0};
    std::int64_t treeVerticalScroll {0};

    std::string activeDocument;
    std::string activeView;
    std::string activeWorkbench;
    std::string editFocus;

    std::vector<PersonalViewOverlay> temporaryOverlays;

    [[nodiscard]] bool operator==(const PersonalViewContext& other) const noexcept;
    [[nodiscard]] bool operator!=(const PersonalViewContext& other) const noexcept
    {
        return !(*this == other);
    }
};

/**
 * Type-erased restoration callable with a statically enforced no-throw boundary.
 *
 * A renderer integration must supply a callable that completely restores the
 * provided state before returning and must not construct this from a
 * potentially throwing callable.  This makes restoration distinct from
 * ordinary state application and gives applyAndRender() the strong restoration
 * guarantee on exceptions.
 */
class GuiExport PersonalViewRestoreCallback
{
public:
    PersonalViewRestoreCallback() = default;
    PersonalViewRestoreCallback(const PersonalViewRestoreCallback&) = default;
    PersonalViewRestoreCallback(PersonalViewRestoreCallback&&) noexcept = default;
    PersonalViewRestoreCallback& operator=(const PersonalViewRestoreCallback&) = default;
    PersonalViewRestoreCallback& operator=(PersonalViewRestoreCallback&&) noexcept = default;

    template<
        typename Callback,
        std::enable_if_t<
            !std::is_same_v<std::decay_t<Callback>, PersonalViewRestoreCallback>
                && std::is_nothrow_invocable_r_v<
                    void,
                    std::decay_t<Callback>&,
                    const PersonalViewContext&>,
            int> = 0>
    PersonalViewRestoreCallback(Callback&& callback)
        : _callback(std::forward<Callback>(callback))
    {}

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(_callback);
    }

    void operator()(const PersonalViewContext& context) const noexcept
    {
        _callback(context);
    }

private:
    std::function<void(const PersonalViewContext&)> _callback;
};

/**
 * Synchronous value/callback boundary for the one shared renderer.
 *
 * The callbacks may refer to live GUI objects, but the store never retains
 * them or a context reference after applyAndRender() returns.  captureState
 * must return a complete value snapshot.  applyState and restoreState must
 * consume the value synchronously and must not retain its reference.
 */
struct GuiExport PersonalViewRendererCallbacks
{
    std::function<PersonalViewContext()> captureState;
    std::function<void(const PersonalViewContext&)> applyState;
    std::function<void()> render;
    PersonalViewRestoreCallback restoreState;
};

enum class PersonalViewRenderStatus
{
    Rendered,
    ActorNotFound
};

/**
 * Thread-safe actor-scoped storage for personal view contexts.
 *
 * Context access and renderer access use independent mutexes.  An in-flight
 * render therefore owns a private context copy: concurrent actor updates are
 * allowed, cannot alter that copy, and cannot alter another actor's context.
 * No process-global selection or other GUI singleton is used as storage.
 */
class GuiExport PersonalViewContextStore
{
public:
    PersonalViewContextStore() = default;

    PersonalViewContextStore(const PersonalViewContextStore&) = delete;
    PersonalViewContextStore& operator=(const PersonalViewContextStore&) = delete;

    /** Insert or replace actorId with an exact value copy. */
    void store(std::string actorId, PersonalViewContext context);

    /** Replace an existing actor value; return false without inserting if absent. */
    [[nodiscard]] bool update(std::string actorId, PersonalViewContext context);

    /** Return an independent value copy, or nullopt when actorId is absent. */
    [[nodiscard]] std::optional<PersonalViewContext> snapshot(
        const std::string& actorId) const;

    /** Remove actorId; return false when it was already absent. */
    [[nodiscard]] bool remove(const std::string& actorId);

    [[nodiscard]] bool contains(const std::string& actorId) const;
    [[nodiscard]] std::size_t actorCount() const;

    /**
     * Render one stable actor snapshot, restoring the prior renderer state.
     *
     * capture/apply/render/restore form one renderer-mutex-serialized action.
     * A callback that recursively enters this operation on the same thread is
     * rejected with std::logic_error before attempting the renderer mutex.
     * The dedicated no-throw restore callback restores the exact prior state
     * after success and after every apply/render exception.  An apply/render
     * exception remains the primary exception and is rethrown unchanged.
     */
    [[nodiscard]] PersonalViewRenderStatus applyAndRender(
        const std::string& actorId,
        const PersonalViewRendererCallbacks& renderer) const;

private:
    static void requireActorId(const std::string& actorId);

    mutable std::mutex _contextsMutex;
    std::unordered_map<std::string, PersonalViewContext> _contexts;
    // The renderer mutex is process-wide in the implementation because all
    // stores project onto the same shared GUI renderer.
};

}  // namespace Gui
