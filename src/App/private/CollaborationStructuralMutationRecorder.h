// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "App/Document.h"
#include "App/CollaborationRegistry.h"
#include "App/Range.h"
#include "Base/Type.h"

#include <cstdint>
#include <map>
#include <optional>

namespace Spreadsheet
{
class Sheet;
}

namespace App
{
class ExtensionContainer;
}

namespace App::Internal
{

class SpreadsheetCellRemovalPhysical;

// Only the native Sheet adapter can prove its raw canonical property and
// exact propAddress entry. This is an operation, not an ambient schema grant.
class AppExport SpreadsheetCellClearOperation final
{
public:
    ~SpreadsheetCellClearOperation();
    SpreadsheetCellClearOperation(const SpreadsheetCellClearOperation&) = delete;
    SpreadsheetCellClearOperation& operator=(const SpreadsheetCellClearOperation&) = delete;

private:
    friend class Spreadsheet::Sheet;
    friend class App::DocumentObject;
    friend class CollaborationStructuralMutationRecorder;
    friend class SpreadsheetCellRemovalPhysical;

    SpreadsheetCellClearOperation(DocumentObject& object, Property* property,
                                  CellAddress address,
                                  std::optional<CellAddress> mappedAddress,
                                  const std::map<const Property*, CellAddress>& addressMap);
    void removeCache();
    static bool requiresBoundary(const Document* document);

    Document* const _document;
    DocumentObject& _object;
    Property* const _property;
    const int64_t _propertyId;
    const Base::Type _propertyType;
    const CellAddress _address;
    // Capture the admitted entry and recheck the live native address map at
    // Entry and Physical. Only Sheet can supply this private const reference.
    const std::optional<CellAddress> _mappedAddress;
    const std::map<const Property*, CellAddress>& _addressMap;
    const std::string _name;
    DocumentIdentity _documentIdentity;
    std::string _objectIdentity;
    bool _privileged {false};
    bool _guarded {false};
    bool _entryConsumed {false};
};

// An Entry admission can reach the Physical gate only through the private
// DocumentObject helper. Callbacks never receive this stack-local handoff.
class SpreadsheetCellRemovalPhysical final
{
public:
    SpreadsheetCellRemovalPhysical(const SpreadsheetCellRemovalPhysical&) = delete;
    SpreadsheetCellRemovalPhysical& operator=(const SpreadsheetCellRemovalPhysical&) = delete;

private:
    friend class App::DocumentObject;
    friend class CollaborationStructuralMutationRecorder;
    explicit SpreadsheetCellRemovalPhysical(SpreadsheetCellClearOperation& operation);
    SpreadsheetCellClearOperation& _operation;
    bool _available {true};
    bool _effectRecorded {false};
};

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

private:
    friend class SpreadsheetCellClearOperation;
    friend class SpreadsheetCellRemovalPhysical;
    friend class App::DynamicProperty;
    static bool requiresCellClearBoundary(const Document* document);
    static void beginCellClear(SpreadsheetCellClearOperation& operation);
    static void endCellClear(SpreadsheetCellClearOperation& operation) noexcept;
    static void validateCellClear(const SpreadsheetCellClearOperation& operation);
    static void consumeCellClearEntry(SpreadsheetCellClearOperation& operation);
    static void recordCellClearRemoval(SpreadsheetCellRemovalPhysical& capability);
    static void consumeCellClearPhysical(SpreadsheetCellRemovalPhysical& capability,
                                        Document& document,
                                        const PropertyContainer& container,
                                        const Property* property, const char* name);
};

}  // namespace App::Internal
