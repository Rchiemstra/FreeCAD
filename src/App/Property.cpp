// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2002 Jürgen Riegel <juergen.riegel@web.de>              *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include <cassert>
#include <algorithm>
#include <array>
#include <exception>
#include <iterator>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <thread>
#include <vector>
#include <chrono>
#include <future>

#include <atomic>
#include <Base/Tools.h>
#include <Base/Interpreter.h>
#include <Base/Writer.h>
#include <CXX/Objects.hxx>

#include "Property.h"
#include "PropertyCleanup.h"
#include "PropertyTouchPublication.h"
#include "Document.h"
#include "DocumentWouldBlock.h"
#include "MainThreadSignal.h"
#include "private/CollaborationStructuralMutationRecorder.h"
#include "ObjectIdentifier.h"
#include "PropertyContainer.h"
#include "DocumentObject.h"
#include "MutationClassification.h"
#include "PropertyLinks.h"
#include "PropertyStandard.h"

using namespace App;

namespace
{

thread_local const Property* suppressedTouchPublication = nullptr;

// Touch is the only event PropertyTouchPublicationSuppressor may skip.
// Value (including purgeTouched) and status stay publishable for that
// same property. Classification is unchanged: status is structural.
enum class PropertyPublication
{
    Touch,
    Value,
    Status,
};

void publishPropertyMutation(Property& property, PropertyPublication publication)
{
    const bool structural = publication == PropertyPublication::Status;
    auto* container = property.getContainer();
    auto* document = documentFromPropertyContainer(container);
    if (!document) {
        return;
    }

    MutationClassificationInput input;
    input.source = structural ? CollaborationMutationSource::PropertyStatus
                              : CollaborationMutationSource::PropertyValue;
    input.mutationKind = structural ? MutationKind::StructuralProperty
                                    : MutationKind::PropertyWrite;
    input.propertyName = property.getName();
    input.propertyFamily = CollaborationPropertyFamily::NotApplicable;
    if (!structural) {
        if (property.isDerivedFrom(PropertyLinkBase::getClassTypeId())) {
            input.propertyFamily = CollaborationPropertyFamily::Link;
        }
        else {
            const auto type = property.getTypeId();
            const bool exactModelValue = type == PropertyBool::getClassTypeId()
                || type == PropertyInteger::getClassTypeId()
                || type == PropertyFloat::getClassTypeId()
                || type == PropertyString::getClassTypeId();
            input.propertyFamily = exactModelValue ? CollaborationPropertyFamily::ModelValue
                                                   : CollaborationPropertyFamily::Unknown;
        }
    }

    if (const auto* object = dynamic_cast<const DocumentObject*>(container)) {
        // Undo retains removed objects with their former Document pointer so
        // they can be restored.  Mutating such detached storage is not a live
        // document mutation and has neither a document name nor a revision
        // identity to publish.
        if (!object->isAttachedToDocument() || object->getDocument() != document
            || !document->containsObject(object)) {
            return;
        }
        input.containerKind = CollaborationContainerKind::DocumentObject;
        input.objectName = object->getNameInDocument();
        input.stableObjectIdentity = document->collaborationObjectIdentity(*object);
    }
    else if (dynamic_cast<const Document*>(container)) {
        input.containerKind = CollaborationContainerKind::Document;
    }
    const auto effects = classifyMutation(input);
    document->recordCollaborationAtomicPresentationEffects(effects, &property);
    if (structural || input.propertyFamily == CollaborationPropertyFamily::Link) {
        Internal::CollaborationStructuralMutationRecorder::record(*document, effects);
    }
    // Audit above still records the mutation. Only the synthetic touch
    // is unpublished; document-level suppression still covers every caller.
    if ((publication == PropertyPublication::Touch && suppressedTouchPublication == &property)
        || document->collaborationRevisionPublicationSuppressed(&property)) {
        return;
    }
    static_cast<void>(document->collaborationRevisions().publish(effects));
}

}  // namespace

PropertyTouchPublicationSuppressor::PropertyTouchPublicationSuppressor(
    const Property& property) noexcept
    : _previous(suppressedTouchPublication)
{
    suppressedTouchPublication = &property;
}

PropertyTouchPublicationSuppressor::~PropertyTouchPublicationSuppressor()
{
    suppressedTouchPublication = _previous;
}

