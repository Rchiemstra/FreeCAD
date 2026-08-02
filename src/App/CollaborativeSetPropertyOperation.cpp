// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborativeSetPropertyOperation.h"

#include "CollaborativeOperation.h"
#include "CollaborativeOperationRegistry.h"
#include "Document.h"
#include "DocumentObject.h"
#include "DocumentRevisionIndex.h"
#include "PropertyContainer.h"
#include "PropertyStandard.h"
#include "private/CollaborativeOperationRegistryInternal.h"

#include <Base/Exception.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace
{

using namespace App;

enum class ScalarPropertyType
{
    Boolean,
    Integer,
    Float,
    String
};

using ScalarPropertyValue = std::variant<bool, long, double, std::string>;

constexpr std::string_view ObjectArgument = "object";
constexpr std::string_view PropertyArgument = "property";
constexpr std::string_view ValueTypeArgument = "value_type";
constexpr std::string_view ValueArgument = "value";

[[nodiscard]] std::string argument(const CollaborativeOperationIntent& intent,
                                   std::string_view name)
{
    const auto found = intent.arguments.find(std::string(name));
    if (found == intent.arguments.end()) {
        throw std::invalid_argument("collaborative set-property argument is missing: "
                                    + std::string(name));
    }
    return found->second;
}

void validateArgumentNames(const CollaborativeOperationIntent& intent)
{
    constexpr std::string_view required[] {
        ObjectArgument, PropertyArgument, ValueTypeArgument, ValueArgument};
    if (intent.arguments.size() != std::size(required)) {
        throw std::invalid_argument(
            "collaborative set-property intent has unexpected or missing arguments");
    }
    for (const auto name : required) {
        if (!intent.arguments.contains(std::string(name))) {
            throw std::invalid_argument(
                "collaborative set-property intent has unexpected or missing arguments");
        }
    }
}

[[nodiscard]] ScalarPropertyType parseValueType(const std::string& valueType)
{
    if (valueType == "bool") {
        return ScalarPropertyType::Boolean;
    }
    if (valueType == "integer") {
        return ScalarPropertyType::Integer;
    }
    if (valueType == "float") {
        return ScalarPropertyType::Float;
    }
    if (valueType == "string") {
        return ScalarPropertyType::String;
    }
    throw std::invalid_argument("collaborative set-property value_type is unsupported");
}

[[nodiscard]] ScalarPropertyValue parseValue(ScalarPropertyType type, const std::string& value)
{
    switch (type) {
        case ScalarPropertyType::Boolean:
            if (value == "true") {
                return true;
            }
            if (value == "false") {
                return false;
            }
            throw std::invalid_argument("collaborative set-property bool value is invalid");

        case ScalarPropertyType::Integer: {
            long parsed = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc {} || end != value.data() + value.size()) {
                throw std::invalid_argument("collaborative set-property integer value is invalid");
            }
            return parsed;
        }

        case ScalarPropertyType::Float: {
            std::istringstream stream(value);
            stream.imbue(std::locale::classic());
            stream >> std::noskipws;
            double parsed = 0.0;
            if (!(stream >> parsed) || !stream.eof() || !std::isfinite(parsed)) {
                throw std::invalid_argument("collaborative set-property float value is invalid");
            }
            return parsed;
        }

        case ScalarPropertyType::String:
            return value;
    }
    throw std::invalid_argument("collaborative set-property value type is invalid");
}

[[nodiscard]] bool propertyMatchesType(const Property& property, ScalarPropertyType type)
{
    switch (type) {
        case ScalarPropertyType::Boolean:
            return property.getTypeId() == PropertyBool::getClassTypeId();
        case ScalarPropertyType::Integer:
            return property.getTypeId() == PropertyInteger::getClassTypeId();
        case ScalarPropertyType::Float:
            return property.getTypeId() == PropertyFloat::getClassTypeId();
        case ScalarPropertyType::String:
            return property.getTypeId() == PropertyString::getClassTypeId();
    }
    return false;
}

