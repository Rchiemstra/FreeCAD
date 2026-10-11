// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborationStructuralMutationRecorder.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

#include "App/Application.h"
#include "App/MutationClassification.h"
#include "Base/Exception.h"
#include "Base/Tools.h"
#include "App/DocumentObject.h"
#include "App/ExtensionContainer.h"
#include "App/GroupExtension.h"
#include "App/PropertyGeo.h"
#include "App/PropertyLinks.h"
#include "App/PropertyStandard.h"
#include "App/Transactions.h"
#include "DocumentP.h"

namespace App::Internal
{

namespace
{

bool isNewStructuralObject(const DocumentP& state, const DocumentObject& object)
{
    if ((state.activeUndoTransaction
         && state.activeUndoTransaction->isObjectNew(&object))
        || state.collaborationNewObjectStructuralSetup.contains(&object)
        || state.collaborationImportNewObjects.contains(&object)) {
        return true;
    }
    return std::ranges::any_of(
        state.collaborationDeferredNotifications,
        [&object](const CollaborationDeferredNotification& notification) {
            return notification.kind == CollaborationDeferredNotificationKind::NewObject
                && notification.object == &object;
        });
}

constexpr unsigned long dynamicSchemaStatusMask()
{
    return (1UL << Property::ReadOnly) | (1UL << Property::Hidden)
        | (1UL << Property::Transient) | (1UL << Property::Output)
        | (1UL << Property::LockDynamic) | (1UL << Property::Ordered)
        | (1UL << Property::EvalOnRestore) | (1UL << Property::CopyOnChange)
        | (1UL << Property::UserEdit);
}

bool propertyStatusMutationAffectsPersistence(const Property& property,
                                              const unsigned long oldStatus,
                                              const unsigned long newStatus)
{
    return ((oldStatus ^ newStatus) & dynamicSchemaStatusMask()) != 0
        && !property.testStatus(Property::PropNoPersist);
}

bool isLiveSketchAttachmentStatusMutation(const DocumentObject& object,
                                          const Property& property,
                                          const unsigned long oldStatus,
                                          const unsigned long newStatus)
{
    const Base::Type sketchType = Base::Type::fromName("Sketcher::SketchObject");
    if (sketchType.isBad() || object.getTypeId() != sketchType) {
        return false;
    }

    const auto* rawSupport = object.getPropertyByName("AttachmentSupport");
    const auto* support = freecad_cast<const PropertyLinkSubList*>(rawSupport);
    const auto* rawMapMode = object.getPropertyByName("MapMode");
    const auto* mapMode = freecad_cast<const PropertyEnumeration*>(rawMapMode);
    if (!support || rawSupport->getTypeId() != PropertyLinkSubList::getClassTypeId()
        || !mapMode || rawMapMode->getTypeId() != PropertyEnumeration::getClassTypeId()
        || mapMode->getValue() == 0) {
        return false;
    }
    const bool hasLiveSupport = std::ranges::any_of(
        support->getValues(), [](const DocumentObject* linked) {
            return linked && linked->getDocument() && linked->isAttachedToDocument();
        });
    if (!hasLiveSupport) {
        return false;
    }

    // AttachExtension owns exactly these presentation flags while a sketch is
    // actively supported: updateSinglePropertyStatus() exposes its attachment
    // controls and makes the inherited Placement read-only. Keep arbitrary
    // status changes, deactivated sketches, and unsupported sketches outside
    // this compatibility contract.
    const auto changed = oldStatus ^ newStatus;
    const char* propertyName = property.getName();
    if (!propertyName) {
        return false;
    }
    const std::string_view name(propertyName);
    if (changed == (1UL << Property::Hidden)) {
        const bool exactProperty =
            (name == "MapPathParameter"
             && property.getTypeId() == PropertyFloat::getClassTypeId())
            || (name == "MapReversed"
                && property.getTypeId() == PropertyBool::getClassTypeId())
            || (name == "AttachmentOffset"
                && property.getTypeId() == PropertyPlacement::getClassTypeId());
        return exactProperty && object.getPropertyByName(propertyName) == &property;
    }
    return changed == (1UL << Property::ReadOnly) && name == "Placement"
        && property.getTypeId() == PropertyPlacement::getClassTypeId()
        && object.getPropertyByName(propertyName) == &property;
}

bool isClaimedByRemovingContainer(const DocumentObject& object)
{
    // Body/Part/Group removal sets Remove on the container first, then
    // unsetupObject() restatuses owned sketches (MapReversed Hidden) while
    // those children do not yet carry Remove themselves. Treat membership in
    // a removing group as the same removal-owned grant, without adding a new
    // enumerated predicate.
    return std::ranges::any_of(object.getInList(), [&object](const DocumentObject* parent) {
        if (!parent || !parent->testStatus(ObjectStatus::Remove)
            || parent->getDocument() != object.getDocument()) {
            return false;
        }
        const auto* group = parent->getExtensionByType<GroupExtension>(true);
        return group && group->hasObject(&object, true);
    });
}

bool isGroundedJointPlacementLockMutation(const DocumentObject& object,
                                          const Property& property,
                                          const unsigned long oldStatus,
                                          const unsigned long newStatus)
{
    // Assembly's GroundedJoint pins the grounded object by flipping the
    // ReadOnly bit on its Placement -- and LinkPlacement when the grounded
    // object is a link -- from the joint's own onChanged, onDocumentRestored
    // and onDelete. The mutation therefore lands on a different, already
    // existing object than the one the commit created, so the new-object grant
    // never covers it and grounding a part inside an agent commit is refused.
    //
    // A grounded joint is an App::FeaturePython whose behaviour lives in a
    // Python proxy, so unlike the sketch case above there is no C++ type to
    // key on. Key on the exact structural signature instead: a live object in
    // this object's in-list holding an App::PropertyLinkGlobal named
    // ObjectToGround that points back here. Nothing else in FreeCAD uses that
    // property name, and the property is added locked by the joint itself.
    const auto changed = oldStatus ^ newStatus;
    if (changed != (1UL << Property::ReadOnly)) {
        return false;
    }
    const char* propertyName = property.getName();
    if (!propertyName) {
        return false;
    }
    const std::string_view name(propertyName);
    if ((name != "Placement" && name != "LinkPlacement")
        || property.getTypeId() != PropertyPlacement::getClassTypeId()
        || object.getPropertyByName(propertyName) != &property) {
        return false;
    }
    return std::ranges::any_of(
        object.getInList(), [&object](const DocumentObject* joint) {
            if (!joint || !joint->isAttachedToDocument()
                || joint->getDocument() != object.getDocument()) {
                return false;
            }
            const auto* rawGround = joint->getPropertyByName("ObjectToGround");
            const auto* ground = freecad_cast<const PropertyLinkGlobal*>(rawGround);
            return ground
                && rawGround->getTypeId() == PropertyLinkGlobal::getClassTypeId()
                && ground->getValue() == &object;
        });
}

void deferOrEmitDynamicExtension(
    DocumentP& state,
    const ExtensionContainer& container,
    std::string extension,
    CollaborationDeferredNotificationKind kind)
{
    if (state.collaborationCommitNotificationBarrier) {
        CollaborationDeferredNotification notification {kind};
        notification.propertyContainer = &container;
        notification.text = std::move(extension);
        state.collaborationDeferredNotifications.push_back(
            std::move(notification));
        return;
    }

    if (kind == CollaborationDeferredNotificationKind::BeforeAddingDynamicExtension) {
        GetApplication().signalBeforeAddingDynamicExtension(container, std::move(extension));
    }
    else {
        GetApplication().signalAddedDynamicExtension(container, std::move(extension));
    }
}

}  // namespace

SpreadsheetDerivedCacheAdapter::SpreadsheetDerivedCacheAdapter(
    DocumentObject& owner, Base::Type nativeType,
    DocumentObjectExecReturn* (*refresh)(DocumentObject&))
    : _owner(owner), _nativeType(nativeType), _refresh(refresh)
{}

SpreadsheetDerivedCacheAdapter::~SpreadsheetDerivedCacheAdapter() = default;

SpreadsheetDerivedCacheOperation::SpreadsheetDerivedCacheOperation(
    Document& document, DocumentObject& object)
    : _document(document), _object(object)
    , _documentIdentity(document.collaborationIdentity())
    , _objectIdentity(document.collaborationObjectIdentity(object))
    , _objectId(object.getID()), _objectType(object.getTypeId())
{}

void CollaborationStructuralMutationRecorder::validateSpreadsheetDerivedCache(
    const SpreadsheetDerivedCacheOperation& operation)
{
    auto& document = operation._document;
    auto& object = operation._object;
    auto& state = *document.d;
    enforceAtomicPresentationMutationTarget(&document);
    if (!document.containsObject(&object) || object.getDocument() != &document
        || !object.isAttachedToDocument() || object.getID() != operation._objectId
        || object.getTypeId() != operation._objectType
        || document.collaborationIdentity() != operation._documentIdentity
        || operation._documentIdentity.state != DocumentLifecycleState::Live
        || document.collaborationObjectIdentity(object) != operation._objectIdentity
        || !document.isCollaborationOwnerThread()
        || !document.testStatus(Document::Recomputing)
        || !state.collaborationCommitNotificationBarrier || !state.activeUndoTransaction
        || !state.suppressCollaborationRevisionPublication
        || state.collaborationCompatibilityStructuralMutationGranted
        || state.collaborationAtomicPresentationAuditActive
        || state.collaborationCommitPoisoned || state.collaborationReplayingNotifications
        || state.rollback || state.collaborationRollbackVerification
        || state.collaborationLifecycleMutationBlockDepth.load(std::memory_order_acquire) != 1
        || object.testStatus(ObjectStatus::Recompute)
        || object.testStatus(ObjectStatus::Destroy) || object.testStatus(ObjectStatus::Remove)
        || (state.collaborationSpreadsheetDerivedCache
            && state.collaborationSpreadsheetDerivedCache != &operation)) {
        throw Base::RuntimeError("spreadsheet derived cache crossed its authoritative boundary");
    }
}

std::optional<bool> CollaborationStructuralMutationRecorder::recomputeSpreadsheetCache(
    Document& document, DocumentObject& object)
{
    auto* adapter = dynamic_cast<SpreadsheetDerivedCacheAdapter*>(&object);
    auto& state = *document.d;
    if (!adapter || &adapter->_owner != &object || object.getTypeId() != adapter->_nativeType
        || !document.testStatus(Document::Recomputing)
        || state.collaborationCompatibilityStructuralMutationGranted
        || !state.collaborationDeferredRecomputeBlocked
        || state.collaborationSpreadsheetRecomputeSchemaObject
        || state.collaborationSpreadsheetCellClear) {
        return std::nullopt;
    }
    // The calling producer must be executing in this document and hold a real
    // dependency link to the sheet. No recursive scheduler or type-name trust.
    if (!std::ranges::any_of(object.getInList(), [&document](const DocumentObject* producer) {
            return producer && producer->getDocument() == &document
                && producer->testStatus(ObjectStatus::Recompute);
        })) {
        return std::nullopt;
    }
    SpreadsheetDerivedCacheOperation operation(document, object);
    validateSpreadsheetDerivedCache(operation);
    Base::ScopeGuard close([&] { state.collaborationSpreadsheetDerivedCache = nullptr; });
    state.collaborationSpreadsheetDerivedCache = &operation;
    try {
        // Own an error result even if post-call identity validation throws.
        std::unique_ptr<DocumentObjectExecReturn> result(adapter->_refresh(object));
        validateSpreadsheetDerivedCache(operation);
        if (result.get() != DocumentObject::StdReturn) {
            result->Which = &object;
            state.addRecomputeLog(result.release());
            return false;
        }
        object.setStatus(ObjectStatus::Error, false);
        return object.isValid();
    }
    catch (const Base::Exception& error) {
        state.addRecomputeLog(error.what(), &object);
    }
    catch (const std::exception& error) {
        state.addRecomputeLog(error.what(), &object);
    }
    return false;
}

bool CollaborationStructuralMutationRecorder::spreadsheetDerivedCacheActive(
    const DocumentObject& object)
{
    auto* document = object.getDocument();
    return document && document->d->collaborationSpreadsheetDerivedCache
        && &document->d->collaborationSpreadsheetDerivedCache->_object == &object;
}

void CollaborationStructuralMutationRecorder::validateSpreadsheetDerivedCacheReuse(
    DocumentObject& object, Property* property, CellAddress address,
    Base::Type nativePropertyType,
    const std::map<const Property*, CellAddress>& addressMap)
{
    const std::string name = address.toString(CellAddress::Cell::ShowRowColumn);
    // Resolve the current raw canonical pointer before lifetime/type/container
    // access. No alias lookup or property flags can establish native ownership.
    if (!address.isValid() || !property
        || object.PropertyContainer::getDynamicPropertyByName(name.c_str()) != property) {
        throw Base::RuntimeError("native spreadsheet cache reuse requires its canonical property");
    }
    const auto propertyId = property->getID();
    const auto propertyType = property->getTypeId();
    auto* document = object.getDocument();
    auto* operation = document ? document->d->collaborationSpreadsheetDerivedCache : nullptr;
    if (!operation || &operation->_object != &object) {
        throw Base::RuntimeError("native spreadsheet cache reuse has no matching operation");
    }
    validateSpreadsheetDerivedCache(*operation);
    const auto mapping = addressMap.find(property);
    if (object.PropertyContainer::getDynamicPropertyByName(name.c_str()) != property
        || property->getID() != propertyId || property->getTypeId() != propertyType
        || property->getContainer() != &object
        || !property->getName() || name != property->getName()
        || !property->testStatus(Property::PropDynamic)
        || property->testStatus(Property::LockDynamic)
        || mapping == addressMap.end() || mapping->second != address
        || (propertyType.isDerivedFrom(nativePropertyType) && propertyType != nativePropertyType)) {
        throw Base::RuntimeError("native spreadsheet cache reuse requires its exact owned native entry");
    }
    // A different type still goes through the existing typed removal gate.
    // An exact same-type entry is now admitted BEFORE map/value assignment.
}

Property* CollaborationStructuralMutationRecorder::addSpreadsheetDerivedCache(
    DocumentObject& object, CellAddress address, Base::Type nativePropertyType,
    const std::map<const Property*, CellAddress>& addressMap)
{
    auto& document = *object.getDocument();
    auto* operation = document.d->collaborationSpreadsheetDerivedCache;
    if (!operation || &operation->_object != &object || !address.isValid()) {
        throw Base::RuntimeError("native spreadsheet cache addition has no matching operation");
    }
    validateSpreadsheetDerivedCache(*operation);
    const std::string name = address.toString(CellAddress::Cell::ShowRowColumn);
    if (object.PropertyContainer::getPropertyByName(name.c_str())
        || std::ranges::any_of(addressMap, [address](const auto& entry) {
            return entry.second == address;
        })) {
        throw Base::RuntimeError("native spreadsheet cache addition requires an empty canonical entry");
    }
    // Fixed native property types are supplied solely by Sheet's private
    // setters. This qualified path has no granting scope, executes no feature,
    // and reaches the index insertion before any transaction/observer callback.
    auto* property = object.TransactionalObject::addDynamicProperty(
        nativePropertyType.getName(), name.c_str(), nullptr, nullptr,
        Prop_ReadOnly | Prop_Hidden | Prop_NoPersist);
    if (!property || object.PropertyContainer::getDynamicPropertyByName(name.c_str()) != property
        || property->getTypeId() != nativePropertyType || property->getContainer() != &object) {
        throw Base::RuntimeError("native spreadsheet cache addition changed its canonical identity");
    }
    const auto propertyId = property->getID();
    document.addOrRemovePropertyOfObject(&object, property, true);
    validateSpreadsheetDerivedCache(*operation);
    if (object.PropertyContainer::getDynamicPropertyByName(name.c_str()) != property
        || property->getID() != propertyId || property->getTypeId() != nativePropertyType) {
        throw Base::RuntimeError("native spreadsheet cache addition changed during transaction capture");
    }
    return property;
}

bool SpreadsheetCellClearOperation::requiresBoundary(const Document* document)
{
    return CollaborationStructuralMutationRecorder::requiresCellClearBoundary(document);
}

bool CollaborationStructuralMutationRecorder::requiresCellClearBoundary(const Document* document)
{
    // A foreign/read-only target is a boundary too, but never grants removal.
    // Outside these boundaries Sheet::clear retains the exact legacy path.
    return atomicPresentationMutationAdmissionActive()
        || (document && (document->d->collaborationCommitNotificationBarrier
                         || document->collaborationLifecycleMutationBlocked()));
}

SpreadsheetCellClearOperation::SpreadsheetCellClearOperation(
    DocumentObject& object, Property* property, CellAddress address,
    std::optional<CellAddress> mappedAddress,
    const std::map<const Property*, CellAddress>& addressMap, bool derivedCacheRemoval)
    : _document(object.getDocument())
    , _object(object)
    , _property(property)
    , _propertyId(property ? property->getID() : 0)
    , _propertyType(property ? property->getTypeId() : Base::Type::BadType)
    , _address(address)
    , _mappedAddress(mappedAddress)
    , _addressMap(addressMap)
    , _name(address.toString(CellAddress::Cell::ShowRowColumn))
    , _derivedCacheRemoval(derivedCacheRemoval)
{
    CollaborationStructuralMutationRecorder::beginCellClear(*this);
}

SpreadsheetCellClearOperation::~SpreadsheetCellClearOperation()
{
    CollaborationStructuralMutationRecorder::endCellClear(*this);
}

void SpreadsheetCellClearOperation::removeCache()
{
    if (_property && !_object.removeDynamicPropertyForCellClear(*this)) {
        throw Base::RuntimeError("spreadsheet canonical cache removal failed");
    }
}

SpreadsheetCellRemovalPhysical::SpreadsheetCellRemovalPhysical(
    SpreadsheetCellClearOperation& operation)
    : _operation(operation)
{
    // Consume Entry before transaction snapshots or expression callbacks.
    CollaborationStructuralMutationRecorder::consumeCellClearEntry(operation);
}

void CollaborationStructuralMutationRecorder::validateCellClear(
    const SpreadsheetCellClearOperation& operation)
{
    auto* document = operation._document;
    auto& object = operation._object;
    auto* property = operation._property;
    auto address = operation._address;
    if (!address.isValid()
        || object.testStatus(ObjectStatus::Destroy)
        || object.testStatus(ObjectStatus::Remove)) {
        throw Base::RuntimeError("spreadsheet cell clear requires a live cell owner");
    }
    // First resolve the live raw canonical entry before dereferencing the
    // captured pointer. Entry and Physical both recheck the native lifetime
    // ID and exact type, so allocator reuse cannot turn a replacement into
    // the admitted cache. Same-type persistent native adoption is supported.
    if (property
        && (object.PropertyContainer::getDynamicPropertyByName(operation._name.c_str())
                != property
            || property->getID() != operation._propertyId
            || property->getTypeId() != operation._propertyType
            || property->getContainer() != &object
            || !property->getName() || operation._name != property->getName()
            || !property->testStatus(Property::PropDynamic)
            || property->testStatus(Property::LockDynamic)
            || (operation._mappedAddress
                && *operation._mappedAddress != operation._address))) {
        throw Base::RuntimeError("spreadsheet cell clear requires its exact unlocked canonical cache");
    }
    const auto mapping = operation._addressMap.find(property);
    if ((property
         && (mapping == operation._addressMap.end()
             || mapping->second != operation._address
             || !operation._mappedAddress
             || mapping->second != *operation._mappedAddress))
        || (!property && std::ranges::any_of(operation._addressMap,
            [&operation](const auto& entry) {
                return entry.second == operation._address;
            }))) {
        throw Base::RuntimeError("spreadsheet cell clear native address mapping changed");
    }
    if (!document || !operation._privileged) {
        throw Base::RuntimeError("spreadsheet cell-clear capability requires a live boundary owner");
    }
    // Also enforces foreign/read-only/prepared mutation-target admission.
    enforceAtomicPresentationMutationTarget(document);
    auto& state = *document->d;
    if (state.collaborationSpreadsheetDerivedCache) {
        if (!operation._derivedCacheRemoval
            || &state.collaborationSpreadsheetDerivedCache->_object != &object) {
            throw Base::RuntimeError("clear callbacks cannot borrow spreadsheet derived cache authority");
        }
        validateSpreadsheetDerivedCache(*state.collaborationSpreadsheetDerivedCache);
    }
    if (!object.isAttachedToDocument() || !document->containsObject(&object)
        || object.getDocument() != document
        || document->collaborationIdentity() != operation._documentIdentity
        || operation._documentIdentity.state != DocumentLifecycleState::Live
        || document->collaborationObjectIdentity(object) != operation._objectIdentity
        || state.collaborationAtomicPresentationAuditActive
        || (state.collaborationSpreadsheetCellClear
            && state.collaborationSpreadsheetCellClear != &operation)) {
        throw Base::RuntimeError("spreadsheet cell clear crossed its live document boundary");
    }
    const bool rollbackVerification = state.collaborationRollbackVerification;
    const bool authoritativeCommitRecompute = state.activeUndoTransaction
        && state.suppressCollaborationRevisionPublication;
    if (!state.collaborationCommitNotificationBarrier
        || !document->isCollaborationOwnerThread()
        || (!document->testStatus(Document::Recomputing) && !rollbackVerification)
        || !(authoritativeCommitRecompute || rollbackVerification)
        || state.collaborationCompatibilityStructuralMutationGranted
        || state.collaborationReplayingNotifications
        || state.collaborationCommitPoisoned
        || (state.rollback && !rollbackVerification)
        || state.collaborationLifecycleMutationBlockDepth.load(std::memory_order_acquire) != 1
        || (state.collaborationSpreadsheetRecomputeSchemaObject
            && state.collaborationSpreadsheetRecomputeSchemaObject != &object)
        || (property && !operation._mappedAddress)) {
        throw Base::RuntimeError(
            "spreadsheet owned cell clear requires the authoritative collaboration recompute");
    }
}

void CollaborationStructuralMutationRecorder::beginCellClear(
    SpreadsheetCellClearOperation& operation)
{
    if (auto* document = operation._document) {
        operation._documentIdentity = document->collaborationIdentity();
        operation._objectIdentity = document->collaborationObjectIdentity(operation._object);
        operation._privileged = requiresCellClearBoundary(document);
    }
    validateCellClear(operation); // No alias, cell, map, transaction or callback mutation yet.
    if (operation._privileged) {
        operation._document->d->collaborationSpreadsheetCellClear = &operation;
        operation._guarded = true;
    }
}

void CollaborationStructuralMutationRecorder::endCellClear(
    SpreadsheetCellClearOperation& operation) noexcept
{
    if (operation._guarded
        && operation._document->d->collaborationSpreadsheetCellClear == &operation) {
        operation._document->d->collaborationSpreadsheetCellClear = nullptr;
    }
}

void CollaborationStructuralMutationRecorder::consumeCellClearEntry(
    SpreadsheetCellClearOperation& operation)
{
    if (operation._entryConsumed || !operation._property) {
        throw Base::RuntimeError("spreadsheet cell removal Entry has already been consumed");
    }
    operation._entryConsumed = true;
    validateCellClear(operation);
}

void CollaborationStructuralMutationRecorder::consumeCellClearPhysical(
    SpreadsheetCellRemovalPhysical& capability, Document& document,
    const PropertyContainer& container, const Property* property, const char* name)
{
    if (!capability._available) {
        throw Base::RuntimeError("spreadsheet cell removal Physical has already been consumed");
    }
    capability._available = false; // Before notification, destruction, or index erase.
    const auto& operation = capability._operation;
    if (!operation._entryConsumed || operation._document != &document
        || &operation._object != &container || operation._property != property
        || !name || operation._name != name) {
        throw Base::RuntimeError("spreadsheet cell removal Physical identity mismatch");
    }
    validateCellClear(operation);
}

void CollaborationStructuralMutationRecorder::recordCellClearRemoval(
    SpreadsheetCellRemovalPhysical& capability)
{
    const auto& operation = capability._operation;
    if (capability._available || capability._effectRecorded || !operation._entryConsumed) {
        throw Base::RuntimeError("spreadsheet cell removal ledger requires one consumed Physical");
    }
    capability._effectRecorded = true;
    if (!operation._privileged) {
        return; // Ordinary removal retains its existing immediate publication path.
    }
    auto& document = *operation._document;
    if (document.d->collaborationSpreadsheetCellClear != &operation
        || operation._object.getDocument() != &document
        || !operation._object.isAttachedToDocument()
        || !document.containsObject(&operation._object)
        || document.collaborationIdentity() != operation._documentIdentity
        || document.collaborationObjectIdentity(operation._object) != operation._objectIdentity) {
        throw Base::RuntimeError("spreadsheet cell removal ledger identity mismatch");
    }
    // Record only the actual physical schema effect, never arbitrary callback
    // writes while the cleanup guard is active. The coordinator deduplicates
    // these same keys with its declared effects and any existing Sheet scope.
    const std::vector<DocumentRevisionPublicationRequest> effects {
        {DocumentRevisionKey::unknownModelMutation(), std::nullopt},
        {DocumentRevisionKey::objectStructure(operation._object.getNameInDocument()),
         operation._objectIdentity},
    };
    auto& ledger = document.d->collaborationObservedStructuralEffects;
    ledger.insert(ledger.end(), effects.begin(), effects.end());
}

void CollaborationStructuralMutationRecorder::ensureDynamicExtensionAllowed(
    Document& document,
    const ExtensionContainer& container)
{
    ensurePropertySchemaMutationAllowed(document, container);
}

void CollaborationStructuralMutationRecorder::ensurePropertySchemaMutationAllowed(
    Document& document,
    const PropertyContainer& container)
{
    const auto* object = dynamic_cast<const DocumentObject*>(&container);
    const auto kind = object && object->getDocument() == &document
            && document.containsObject(object) && isNewStructuralObject(*document.d, *object)
        ? Document::CollaborationStructuralMutationKind::DynamicPropertyOnNewObject
        : Document::CollaborationStructuralMutationKind::Restricted;
    std::string mutation = "propertySchema on ";
    if (object) {
        const char* objectName = object->getNameInDocument();
        mutation += (objectName && *objectName) ? objectName : "<unnamed>";
    }
    else {
        mutation += container.getTypeId().getName();
    }
    document.ensureCollaborationStructuralMutationAllowed(kind, mutation.c_str());
}

void CollaborationStructuralMutationRecorder::ensurePropertyStatusMutationAllowed(
    Document& document,
    Property& property,
    const unsigned long oldStatus,
    const unsigned long newStatus)
{
    // NoPersist normally exempts presentation flags from persistence
    // admission. During native clear, callbacks must not change dynamic locks
    // or schema flags (including NoPersist caches) between Entry and Physical.
    // Runtime locker bits keep their established non-granting behavior.
    if ((document.d->collaborationSpreadsheetCellClear
         || document.d->collaborationSpreadsheetDerivedCache)
        && property.testStatus(Property::PropDynamic)
        && ((oldStatus ^ newStatus) & dynamicSchemaStatusMask()) != 0) {
        throw Base::RuntimeError("status callbacks cannot borrow spreadsheet cell-clear authority");
    }
    // Internal guards use runtime-only status bits such as User3 while
    // maintaining group link caches. Those transitions do not change the
    // persisted property schema and are safe inside an atomic commit.
    if (!propertyStatusMutationAffectsPersistence(property, oldStatus, newStatus)) {
        return;
    }
    auto* container = property.getContainer();
    const auto* object = dynamic_cast<const DocumentObject*>(container);
    const bool attachedStructuralObject = object && object->getDocument() == &document
        && document.containsObject(object);
    // ReadOnly, Hidden, and the other persisted flags on an existing static
    // property are presentation state. They are not an object or
    // dynamic-property structure change, so the stable-boundary gate does not
    // apply. Dynamic-property status stays structural.
    if (attachedStructuralObject && !property.testStatus(Property::PropDynamic)) {
        const char* propertyName = property.getName();
        if (propertyName && *propertyName
            && object->getPropertyByName(propertyName) == &property) {
            return;
        }
    }
    const bool newStructuralObject = attachedStructuralObject
        && isNewStructuralObject(*document.d, *object);
    // unsetupObject() may adjust persistent property flags while an object is
    // being removed (for example PartDesign attachment/placement flags). The
    // object already carries Document's internal Remove status, so these
    // changes cannot survive the removal boundary and are covered by its
    // structural transaction snapshot. Do not grant the same authority to a
    // surviving existing object.
    const bool removalOwnedStatus = attachedStructuralObject
        && (object->testStatus(ObjectStatus::Remove)
            || isClaimedByRemovingContainer(*object));
    const bool liveSketchAttachmentStatus = attachedStructuralObject
        && isLiveSketchAttachmentStatusMutation(*object, property, oldStatus, newStatus);
    const bool groundedJointPlacementLock = attachedStructuralObject
        && isGroundedJointPlacementLockMutation(*object, property, oldStatus, newStatus);
    // execute() routinely republishes which of its own inputs currently apply:
    // PartDesign's extrude features flip ReadOnly on AlongSketchNormal and the
    // offset/length group as the pad type changes. The coordinator runs that
    // execute() on the owner thread inside the commit boundary, so the writes
    // land here rather than in a detached process where the recorder never
    // saw them. This is the recompute-time counterpart of the removal-owned
    // grant above and is keyed just as narrowly: the object the coordinator is
    // currently executing, adjusting a property it owns itself.
    const char* const statusPropertyName = property.getName();
    const bool executeOwnedStatus = attachedStructuralObject
        && object->testStatus(ObjectStatus::Recompute) && statusPropertyName
        && *statusPropertyName
        && object->getPropertyByName(statusPropertyName) == &property;
    auto kind = Document::CollaborationStructuralMutationKind::Restricted;
    if (newStructuralObject) {
        kind = Document::CollaborationStructuralMutationKind::DynamicPropertyOnNewObject;
    }
    else if (removalOwnedStatus || liveSketchAttachmentStatus
             || groundedJointPlacementLock || executeOwnedStatus) {
        kind = Document::CollaborationStructuralMutationKind::Object;
    }
    std::string mutation = "propertyStatus on ";
    if (object) {
        const char* objectName = object->getNameInDocument();
        mutation += (objectName && *objectName) ? objectName : "<unnamed>";
    }
    else if (container) {
        mutation += container->getTypeId().getName();
    }
    else {
        mutation += "<detached>";
    }
    if (const char* propertyName = property.getName(); propertyName && *propertyName) {
        mutation += ".";
        mutation += propertyName;
    }
    document.ensureCollaborationStructuralMutationAllowed(kind, mutation.c_str());
}

bool CollaborationStructuralMutationRecorder::isTransactionOwnedNewObject(
    const Document& document,
    const DocumentObject& object)
{
    return object.getDocument() == &document && document.containsObject(&object)
        && document.d->activeUndoTransaction
        && document.d->activeUndoTransaction->isObjectNew(&object);
}

void CollaborationStructuralMutationRecorder::ensureDynamicPropertyRemovalAllowed(
    Document& document,
    const PropertyContainer& container,
    const Property* property)
{
    if (const auto* object = dynamic_cast<const DocumentObject*>(&container)) {
        document.ensureCollaborationDynamicPropertyRemovalAllowed(*object, property);
        return;
    }
    ensurePropertySchemaMutationAllowed(document, container);
}

bool CollaborationStructuralMutationRecorder::dynamicPropertyNotificationDeferralRequired(
    const Document& document) noexcept
{
    return document.d->collaborationCommitNotificationBarrier;
}

bool CollaborationStructuralMutationRecorder::emitRemoveDynamicProperty(
    Document& document,
    const PropertyContainer& container,
    Property& property,
    std::shared_ptr<Property> retainedProperty,
    std::shared_ptr<std::string> retainedName)
{
    if (!document.d->collaborationCommitNotificationBarrier) {
        GetApplication().signalRemoveDynamicProperty(property);
        return false;
    }

    auto& notifications = document.d->collaborationDeferredNotifications;
    const auto wasAddedInsideBarrier = std::ranges::any_of(
        notifications,
        [&property](const CollaborationDeferredNotification& notification) {
            return notification.kind
                    == CollaborationDeferredNotificationKind::AppendDynamicProperty
                && notification.property == &property;
        });
    if (wasAddedInsideBarrier) {
        // The property exists in neither stable state.  Drop every queued
        // property-specific observer record so replay cannot expose an
        // impossible append/remove history or retain a dangling pointer.
        notifications.erase(
            std::remove_if(
                notifications.begin(),
                notifications.end(),
                [&property](const CollaborationDeferredNotification& notification) {
                    return notification.property == &property;
                }),
            notifications.end());
        return false;
    }

    const auto firstRename = std::ranges::find_if(
        notifications,
        [&property](const CollaborationDeferredNotification& notification) {
            return notification.kind
                    == CollaborationDeferredNotificationKind::RenameDynamicProperty
                && notification.property == &property;
        });
    if (firstRename != notifications.end()) {
        // A rename followed by removal is one removal from the boundary-visible
        // old name, not a rename of an already-absent property.
        *retainedName = firstRename->text;
        notifications.erase(
            std::remove_if(
                notifications.begin(),
                notifications.end(),
                [&property](const CollaborationDeferredNotification& notification) {
                    return notification.kind
                            == CollaborationDeferredNotificationKind::RenameDynamicProperty
                        && notification.property == &property;
                }),
            notifications.end());
    }
    CollaborationDeferredNotification notification {
        CollaborationDeferredNotificationKind::RemoveDynamicProperty};
    notification.property = &property;
    notification.propertyContainer = &container;
    notification.retainedText = std::move(retainedName);
    notification.retainedProperty = std::move(retainedProperty);
    notifications.push_back(std::move(notification));
    return true;
}

void CollaborationStructuralMutationRecorder::emitRenameDynamicProperty(
    Document& document,
    const PropertyContainer& container,
    Property& property,
    std::string oldName)
{
    if (!document.d->collaborationCommitNotificationBarrier) {
        GetApplication().signalRenameDynamicProperty(property, oldName.c_str());
        return;
    }
    const auto alreadyRepresented = std::ranges::any_of(
        document.d->collaborationDeferredNotifications,
        [&property](const CollaborationDeferredNotification& notification) {
            return notification.property == &property
                && (notification.kind
                        == CollaborationDeferredNotificationKind::AppendDynamicProperty
                    || notification.kind
                        == CollaborationDeferredNotificationKind::RenameDynamicProperty);
        });
    if (alreadyRepresented) {
        // An append observes the final name at replay.  Repeated renames retain
        // the first boundary-visible old name in the existing rename record.
        return;
    }
    CollaborationDeferredNotification notification {
        CollaborationDeferredNotificationKind::RenameDynamicProperty};
    notification.property = &property;
    notification.propertyContainer = &container;
    notification.text = std::move(oldName);
    document.d->collaborationDeferredNotifications.push_back(std::move(notification));
}

void CollaborationStructuralMutationRecorder::emitBeforeAddingDynamicExtension(
    Document& document,
    const ExtensionContainer& container,
    std::string extension)
{
    deferOrEmitDynamicExtension(
        *document.d,
        container,
        std::move(extension),
        CollaborationDeferredNotificationKind::BeforeAddingDynamicExtension);
}

void CollaborationStructuralMutationRecorder::emitAddedDynamicExtension(
    Document& document,
    const ExtensionContainer& container,
    std::string extension)
{
    deferOrEmitDynamicExtension(
        *document.d,
        container,
        std::move(extension),
        CollaborationDeferredNotificationKind::AddedDynamicExtension);
}

}  // namespace App::Internal