namespace
{

constexpr unsigned long serializedPropertyStatusMask()
{
    return (1UL << Property::ReadOnly) | (1UL << Property::Hidden)
        | (1UL << Property::Transient) | (1UL << Property::Output)
        | (1UL << Property::LockDynamic) | (1UL << Property::Ordered)
        | (1UL << Property::EvalOnRestore) | (1UL << Property::CopyOnChange)
        | (1UL << Property::UserEdit);
}

void capturePersistedStaticStatus(Property& property,
                                  Document& document,
                                  unsigned long oldStatus,
                                  unsigned long newStatus)
{
    if (property.testStatus(Property::PropDynamic)) {
        return;
    }
    if (((oldStatus ^ newStatus) & serializedPropertyStatusMask()) == 0) {
        return;
    }
    auto* object = dynamic_cast<DocumentObject*>(property.getContainer());
    if (!object || object->getDocument() != &document) {
        return;
    }
    document.captureActiveTransactionProperty(object, &property);
}

}  // namespace


//**************************************************************************
//**************************************************************************
// Property
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE_ABSTRACT(App::Property, Base::Persistence)

//**************************************************************************
// Construction/Destruction

static std::atomic<int64_t> _PropID;

// Here is the implementation! Description should take place in the header file!
Property::Property()
    : _id(++_PropID)
{}

Property::~Property() = default;

const char* Property::getName() const
{
    return myName ? myName : "";
}

bool Property::hasName() const
{
    return isValidName(myName);
}

bool Property::isValidName(const char* name)
{
    return !Base::Tools::isNullOrEmpty(name);
}

std::string Property::getFullName() const
{
    std::string name;
    if (myName) {
        if (father) {
            name = father->getFullName() + ".";
        }
        else {
            name = "?.";
        }
        name += myName;
    }
    else {
        return "?";
    }
    return name;
}

std::string Property::getFileName(const char* postfix, const char* prefix) const
{
    std::ostringstream ss;
    if (prefix) {
        ss << prefix;
    }
    if (!myName) {
        ss << "Property";
    }
    else {
        std::string name = getFullName();
        auto pos = name.find('#');
        if (pos == std::string::npos) {
            ss << name;
        }
        else {
            ss << (name.c_str() + pos + 1);
        }
    }
    if (postfix) {
        ss << postfix;
    }
    return ss.str();
}

// clang-format off
static constexpr auto mapProps = std::to_array<std::tuple<Property::Status, PropertyType>>({
    {App::Property::PropReadOnly,    Prop_ReadOnly},
    {App::Property::PropHidden,      Prop_Hidden},
    {App::Property::PropInput,       Prop_Input},
    {App::Property::PropOutput,      Prop_Output},
    {App::Property::PropTransient,   Prop_Transient},
    {App::Property::PropNoRecompute, Prop_NoRecompute},
    {App::Property::PropNoPersist,   Prop_NoPersist}
});
// clang-format on

short Property::getType() const
{
    short type = 0;
    for (const auto& [propertyStatus, propertyType] : mapProps) {
        if (testStatus(propertyStatus)) {
            type |= propertyType;
        }
    }
    return type;
}

void Property::syncType(unsigned type)
{
    for (const auto& [propertyStatus, propertyType] : mapProps) {
        if (type & propertyType) {
            StatusBits.set((size_t)propertyStatus);
        }
    }
}

const char* Property::getGroup() const
{
    return father->getPropertyGroup(this);
}

const char* Property::getDocumentation() const
{
    return father->getPropertyDocumentation(this);
}

void Property::setContainer(PropertyContainer* father)
{
    this->father = father;
}

void Property::setPathValue(const ObjectIdentifier& path, const boost::any& value)
{
    path.setValue(value);
}

const boost::any Property::getPathValue(const ObjectIdentifier& path) const
{
    return path.getValue();
}

void Property::getPaths(std::vector<ObjectIdentifier>& paths) const
{
    paths.emplace_back(getContainer(), getName());
}

void Property::getPathsForCompletion(
    std::vector<ObjectIdentifier>& paths,
    const std::string&
) const
{
    getPaths(paths);
}

ObjectIdentifier Property::canonicalPath(const ObjectIdentifier& p) const
{
    return p;
}