[[nodiscard]] bool propertyIsCollaborativelyEditable(const PropertyContainer& container,
                                                       const Property& property) noexcept
{
    const short declaredType = container.getPropertyType(&property);
    return !container.isReadOnly(&property) && (declaredType & Prop_Output) == 0
        && !property.isReadOnly() && !property.testStatus(Property::Output);
}

[[nodiscard]] const Property& resolveProperty(const Document& document,
                                               const std::string& objectName,
                                               const std::string& objectIdentity,
                                               const std::string& propertyName,
                                               ScalarPropertyType valueType)
{
    const auto* object = document.getObject(objectName.c_str());
    if (!object || document.collaborationObjectIdentity(*object) != objectIdentity) {
        throw std::runtime_error("collaborative set-property target object became stale");
    }
    const auto* property = object->getPropertyByName(propertyName.c_str());
    if (!property) {
        throw std::runtime_error("collaborative set-property target property no longer exists");
    }
    if (!propertyMatchesType(*property, valueType)) {
        throw std::runtime_error("collaborative set-property target property type changed");
    }
    if (!propertyIsCollaborativelyEditable(*object, *property)) {
        throw std::runtime_error("collaborative set-property target property is not editable");
    }
    return *property;
}

class SetPropertyOperation final: public CollaborativeOperation
{
public:
    SetPropertyOperation(std::string objectName,
                         std::string objectIdentity,
                         std::string propertyName,
                         ScalarPropertyType valueType,
                         ScalarPropertyValue value)
        : _objectName(std::move(objectName))
        , _objectIdentity(std::move(objectIdentity))
        , _propertyName(std::move(propertyName))
        , _valueType(valueType)
        , _value(std::move(value))
    {}

    std::string_view typeId() const noexcept override
    {
        return CollaborativeSetPropertyOperationType;
    }

    void apply(Document& document) const override
    {
        const Property& resolved = resolveProperty(document,
                                                   _objectName,
                                                   _objectIdentity,
                                                   _propertyName,
                                                   _valueType);
        auto& property = const_cast<Property&>(resolved);
        switch (_valueType) {
            case ScalarPropertyType::Boolean:
                static_cast<PropertyBool&>(property).setValue(std::get<bool>(_value));
                return;
            case ScalarPropertyType::Integer:
                static_cast<PropertyInteger&>(property).setValue(std::get<long>(_value));
                return;
            case ScalarPropertyType::Float:
                static_cast<PropertyFloat&>(property).setValue(std::get<double>(_value));
                return;
            case ScalarPropertyType::String:
                static_cast<PropertyString&>(property).setValue(std::get<std::string>(_value));
                return;
        }
        throw std::runtime_error("collaborative set-property value type is invalid");
    }

    CollaborativePostconditionResult checkPostcondition(const Document& document) const override
    {
        try {
            const Property& property = resolveProperty(document,
                                                       _objectName,
                                                       _objectIdentity,
                                                       _propertyName,
                                                       _valueType);
            bool matches = false;
            switch (_valueType) {
                case ScalarPropertyType::Boolean:
                    matches = static_cast<const PropertyBool&>(property).getValue()
                        == std::get<bool>(_value);
                    break;
                case ScalarPropertyType::Integer:
                    matches = static_cast<const PropertyInteger&>(property).getValue()
                        == std::get<long>(_value);
                    break;
                case ScalarPropertyType::Float:
                    matches = static_cast<const PropertyFloat&>(property).getValue()
                        == std::get<double>(_value);
                    break;
                case ScalarPropertyType::String:
                    matches = static_cast<const PropertyString&>(property).getStrValue()
                        == std::get<std::string>(_value);
                    break;
            }
            return {matches,
                    matches ? std::string {}
                            : "collaborative set-property value does not match prepared value"};
        }
        catch (const Base::Exception& exception) {
            return {false, exception.what()};
        }
        catch (const std::exception& exception) {
            return {false, exception.what()};
        }
    }

private:
    const std::string _objectName;
    const std::string _objectIdentity;
    const std::string _propertyName;
    const ScalarPropertyType _valueType;
    const ScalarPropertyValue _value;
};

