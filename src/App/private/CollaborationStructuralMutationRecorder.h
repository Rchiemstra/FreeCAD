// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "App/Document.h"

namespace App
{
class ExtensionContainer;
}

namespace App::Internal
{

class CollaborationStructuralMutationRecorder final
{
public:
    static void record(
        Document& document,
        const std::vector<DocumentRevisionPublicationRequest>& effects)
    {
        document.recordCollaborationObservedStructuralEffects(effects);
    }

    static void recordContainer(Document& document, const PropertyContainer& container)
    {
        document.recordCollaborationObservedStructuralMutation(container);
    }

    static void ensurePropertySchemaMutationAllowed(
        Document& document,
        const PropertyContainer& container);
    static void ensurePropertyStatusMutationAllowed(
        Document& document,
        Property& property,
        unsigned long oldStatus,
        unsigned long newStatus);
    [[nodiscard]] static bool isTransactionOwnedNewObject(
        const Document& document,
        const DocumentObject& object);
    static void ensureDynamicPropertyRemovalAllowed(
        Document& document,
        const PropertyContainer& container,
        const Property* property);
    [[nodiscard]] static bool dynamicPropertyNotificationDeferralRequired(
        const Document& document) noexcept;
    static bool emitRemoveDynamicProperty(
        Document& document,
        const PropertyContainer& container,
        Property& property,
        std::shared_ptr<Property> retainedProperty,
        std::shared_ptr<std::string> retainedName);
    static void emitRenameDynamicProperty(
        Document& document,
        const PropertyContainer& container,
        Property& property,
        std::string oldName);
    static void ensureDynamicExtensionAllowed(
        Document& document,
        const ExtensionContainer& container);
    static void emitBeforeAddingDynamicExtension(
        Document& document,
        const ExtensionContainer& container,
        std::string extension);
    static void emitAddedDynamicExtension(
        Document& document,
        const ExtensionContainer& container,
        std::string extension);
};

}  // namespace App::Internal