namespace App
{
/*!
 * \brief The PropertyCleaner struct
 * Make deleting dynamic property safer by postponing its destruction.
 *
 * Dynamic property can be removed at any time, even during triggering of
 * onChanged() signal of the removing property. This patch introduced
 * static function Property::destroy() to make it safer by queueing any
 * removed property, and only deleting them when no onChanged() call is
 * active.
 */
struct PropertyCleaner
{
    struct Entry {
        Property* property;
        PropertyCleanupBoundary* boundary;
    };
    struct State {
        std::vector<Entry> removed;
        std::atomic<unsigned> active {0};
        std::atomic<bool> sealed {false};
        int guards {0}; // Only the owner thread reads or writes this.
        bool managed {false};
        std::shared_future<void> ownerCompletion;
        std::thread::id owner {std::this_thread::get_id()};
    };
    explicit PropertyCleaner(Property* p);
    ~PropertyCleaner();
    static State& state();
    static void beginUse(State& state);
    static void endUse(State& state) noexcept;
    static void add(Property* prop);
    Property* prop;
    State* owner;
};
}  // namespace App

namespace {
using CleanerState = App::PropertyCleaner::State;
struct CleanerRegistry;
void reapExitedCleanerStates(CleanerRegistry& registry) noexcept;
struct CleanerRegistry {
    CleanerRegistry(): reaper([this] { reapExitedCleanerStates(*this); }) {}
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<CleanerState>> states;
    std::size_t drains {0};
    bool closing {false};
    bool stopReaper {false};
    std::thread reaper;
};
CleanerRegistry& cleanerRegistry()
{
    static auto* registry = new CleanerRegistry;
    return *registry;
}
thread_local bool managedCleanerThread = false;
thread_local bool cleanerDrainAuthority = false;
thread_local bool insideExitDrain = false;
thread_local CleanerState* cleanerLocalState = nullptr;
thread_local CleanerState* cleanerDrainState = nullptr;
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
std::atomic<void (*)()> cleanupDrainWaitHook {nullptr};
#endif

void drainState(CleanerState& state) noexcept
{
    PyObject* errorType = nullptr;
    PyObject* errorValue = nullptr;
    PyObject* errorTraceback = nullptr;
    const bool preserveError = Py_IsInitialized() && PyGILState_Check();
    if (preserveError) PyErr_Fetch(&errorType, &errorValue, &errorTraceback);
    while (!state.removed.empty()) {
        auto* property = state.removed.back().property;
        state.removed.pop_back();
        property->setContainer(nullptr);
        delete property;
    }
    if (preserveError) {
        PyErr_Clear();
        PyErr_Restore(errorType, errorValue, errorTraceback);
    }
}

struct CleanerThreadHolder {
    ~CleanerThreadHolder() noexcept
    {
        auto* state = cleanerLocalState;
        if (!state) return;
        auto& registry = cleanerRegistry();
        {
            std::lock_guard lock(registry.mutex);
            // The registry retains the queue until true thread completion.
            if (registry.closing) return;
            ++registry.drains;
        }
        if (state->active.load(std::memory_order_acquire) != 0) std::terminate();
        const bool previous = cleanerDrainAuthority;
        cleanerDrainAuthority = true;
        insideExitDrain = true;
        drainState(*state);
        insideExitDrain = false;
        cleanerDrainAuthority = previous;
        {
            std::lock_guard lock(registry.mutex);
            --registry.drains;
        }
        registry.changed.notify_all();
    }
};
thread_local CleanerThreadHolder cleanerThread;

void reapExitedCleanerStates(CleanerRegistry& registry) noexcept
{
    std::unique_lock lock(registry.mutex);
    while (!registry.stopReaper) {
        auto found = registry.states.end();
        if (!registry.closing) {
            found = std::find_if(registry.states.begin(), registry.states.end(), [](const auto& state) {
                return state->ownerCompletion.wait_for(std::chrono::seconds(0))
                    == std::future_status::ready;
            });
        }
        if (found == registry.states.end()) {
            registry.changed.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }
        auto exited = *found;
        registry.states.erase(found);
        ++registry.drains;
        lock.unlock();
        if (exited->active.load() != 0) std::terminate();
        cleanerDrainState = exited.get();
        cleanerDrainAuthority = true;
        insideExitDrain = true;
        drainState(*exited);
        insideExitDrain = false;
        cleanerDrainAuthority = false;
        cleanerDrainState = nullptr;
        lock.lock();
        --registry.drains;
        registry.changed.notify_all();
    }
}
}  // namespace