[[nodiscard]] CollaborativeOperationPreparation prepareSetProperty(
    const Document& document,
    const CollaborativeOperationIntent& intent)
{
    validateArgumentNames(intent);
    const std::string objectName = argument(intent, ObjectArgument);
    const std::string propertyName = argument(intent, PropertyArgument);
    const std::string valueTypeName = argument(intent, ValueTypeArgument);
    if (objectName.empty() || propertyName.empty() || valueTypeName.empty()) {
        throw std::invalid_argument(
            "collaborative set-property object, property, and value_type must be nonempty");
    }
    if (propertyName == "Label") {
        throw std::invalid_argument(
            "collaborative set-property does not support the semantic Label property");
    }
    const ScalarPropertyType valueType = parseValueType(valueTypeName);
    const ScalarPropertyValue value = parseValue(valueType, argument(intent, ValueArgument));

    const auto* object = document.getObject(objectName.c_str());
    if (!object) {
        throw std::invalid_argument("collaborative set-property target object does not exist");
    }
    const auto* property = object->getPropertyByName(propertyName.c_str());
    if (!property) {
        throw std::invalid_argument("collaborative set-property target property does not exist");
    }
    if (!propertyMatchesType(*property, valueType)) {
        throw std::invalid_argument(
            "collaborative set-property value_type does not match property");
    }
    if (!propertyIsCollaborativelyEditable(*object, *property)) {
        throw std::invalid_argument(
            "collaborative set-property target property is not editable");
    }

    const std::string objectIdentity = document.collaborationObjectIdentity(*object);

    // Freeze the same-document reverse dependency closure. The map both
    // removes a possible cycle duplicate and gives deterministic object-name
    // order without retaining any live object pointer in the preparation.
    std::map<std::string, std::string> affectedObjects;
    affectedObjects.emplace(objectName, objectIdentity);
    for (const auto* dependent : object->getInListRecursive()) {
        if (!dependent || dependent->getDocument() != &document) {
            continue;
        }
        const char* dependentName = dependent->getNameInDocument();
        if (!dependentName || *dependentName == '\0') {
            throw std::invalid_argument(
                "collaborative set-property dependent object has no document name");
        }
        const std::string dependentIdentity =
            document.collaborationObjectIdentity(*dependent);
        const auto [found, inserted] =
            affectedObjects.emplace(dependentName, dependentIdentity);
        if (!inserted && found->second != dependentIdentity) {
            throw std::invalid_argument(
                "collaborative set-property dependency graph has ambiguous object identity");
        }
    }

    std::vector<DocumentRevisionKey> reads;
    std::vector<DocumentRevisionKey> writes;
    std::vector<DocumentRevisionPublicationRequest> effects;
    reads.reserve(affectedObjects.size() * 3 + 2);
    writes.reserve(affectedObjects.size());
    effects.reserve(affectedObjects.size());
    for (const auto& [affectedName, affectedIdentity] : affectedObjects) {
        reads.push_back(DocumentRevisionKey::objectExistence(affectedName));
        reads.push_back(DocumentRevisionKey::objectModel(affectedName));
        reads.push_back(DocumentRevisionKey::objectStructure(affectedName));
        writes.push_back(DocumentRevisionKey::objectModel(affectedName));
        effects.push_back(
            {DocumentRevisionKey::objectModel(affectedName), affectedIdentity});
    }
    // A no-touch link mutation can grow the reverse dependency closure
    // without scheduling recompute or touching the wildcard. Freeze the
    // document structure that the closure was derived from as well.
    reads.push_back(DocumentRevisionKey::documentStructure());
    reads.push_back(DocumentRevisionKey::unknownModelMutation());
    std::sort(reads.begin(), reads.end());
    std::sort(writes.begin(), writes.end());
    std::sort(effects.begin(), effects.end(), [](const auto& left, const auto& right) {
        return left.key < right.key;
    });

    return {std::move(reads),
            std::move(writes),
            std::move(effects),
            std::make_unique<const SetPropertyOperation>(objectName,
                                                         objectIdentity,
                                                         propertyName,
                                                         valueType,
                                                         value)};
}

}  // namespace

void App::ensureCollaborativeSetPropertyOperationRegistered()
{
    static std::once_flag registered;
    std::call_once(registered, [] {
        Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(CollaborativeSetPropertyOperationType), prepareSetProperty);
    });
}
