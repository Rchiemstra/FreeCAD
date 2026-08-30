// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborationStructuralMutationRecorder.h"

#include <algorithm>
#include <string>

#include "App/Application.h"
#include "App/DocumentObject.h"
#include "App/ExtensionContainer.h"
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
    auto* container = property.getContainer();
    const auto* object = dynamic_cast<const DocumentObject*>(container);
    static_cast<void>(oldStatus);
    static_cast<void>(newStatus);
    const bool attachedStructuralObject = object && object->getDocument() == &document
        && document.containsObject(object);
    const bool newStructuralObject = attachedStructuralObject
        && isNewStructuralObject(*document.d, *object);
    const auto kind = newStructuralObject
        ? Document::CollaborationStructuralMutationKind::DynamicPropertyOnNewObject
        : Document::CollaborationStructuralMutationKind::Restricted;
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