PropertyCleaner::State& PropertyCleaner::state()
{
    if (cleanerDrainState) return *cleanerDrainState;
    if (!cleanerLocalState) {
        // The holder drains opportunistically, but state access itself uses a
        // trivially destructible pointer retained until physical thread exit.
        (void)&cleanerThread;
        auto state = std::make_shared<State>();
        state->managed = managedCleanerThread;
        std::promise<void> completion;
        state->ownerCompletion = completion.get_future().share();
        completion.set_value_at_thread_exit();
        auto& registry = cleanerRegistry();
        std::lock_guard lock(registry.mutex);
        // No new state may enter while finalization owns the registry. This
        // includes public pools constructed reentrantly by a finalizer.
        if (registry.closing)
            throw Base::RuntimeError("property cleanup is closing");
        registry.states.push_back(state);
        cleanerLocalState = state.get();
        registry.changed.notify_all();
    }
    return *cleanerLocalState;
}

void PropertyCleaner::beginUse(State& state)
{
    if (state.sealed.load() && !cleanerDrainAuthority)
        throw Base::RuntimeError("property cleanup is closing");
    state.active.fetch_add(1);
    if (state.sealed.load() && !cleanerDrainAuthority) {
        state.active.fetch_sub(1);
        throw Base::RuntimeError("property cleanup is closing");
    }
}
void PropertyCleaner::endUse(State& state) noexcept
{ state.active.fetch_sub(1); }

PropertyCleaner::PropertyCleaner(Property* p): prop(p), owner(&state())
{
    beginUse(*owner);
    ++owner->guards;
}
PropertyCleaner::~PropertyCleaner()
{
    if (--owner->guards == 0) {
        bool found = false;
        Entry retained {nullptr, nullptr};
        while (!owner->removed.empty()) {
            auto entry = owner->removed.back();
            owner->removed.pop_back();
            if (entry.property != prop) {
                entry.property->setContainer(nullptr);
                delete entry.property;
            }
            else { found = true; retained = entry; }
        }
        if (found) owner->removed.push_back(retained);
    }
    endUse(*owner);
}
void PropertyCleaner::add(Property* property)
{
    auto& s = state();
    beginUse(s);
    struct Done { State& s; ~Done() { PropertyCleaner::endUse(s); } } done {s};
    s.removed.push_back({property, PropertyCleanupBoundary::current});
}

thread_local PropertyCleanupBoundary* PropertyCleanupBoundary::current = nullptr;

bool App::propertyCleanupIsQuiescent() noexcept
{
    return !insideExitDrain
        && (!(cleanerDrainState ? cleanerDrainState : cleanerLocalState)
            || (cleanerDrainState ? cleanerDrainState : cleanerLocalState)->active.load() == 0)
        && !PropertyCleanupBoundary::current;
}

PropertyCleanupBoundary::PropertyCleanupBoundary() : previous(current)
{
    PropertyCleaner::beginUse(PropertyCleaner::state());
    current = this;
}

PropertyCleanupBoundary::~PropertyCleanupBoundary() noexcept
{
    auto& state = PropertyCleaner::state();
    auto& entries = state.removed;
    if (state.guards == 0) {
        // Remove one matching entry before deletion: finalizers can re-enter
        // and modify the same queue, or retire another property in this job.
        while (true) {
            auto it = std::find_if(entries.rbegin(), entries.rend(),
                                   [this](const auto& e) { return e.boundary == this; });
            if (it == entries.rend()) break;
            auto* property = it->property;
            entries.erase(std::next(it).base());
            property->setContainer(nullptr);
            delete property;
        }
    }
    // A nested boundary cannot drain through an active BASE guard.
    for (auto& entry : entries) if (entry.boundary == this) entry.boundary = previous;
    current = previous;
    PropertyCleaner::endUse(state);
}

void App::drainRetiredPropertiesAtQuiescence() noexcept
{
    if (!propertyCleanupIsQuiescent()) std::terminate();
    if (cleanerLocalState) drainState(*cleanerLocalState);
}

void App::initializeManagedPropertyCleanup(bool applicationOwned)
{
    managedCleanerThread = applicationOwned;
    // Construct the holder before application TLS which may retire values.
    (void)PropertyCleaner::state();
}

bool App::preparePropertyCleanupFinalization()
{
    auto& self = PropertyCleaner::state();
    auto& registry = cleanerRegistry();
    std::lock_guard lock(registry.mutex);
    if (registry.closing) return false;
    // A setter may be between aboutToSetValue and hasSetValue with no guard.
    // Never seal a live foreign state even temporarily: its setter could have
    // already stored a value before encountering that seal. A future marked
    // ready means this owner's C++ thread-storage destructors have completed.
    // Only application-owned workers are stopped and joined by shutdown.
    for (const auto& state : registry.states) {
        if (!state->managed && state.get() != &self
            && (state->active.load() != 0
                || state->ownerCompletion.wait_for(std::chrono::seconds(0))
                    != std::future_status::ready)) {
            return false;
        }
    }
    registry.closing = true;
    return true;
}

void App::cancelPropertyCleanupFinalization() noexcept
{
    auto& registry = cleanerRegistry();
    // While admission remains closed, claim only queues whose owner's TLS
    // completion has been signalled. Never hold the mutex across deletion.
    while (true) {
        std::shared_ptr<CleanerState> exited;
        {
            std::lock_guard lock(registry.mutex);
            const auto found = std::find_if(registry.states.begin(), registry.states.end(),
                [](const auto& state) {
                    return state->ownerCompletion.wait_for(std::chrono::seconds(0))
                        == std::future_status::ready;
                });
            if (found == registry.states.end()) {
                for (const auto& state : registry.states) state->sealed.store(false);
                registry.closing = false;
                registry.changed.notify_all();
                break;
            }
            exited = *found;
            std::erase(registry.states, exited);
        }
        if (exited->active.load() != 0) std::terminate();
        const bool previousAuthority = cleanerDrainAuthority;
        const bool previousExitDrain = insideExitDrain;
        cleanerDrainAuthority = true;
        insideExitDrain = true;
        drainState(*exited);
        insideExitDrain = previousExitDrain;
        cleanerDrainAuthority = previousAuthority;
    }
    cleanerDrainAuthority = false;
}

void App::waitForPropertyCleanupDrains()
{
    auto& registry = cleanerRegistry();
    std::optional<Base::PyGILStateRelease> release;
    if (Py_IsInitialized() && PyGILState_Check()) release.emplace();
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
    if (auto hook = cleanupDrainWaitHook.load()) hook();
#endif
    std::unique_lock lock(registry.mutex);
    while (registry.drains != 0) {
        registry.changed.wait_for(lock, std::chrono::milliseconds(2));
        if (registry.drains != 0 && DocumentWouldBlock::isGuiThread()) {
            lock.unlock();
            MainThreadSignalConfig::serviceMarshalledTasks(std::chrono::milliseconds(2));
            lock.lock();
        }
    }
}

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
void App::Internal::setCleanupDrainWaitHookForTesting(void (*hook)()) noexcept
{
    cleanupDrainWaitHook.store(hook);
}
#endif

bool App::finishPropertyCleanupFinalization()
{
    auto& registry = cleanerRegistry();
    {
        std::optional<Base::PyGILStateRelease> release;
        if (Py_IsInitialized() && PyGILState_Check()) release.emplace();
        // Lane exit and cleaner completion are separate thread-exit futures;
        // neither future's readiness implies the other is already ready.
        // All managed owners here are application-owned and were asked to stop.
        for (const auto& state : registry.states) {
            if (!state->managed || state.get() == cleanerLocalState) continue;
            while (state->ownerCompletion.wait_for(std::chrono::milliseconds(2))
                   != std::future_status::ready) {
                if (DocumentWouldBlock::isGuiThread())
                    MainThreadSignalConfig::serviceMarshalledTasks(std::chrono::milliseconds(2));
            }
        }
        std::unique_lock lock(registry.mutex);
        registry.changed.wait(lock, [&] { return registry.drains == 0; });
        // Seal before inspecting use counts, including separately owned
        // managed workers that are not part of the Application pool.
        for (const auto& state : registry.states) state->sealed.store(true);
        for (const auto& state : registry.states) {
            if (state->active.load() != 0) return false;
        }
    }
    cleanerDrainAuthority = true;
    bool pending;
    do {
        pending = false;
        // closing prevents registry mutation; retained shared states remain
        // available for cancellation until shutdown is committed.
        for (const auto& state : registry.states) {
            if (!state->removed.empty()) { pending = true; drainState(*state); }
        }
    } while (pending);
    return true;
}

void App::stopPropertyCleanupReaper() noexcept
{
    auto& registry = cleanerRegistry();
    {
        std::lock_guard lock(registry.mutex);
        registry.stopReaper = true;
    }
    registry.changed.notify_all();
    std::optional<Base::PyGILStateRelease> release;
    if (Py_IsInitialized() && PyGILState_Check()) release.emplace();
    if (registry.reaper.joinable()) registry.reaper.join();
}

void Property::destroy(Property* p)
{
    if (p) {
        // Is it necessary to nullify the container? May cause crash if any
        // onChanged() caller assumes a non-null container.
        //
        // p->setContainer(0);

        PropertyCleaner::add(p);
    }
}

bool Property::enableNotify(bool enable)
{
    bool isNotify = isNotifyEnabled();

    if (enable) {
        StatusBits.reset(DisableNotify);
    }
    else {
        StatusBits.set(DisableNotify);
    }
    return isNotify;
}

bool Property::isNotifyEnabled() const
{
    return !StatusBits.test(DisableNotify);
}

void Property::touch()
{
    PropertyCleaner guard(this);
    if (father) {
        if (Document* doc = documentFromPropertyContainer(father)) {
            enforceAtomicPresentationMutationTarget(doc);
        }
    }
    StatusBits.set(Touched);
    publishPropertyMutation(*this, PropertyPublication::Touch);
    if (father && isNotifyEnabled()) {
        father->onEarlyChange(this);
        father->onChanged(this);
    }
}

void Property::purgeTouched()
{
    if (!isTouched()) {
        return;
    }
    if (father) {
        if (Document* doc = documentFromPropertyContainer(father)) {
            enforceAtomicPresentationMutationTarget(doc);
        }
    }
    StatusBits.reset(Touched);
    publishPropertyMutation(*this, PropertyPublication::Value);
}

void Property::setReadOnly(bool readOnly)
{
    this->setStatus(App::Property::ReadOnly, readOnly);
}

void Property::hasSetValue()
{
    PropertyCleaner guard(this);
    publishPropertyMutation(*this, PropertyPublication::Value);
    if (father) {
        if (isNotifyEnabled()) {
            father->onChanged(this);
        }
        if (!testStatus(Busy)) {
            Base::BitsetLocker<decltype(StatusBits)> guard(StatusBits, Busy);
            if (Document* doc = documentFromPropertyContainer(father)) {
                doc->emitCollaborationPropertyChanged(*this);
            }
            else {
                signalChanged(*this);
            }
        }
    }
    StatusBits.set(Touched);
}

void Property::aboutToSetValue()
{
    auto& cleanup = PropertyCleaner::state();
    PropertyCleaner::beginUse(cleanup);
    struct Done { PropertyCleaner::State& s; ~Done() { PropertyCleaner::endUse(s); } } done {cleanup};
    if (father) {
        if (Document* doc = documentFromPropertyContainer(father)) {
            enforceAtomicPresentationMutationTarget(doc);
        }
        father->onBeforeChange(this);
    }
}

void Property::verifyPath(const ObjectIdentifier& p) const
{
    p.verify(*this);
}

Property* Property::Copy() const
{
    // have to be reimplemented by a subclass!
    assert(0);
    return nullptr;
}

void Property::Paste(const Property& /*from*/)
{
    // have to be reimplemented by a subclass!
    assert(0);
}

void Property::setStatusValue(unsigned long status)
{
    // clang-format off
    static const unsigned long mask =
         (1<<PropDynamic)
        |(1<<PropNoRecompute)
        |(1<<PropReadOnly)
        |(1<<PropTransient)
        |(1<<PropOutput)
        |(1<<PropHidden)
        |(1<<PropNoPersist)
        |(1<<Busy);
    // clang-format on

    // User1..User3 are runtime re-entrancy lockers (Base::ObjectStatusLocker,
    // e.g. ViewProviderDocumentObject::updateView); they are never persisted
    // and are not observable state, so they are neither guarded nor published.
    // Otherwise another thread's or document's atomic presentation admission,
    // taken while a locker is alive, throws from the locker's destructor and
    // terminates. (User4 does not fit in StatusBits.)
    static constexpr unsigned long runtimeLockerMask =
        (1UL << User1) | (1UL << User2) | (1UL << User3);

    status &= ~mask;
    status |= StatusBits.to_ulong() & mask;
    unsigned long oldStatus = StatusBits.to_ulong();
    const bool onlyRuntimeLockerBits = ((status ^ oldStatus) & ~runtimeLockerMask) == 0;
    if (status != oldStatus && father && !onlyRuntimeLockerBits) {
        if (auto* document = documentFromPropertyContainer(father)) {
            enforceAtomicPresentationMutationTarget(document);
            // User3/Touched and other non-schema flags are temporary lockers
            // (e.g. GeoFeatureGroupExtension::extensionOnChanged). Persistence
            // schema still goes through DynamicProperty / setPropertyStatus;
            // ensurePropertyStatusMutationAllowed ignores those runtime bits.
            Internal::CollaborationStructuralMutationRecorder::
                ensurePropertyStatusMutationAllowed(*document, *this, oldStatus, status);
            capturePersistedStaticStatus(*this, *document, oldStatus, status);
        }
        else {
            // ViewProvider properties are GUI-owned containers. Their existing
            // pre-change path reaches GUI audit and the owning App transaction
            // before StatusBits is modified.
            father->onBeforeChange(this);
        }
    }
    StatusBits = decltype(StatusBits)(status);

    if (father) {
        if (status != oldStatus && !onlyRuntimeLockerBits) {
            publishPropertyMutation(*this, PropertyPublication::Status);
        }
        if (status != oldStatus) {
            father->onPropertyStatusChanged(*this, oldStatus);
        }
    }
}

void Property::setStatus(Status pos, bool on)
{
    auto bits = StatusBits;
    bits.set(pos, on);
    setStatusValue(bits.to_ulong());
}

bool Property::isSame(const Property& other) const
{
    if (&other == this) {
        return true;
    }
    if (other.getTypeId() != getTypeId() || getMemSize() != other.getMemSize()) {
        return false;
    }

    Base::StringWriter writer, writer2;
    Save(writer);
    other.Save(writer2);
    return writer.getString() == writer2.getString();
}

//**************************************************************************
//**************************************************************************
// PropertyListsBase
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

void PropertyListsBase::_setPyObject(PyObject* value)
{
    std::vector<int> indices;
    std::vector<PyObject*> vals;
    Py::Object pySeq;

    if (PyDict_Check(value)) {
        Py::Dict dict(value);
        auto size = dict.size();
        vals.reserve(size);
        indices.reserve(size);
        int listSize = getSize();
        for (auto it = dict.begin(); it != dict.end(); ++it) {
            const auto& item = *it;
            PyObject* key = item.first.ptr();
            if (!PyLong_Check(key)) {
                throw Base::TypeError("expect key type to be integer");
            }
            long idx = PyLong_AsLong(key);
            if (idx < -1 || idx > listSize) {
                throw Base::ValueError("index out of bound");
            }
            if (idx == -1 || idx == listSize) {
                idx = listSize;
                ++listSize;
            }
            indices.push_back(idx);
            vals.push_back(item.second.ptr());
        }
    }
    else {
        if (PySequence_Check(value)) {
            pySeq = value;
        }
        else {
            PyObject* iter = PyObject_GetIter(value);
            if (iter) {
                Py::Object pyIter(iter, true);
                pySeq = Py::asObject(PySequence_Fast(iter, ""));
            }
            else {
                PyErr_Clear();
                vals.push_back(value);
            }
        }
        if (!pySeq.isNone()) {
            Py::Sequence seq(pySeq);
            vals.reserve(seq.size());
            for (auto it = seq.begin(); it != seq.end(); ++it) {
                vals.push_back((*it).ptr());
            }
        }
    }
    setPyValues(vals, indices);
}


//**************************************************************************
//**************************************************************************
// PropertyLists
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE_ABSTRACT(App::PropertyLists, App::Property)
