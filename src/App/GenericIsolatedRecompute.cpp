// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GenericIsolatedRecompute.h"

#include "Application.h"
#include "CollaborativeOperation.h"
#include "Document.h"
#include "DocumentObject.h"
#include "GeometryWorkerOperationRegistry.h"
#include "MergeDocuments.h"
#include "ObjectIdentifier.h"
#include "PropertyLinks.h"
#include "PropertyPythonObject.h"
#include "private/CollaborativeOperationRegistryInternal.h"

#include <Base/Exception.h>
#include <Base/Tools.h>
#include <Base/Type.h>
#include <Base/Writer.h>

#include <QCryptographicHash>

#include <boost/scope_exit.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <typeinfo>
#include <unordered_set>
#include <utility>
#include <vector>

namespace App::Internal
{

class GenericIsolatedRecomputeAccess
{
public:
    static int execute(Document& document, DocumentObject& feature)
    {
        if (!document.testStatus(Document::TempDoc)
            || feature.getDocument() != &document
            || !feature.isAttachedToDocument()) {
            throw Base::RuntimeError(
                "generic recompute worker access requires an attached temporary document");
        }
        return document._recomputeFeature(&feature);
    }

    static int executeAuthoritative(Document& document, DocumentObject& feature)
    {
        if (document.testStatus(Document::TempDoc)
            || !document.testStatus(Document::Recomputing)
            || !document.isCollaborationOwnerThread()
            || !document.hasPendingTransaction()
            || !document.collaborationRevisionPublicationSuppressed()
            || feature.getDocument() != &document
            || !feature.isAttachedToDocument()) {
            throw Base::RuntimeError(
                "authoritative transient-schema recompute requires the coordinator commit boundary");
        }
        return document._recomputeFeature(&feature);
    }

    static void applyFailure(Document& document,
                             DocumentObject& feature,
                             const std::string_view diagnostic)
    {
        document.applyCollaborationRecomputeFailure(feature, diagnostic);
    }
};

}  // namespace App::Internal

namespace
{

constexpr std::uint32_t ProtocolMagic = 0x31524947U;  // GIR1
constexpr std::uint32_t ProtocolVersion = 2;
constexpr std::size_t MaxObjects = 10'000;
constexpr std::size_t MaxProperties = 1'000'000;
constexpr std::size_t MaxFieldBytes = 1U << 20;
constexpr std::size_t MaxPayloadBytes = 128U << 20;

struct PropertyManifest
{
    std::string name;
    std::string type;
    bool output {false};
};

struct ObjectManifest
{
    std::string name;
    std::string type;
    std::vector<PropertyManifest> properties;
};

struct DecodedOutput
{
    std::string name;
    std::string type;
    std::unique_ptr<App::Property> value;
};

void appendU32(std::vector<std::uint8_t>& target, const std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        target.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void appendString(std::vector<std::uint8_t>& target, const std::string_view value)
{
    if (value.size() > MaxFieldBytes
        || value.size() > std::numeric_limits<std::uint32_t>::max()
        || target.size() > MaxPayloadBytes - value.size() - sizeof(std::uint32_t)) {
        throw std::invalid_argument("generic recompute field exceeds its protocol limit");
    }
    appendU32(target, static_cast<std::uint32_t>(value.size()));
    target.insert(target.end(), value.begin(), value.end());
}

void appendBytes(std::vector<std::uint8_t>& target, const std::string& value)
{
    if (value.size() > MaxPayloadBytes
        || value.size() > std::numeric_limits<std::uint32_t>::max()
        || target.size() > MaxPayloadBytes - value.size() - sizeof(std::uint32_t)) {
        throw std::invalid_argument("generic recompute value exceeds its protocol limit");
    }
    appendU32(target, static_cast<std::uint32_t>(value.size()));
    target.insert(target.end(), value.begin(), value.end());
}

class BinaryReader
{
public:
    explicit BinaryReader(const std::vector<std::uint8_t>& bytes)
        : _bytes(bytes)
    {
        if (_bytes.size() > MaxPayloadBytes) {
            throw std::invalid_argument("generic recompute payload is oversized");
        }
    }

    std::uint32_t u32()
    {
        require(sizeof(std::uint32_t));
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(_bytes[_offset++]) << shift;
        }
        return value;
    }

    std::string string()
    {
        const auto size = static_cast<std::size_t>(u32());
        if (size > MaxFieldBytes) {
            throw std::invalid_argument("generic recompute field is oversized");
        }
        return bytes(size);
    }

    std::string value()
    {
        return bytes(static_cast<std::size_t>(u32()));
    }

    void finish() const
    {
        if (_offset != _bytes.size()) {
            throw std::invalid_argument("generic recompute payload has trailing bytes");
        }
    }

private:
    void require(const std::size_t size) const
    {
        if (size > _bytes.size() - _offset) {
            throw std::invalid_argument("generic recompute payload is truncated");
        }
    }

    std::string bytes(const std::size_t size)
    {
        require(size);
        const auto* start = reinterpret_cast<const char*>(_bytes.data() + _offset);
        std::string result(start, size);
        _offset += size;
        return result;
    }

    const std::vector<std::uint8_t>& _bytes;
    std::size_t _offset {0};
};

const App::GeometryArchiveSection& requireSection(
    const App::GeometryArchive& archive,
    const std::string_view name,
    const std::size_t expectedSectionCount)
{
    if (archive.sections.size() != expectedSectionCount) {
        throw std::invalid_argument("generic recompute archive has an unexpected section count");
    }
    const auto found = std::ranges::find_if(archive.sections, [&](const auto& section) {
        return section.name == name;
    });
    if (found == archive.sections.end()) {
        throw std::invalid_argument("generic recompute archive section is missing");
    }
    return *found;
}

std::string sectionDigest(const std::vector<App::GeometryArchiveSection>& sections)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto& section : sections) {
        const std::uint64_t nameSize = section.name.size();
        const std::uint64_t byteSize = section.bytes.size();
        hash.addData(reinterpret_cast<const char*>(&nameSize), sizeof(nameSize));
        hash.addData(section.name.data(), static_cast<qsizetype>(section.name.size()));
        hash.addData(reinterpret_cast<const char*>(&byteSize), sizeof(byteSize));
        if (!section.bytes.empty()) {
            hash.addData(reinterpret_cast<const char*>(section.bytes.data()),
                         static_cast<qsizetype>(section.bytes.size()));
        }
    }
    return hash.result().toHex().toStdString();
}

std::vector<std::pair<std::string, App::Property*>> namedProperties(
    const App::DocumentObject& object)
{
    std::vector<std::pair<const char*, App::Property*>> raw;
    object.getPropertyNamedList(raw);
    std::vector<std::pair<std::string, App::Property*>> result;
    result.reserve(raw.size());
    for (const auto& [name, property] : raw) {
        if (!name || !property) {
            throw std::runtime_error("generic recompute encountered an unnamed property");
        }
        result.emplace_back(name, property);
    }
    const auto nameProjection = [](const auto& entry) -> const std::string& {
        return entry.first;
    };
    std::ranges::sort(result, {}, nameProjection);
    if (std::ranges::adjacent_find(result, {}, nameProjection) != result.end()) {
        throw std::runtime_error("generic recompute encountered duplicate property names");
    }
    return result;
}

bool isPartFeatureShapeRecomputeOutput(const App::DocumentObject& object,
                                       const App::Property& property)
{
    const Base::Type partFeatureType = Base::Type::fromName("Part::Feature");
    return !partFeatureType.isBad()
        && object.getTypeId().isDerivedFrom(partFeatureType)
        && object.getPropertyByName("Shape") == &property;
}

bool usesAuthoritativeTransientRecomputeSchema(const App::DocumentObject& object)
{
    // Spreadsheet cell values are derived Prop_NoPersist dynamic properties.
    // They cannot cross the FCStd worker archive, so the isolated execution is
    // used as a fail-closed preflight and the exact built-in Sheet type repeats
    // its deterministic execute() inside the coordinator-owned transaction.
    const Base::Type spreadsheetType = Base::Type::fromName("Spreadsheet::Sheet");
    return !spreadsheetType.isBad() && object.getTypeId() == spreadsheetType;
}

bool isArchiveTransientDynamicProperty(const App::Property& property)
{
    return property.testStatus(App::Property::PropDynamic)
        && property.testStatus(App::Property::PropNoPersist);
}

std::vector<std::pair<std::string, App::Property*>> persistentNamedProperties(
    const App::DocumentObject& object)
{
    auto properties = namedProperties(object);
    if (usesAuthoritativeTransientRecomputeSchema(object)) {
        std::erase_if(properties, [](const auto& entry) {
            return isArchiveTransientDynamicProperty(*entry.second);
        });
    }
    return properties;
}

bool isDeclaredRecomputeOutput(const App::DocumentObject& object,
                               const App::Property& property)
{
    // Part::Feature::execute() touches Shape even for the plain assignable
    // feature, while legacy edit/touch behavior requires Shape to remain a
    // normal (non-Prop_Output) property.  Keep this compatibility contract
    // narrow: exact registered Part ancestry plus the built-in Shape member.
    const auto expressions = object.ExpressionEngine.getExpressions();
    const bool expressionOutput = std::ranges::any_of(
        expressions, [&property](const auto& expression) {
            return expression.first.getProperty() == &property;
        });
    return object.isOutputProperty(&property)
        || expressionOutput
        || isPartFeatureShapeRecomputeOutput(object, property);
}

ObjectManifest manifestFor(const App::DocumentObject& object)
{
    if (!object.getNameInDocument()) {
        throw std::runtime_error("generic recompute object is not attached");
    }
    ObjectManifest result;
    result.name = object.getNameInDocument();
    result.type = object.getTypeId().getName();
    for (const auto& [name, property] : persistentNamedProperties(object)) {
        result.properties.push_back(
            {name,
             std::string(property->getTypeId().getName()),
             isDeclaredRecomputeOutput(object, *property)});
    }
    return result;
}

std::vector<ObjectManifest> manifestsFor(const std::vector<App::DocumentObject*>& objects)
{
    if (objects.size() > MaxObjects) {
        throw std::invalid_argument("generic recompute closure contains too many objects");
    }
    std::vector<ObjectManifest> result;
    result.reserve(objects.size());
    std::size_t propertyCount = 0;
    for (const auto* object : objects) {
        if (!object) {
            throw std::invalid_argument("generic recompute closure contains a null object");
        }
        auto manifest = manifestFor(*object);
        if (manifest.properties.size() > MaxProperties - propertyCount) {
            throw std::invalid_argument("generic recompute closure contains too many properties");
        }
        propertyCount += manifest.properties.size();
        result.push_back(std::move(manifest));
    }
    std::ranges::sort(result, {}, &ObjectManifest::name);
    if (std::ranges::adjacent_find(result, {}, &ObjectManifest::name) != result.end()) {
        throw std::invalid_argument("generic recompute closure contains duplicate object names");
    }
    return result;
}

std::vector<std::uint8_t> encodeParameters(
    const std::string& target,
    const std::vector<ObjectManifest>& manifests)
{
    std::vector<std::uint8_t> result;
    appendU32(result, ProtocolMagic);
    appendU32(result, ProtocolVersion);
    appendString(result, target);
    appendU32(result, static_cast<std::uint32_t>(manifests.size()));
    for (const auto& object : manifests) {
        appendString(result, object.name);
        appendString(result, object.type);
        appendU32(result, static_cast<std::uint32_t>(object.properties.size()));
        for (const auto& property : object.properties) {
            appendString(result, property.name);
            appendString(result, property.type);
            appendU32(result, property.output ? 1U : 0U);
        }
    }
    return result;
}

std::pair<std::string, std::vector<ObjectManifest>> decodeParameters(
    const std::vector<std::uint8_t>& bytes)
{
    BinaryReader reader(bytes);
    if (reader.u32() != ProtocolMagic || reader.u32() != ProtocolVersion) {
        throw std::invalid_argument("generic recompute protocol header is invalid");
    }
    std::string target = reader.string();
    if (target.empty()) {
        throw std::invalid_argument("generic recompute target is empty");
    }
    const auto objectCount = static_cast<std::size_t>(reader.u32());
    if (objectCount == 0 || objectCount > MaxObjects) {
        throw std::invalid_argument("generic recompute object count is invalid");
    }
    std::vector<ObjectManifest> manifests;
    manifests.reserve(objectCount);
    std::size_t propertyCount = 0;
    for (std::size_t objectIndex = 0; objectIndex < objectCount; ++objectIndex) {
        ObjectManifest object;
        object.name = reader.string();
        object.type = reader.string();
        if (object.name.empty() || object.type.empty()) {
            throw std::invalid_argument("generic recompute object manifest is incomplete");
        }
        const auto count = static_cast<std::size_t>(reader.u32());
        if (count > MaxProperties - propertyCount) {
            throw std::invalid_argument("generic recompute property count is invalid");
        }
        propertyCount += count;
        object.properties.reserve(count);
        for (std::size_t propertyIndex = 0; propertyIndex < count; ++propertyIndex) {
            PropertyManifest property;
            property.name = reader.string();
            property.type = reader.string();
            const auto output = reader.u32();
            if (property.name.empty() || property.type.empty() || output > 1) {
                throw std::invalid_argument("generic recompute property manifest is invalid");
            }
            property.output = output == 1;
            object.properties.push_back(std::move(property));
        }
        manifests.push_back(std::move(object));
    }
    reader.finish();
    if (!std::ranges::is_sorted(manifests, {}, &ObjectManifest::name)
        || std::ranges::adjacent_find(manifests, {}, &ObjectManifest::name)
            != manifests.end()) {
        throw std::invalid_argument("generic recompute object manifest is not canonical");
    }
    for (const auto& object : manifests) {
        if (!std::ranges::is_sorted(object.properties, {}, &PropertyManifest::name)
            || std::ranges::adjacent_find(
                   object.properties, {}, &PropertyManifest::name)
                != object.properties.end()) {
            throw std::invalid_argument("generic recompute property manifest is not canonical");
        }
    }
    return {std::move(target), std::move(manifests)};
}

void validateDetachedSchema(
    const App::Document& document,
    const std::vector<ObjectManifest>& expected)
{
    const auto actualObjects = document.getObjects();
    if (actualObjects.size() != expected.size()) {
        throw std::runtime_error("generic recompute changed the detached object count");
    }
    std::map<std::string, const App::DocumentObject*> actualByName;
    for (const auto* object : actualObjects) {
        if (!object || !object->getNameInDocument()
            || !actualByName.emplace(object->getNameInDocument(), object).second) {
            throw std::runtime_error("generic recompute detached object identity is invalid");
        }
    }
    for (const auto& object : expected) {
        const auto found = actualByName.find(object.name);
        if (found == actualByName.end() || found->second->getTypeId().getName() != object.type) {
            throw std::runtime_error("generic recompute changed an object name or type");
        }
        const auto actualProperties = persistentNamedProperties(*found->second);
        if (actualProperties.size() != object.properties.size()) {
            throw std::runtime_error("generic recompute changed a property set");
        }
        for (std::size_t index = 0; index < actualProperties.size(); ++index) {
            const auto& [name, property] = actualProperties[index];
            const auto& expectedProperty = object.properties[index];
            if (name != expectedProperty.name
                || property->getTypeId().getName() != expectedProperty.type
                || isDeclaredRecomputeOutput(*found->second, *property)
                    != expectedProperty.output) {
                throw std::runtime_error("generic recompute changed a property schema");
            }
        }
    }
}

using PropertySnapshots =
    std::map<std::string, std::map<std::string, std::unique_ptr<App::Property>>>;

PropertySnapshots capturePropertySnapshots(
    const App::Document& document,
    const std::vector<ObjectManifest>& manifests)
{
    PropertySnapshots result;
    for (const auto& objectManifest : manifests) {
        const auto* object = document.getObject(objectManifest.name.c_str());
        if (!object) {
            throw std::runtime_error("generic recompute detached object disappeared");
        }
        auto& properties = result[objectManifest.name];
        for (const auto& [name, property] : namedProperties(*object)) {
            // Some legacy Copy() implementations return a base property (for
            // example PropertyAngle inherits PropertyFloat::Copy).  Construct
            // the registered concrete type before pasting so later semantic
            // equality compares like with like.
            std::unique_ptr<App::Property> copy(
                static_cast<App::Property*>(property->getTypeId().createInstance()));
            if (!copy) {
                throw std::runtime_error("generic recompute property cannot be copied: " + name);
            }
            copy->Paste(*property);
            properties.emplace(name, std::move(copy));
        }
    }
    return result;
}

std::string dumpProperty(App::Property& property)
{
    std::ostringstream stream(std::ios::out | std::ios::binary);
    property.dumpToStream(stream, 1);
    return stream.str();
}

bool sameSerializedProperty(const App::Property& left, const App::Property& right)
{
    if (left.getTypeId() != right.getTypeId()) {
        return false;
    }

    // Prefer the property's semantic equality contract.  Archive round-trips
    // may legitimately normalize non-value serialization details (for
    // example PropertyQuantity's display format), which must not be reported
    // as an undeclared recompute side effect.
    if (left.isSame(right)) {
        return true;
    }

    // Property's base implementation also compares allocation-sensitive
    // memory usage.  Keep the serialized fallback for property types that do
    // not provide a value-aware isSame() override.
    Base::StringWriter leftWriter;
    Base::StringWriter rightWriter;
    left.Save(leftWriter);
    right.Save(rightWriter);
    return leftWriter.getString() == rightWriter.getString();
}

std::vector<std::uint8_t> encodeOutputs(
    const std::string& targetName,
    const App::DocumentObject& target,
    const ObjectManifest& targetManifest,
    const PropertySnapshots& baseline)
{
    struct OutputValue
    {
        std::string name;
        std::string type;
        std::string bytes;
    };
    std::vector<OutputValue> changed;
    const auto& targetBaseline = baseline.at(targetName);
    for (const auto& propertyManifest : targetManifest.properties) {
        auto* property = target.getPropertyByName(propertyManifest.name.c_str());
        if (!property) {
            throw std::runtime_error("generic recompute output property disappeared");
        }
        if (!propertyManifest.output) {
            continue;
        }
        if (property->isDerivedFrom<App::PropertyLinkBase>()
            || property->isDerivedFrom<App::PropertyPythonObject>()) {
            throw std::runtime_error(
                "generic recompute refuses structural or Python output property: "
                + propertyManifest.name);
        }
        if (!sameSerializedProperty(
                *property, *targetBaseline.at(propertyManifest.name))) {
            changed.push_back(
                {propertyManifest.name, propertyManifest.type, dumpProperty(*property)});
        }
    }

    std::vector<std::uint8_t> result;
    appendU32(result, ProtocolMagic);
    appendU32(result, ProtocolVersion);
    appendString(result, targetName);
    appendU32(result, 1);
    appendU32(result, static_cast<std::uint32_t>(changed.size()));
    for (const auto& output : changed) {
        appendString(result, output.name);
        appendString(result, output.type);
        appendBytes(result, output.bytes);
    }
    return result;
}

std::vector<std::uint8_t> encodeFailure(
    const std::string& targetName,
    std::string diagnostic)
{
    if (diagnostic.empty()) {
        diagnostic = "detached feature recompute failed";
    }
    std::vector<std::uint8_t> result;
    appendU32(result, ProtocolMagic);
    appendU32(result, ProtocolVersion);
    appendString(result, targetName);
    appendU32(result, 0);
    appendString(result, diagnostic);
    return result;
}

std::unique_ptr<App::Property> restoreProperty(
    const std::string& typeName,
    const std::string& bytes)
{
    const Base::Type type = Base::Type::getTypeIfDerivedFrom(
        typeName, App::Property::getClassTypeId(), true);
    if (type.isBad()) {
        throw std::invalid_argument("generic recompute output property type is not trusted");
    }
    std::unique_ptr<App::Property> result(static_cast<App::Property*>(type.createInstance()));
    if (!result) {
        throw std::invalid_argument("generic recompute output property type cannot be created");
    }
    std::istringstream stream(bytes, std::ios::in | std::ios::binary);
    result->restoreFromStream(stream);
    return result;
}

class GenericRecomputeOperation final: public App::CollaborativeOperation
{
public:
    GenericRecomputeOperation(std::string target,
                              std::string stableIdentity,
                              std::vector<DecodedOutput> outputs,
                              const bool authoritativeTransientSchema)
        : _target(std::move(target))
        , _stableIdentity(std::move(stableIdentity))
        , _outputs(std::move(outputs))
        , _authoritativeTransientSchema(authoritativeTransientSchema)
    {}

    GenericRecomputeOperation(std::string target,
                              std::string stableIdentity,
                              std::string failureDiagnostic)
        : _target(std::move(target))
        , _stableIdentity(std::move(stableIdentity))
        , _failureDiagnostic(std::move(failureDiagnostic))
    {}

    std::string_view typeId() const noexcept override
    {
        return App::GenericIsolatedRecomputeOperationType;
    }

    void apply(App::Document& document) const override
    {
        auto* target = requireTarget(document);
        if (_failureDiagnostic) {
            App::Internal::GenericIsolatedRecomputeAccess::applyFailure(
                document, *target, *_failureDiagnostic);
            return;
        }
        _applied = false;
        _appliedOutputs.clear();
        if (_authoritativeTransientSchema) {
            if (!usesAuthoritativeTransientRecomputeSchema(*target)) {
                throw std::runtime_error(
                    "generic recompute transient-schema target contract no longer matches");
            }
            Base::ObjectStatusLocker<App::Document::Status, App::Document> recomputing(
                App::Document::Recomputing, &document);
            const int result =
                App::Internal::GenericIsolatedRecomputeAccess::executeAuthoritative(
                    document, *target);
            if (result != 0) {
                const char* diagnostic = document.getErrorDescription(target);
                throw std::runtime_error(
                    diagnostic && *diagnostic
                        ? std::string("authoritative transient-schema recompute failed: ")
                            + diagnostic
                        : "authoritative transient-schema recompute failed");
            }
        }
        for (const auto& output : _outputs) {
            auto* property = target->getPropertyByName(output.name.c_str());
            if (!property || property->getTypeId().getName() != output.type
                || !isDeclaredRecomputeOutput(*target, *property)
                || property->isDerivedFrom<App::PropertyLinkBase>()
                || property->isDerivedFrom<App::PropertyPythonObject>()) {
                throw std::runtime_error(
                    "generic recompute live output contract no longer matches");
            }
            property->Paste(*output.value);
        }
        target->purgeError();
        target->purgeTouched();
        _appliedOutputs.reserve(_outputs.size());
        for (const auto& output : _outputs) {
            auto* property = target->getPropertyByName(output.name.c_str());
            if (!property) {
                throw std::runtime_error(
                    "generic recompute could not snapshot its applied output");
            }
            std::unique_ptr<App::Property> snapshot(
                static_cast<App::Property*>(property->getTypeId().createInstance()));
            if (!snapshot) {
                throw std::runtime_error(
                    "generic recompute could not create its applied-output snapshot");
            }
            // Keep a semantic property snapshot. Persistence archives include
            // container timestamps and are not stable postcondition values.
            snapshot->Paste(*property);
            _appliedOutputs.push_back(std::move(snapshot));
        }
        _applied = true;
    }

    App::CollaborativePostconditionResult checkPostcondition(
        const App::Document& document) const override
    {
        try {
            auto* target = requireTarget(document);
            if (_failureDiagnostic) {
                const bool failureStateApplied =
                    !target->isValid() && target->mustRecompute();
                return {failureStateApplied,
                        failureStateApplied
                            ? std::string {}
                            : "generic recompute failure state was not applied"};
            }
            if (!_applied || _appliedOutputs.size() != _outputs.size()) {
                return {false, "generic recompute output was not applied"};
            }
            for (std::size_t index = 0; index < _outputs.size(); ++index) {
                const auto& output = _outputs[index];
                auto* property = target->getPropertyByName(output.name.c_str());
                if (!property || property->getTypeId().getName() != output.type
                    || !sameSerializedProperty(*property, *_appliedOutputs[index])) {
                    return {false, "generic recompute output postcondition failed"};
                }
            }
            if (!target->isValid() || target->mustRecompute()) {
                return {false, "generic recompute target did not reach a clean valid state"};
            }
            return {true, {}};
        }
        catch (const std::exception& error) {
            return {false, error.what()};
        }
    }

    bool recomputeOutcomeSucceeded() const noexcept override
    {
        return !_failureDiagnostic.has_value();
    }

    std::string_view recomputeOutcomeDiagnostic() const noexcept override
    {
        return _failureDiagnostic ? std::string_view(*_failureDiagnostic)
                                  : std::string_view {};
    }

private:
    App::DocumentObject* requireTarget(const App::Document& document) const
    {
        auto* target = document.getObject(_target.c_str());
        if (!target
            || document.collaborationObjectIdentity(*target) != _stableIdentity) {
            throw std::runtime_error("generic recompute target identity is stale");
        }
        return target;
    }

    std::string _target;
    std::string _stableIdentity;
    std::vector<DecodedOutput> _outputs;
    bool _authoritativeTransientSchema {false};
    std::optional<std::string> _failureDiagnostic;
    mutable bool _applied {false};
    mutable std::vector<std::unique_ptr<App::Property>> _appliedOutputs;
};

class GenericRecomputeBookkeepingOperation final: public App::CollaborativeOperation
{
public:
    GenericRecomputeBookkeepingOperation(std::string target,
                                         std::string stableIdentity)
        : _target(std::move(target))
        , _stableIdentity(std::move(stableIdentity))
    {}

    std::string_view typeId() const noexcept override
    {
        return App::GenericIsolatedRecomputeOperationType;
    }

    void apply(App::Document& document) const override
    {
        auto* target = requireTarget(document);
        if (target->mustRecompute()) {
            throw std::runtime_error(
                "generic recompute bookkeeping target became executable before commit");
        }
        _applied = false;
        target->purgeTouched();
        _applied = true;
    }

    App::CollaborativePostconditionResult checkPostcondition(
        const App::Document& document) const override
    {
        try {
            const auto* target = requireTarget(document);
            const bool satisfied = _applied && !target->isTouched()
                && target->mustRecompute() == 0;
            return {satisfied,
                    satisfied
                        ? std::string {}
                        : "generic recompute bookkeeping did not clear the touch state"};
        }
        catch (const std::exception& error) {
            return {false, error.what()};
        }
    }

private:
    App::DocumentObject* requireTarget(const App::Document& document) const
    {
        auto* target = document.getObject(_target.c_str());
        if (!target
            || document.collaborationObjectIdentity(*target) != _stableIdentity) {
            throw std::runtime_error("generic recompute bookkeeping target identity is stale");
        }
        return target;
    }

    std::string _target;
    std::string _stableIdentity;
    mutable bool _applied {false};
};

std::unique_ptr<const App::CollaborativeOperation> decodeResult(
    const App::GeometryArchive& archive,
    const std::string& target,
    const std::string& stableIdentity,
    const std::map<std::string, std::string>& expectedOutputs,
    const bool authoritativeTransientSchema)
{
    const auto& section = requireSection(archive, "recompute.outputs", 1);
    BinaryReader reader(section.bytes);
    if (reader.u32() != ProtocolMagic || reader.u32() != ProtocolVersion
        || reader.string() != target) {
        throw std::invalid_argument("generic recompute output binding is invalid");
    }
    const auto succeeded = reader.u32();
    if (succeeded > 1) {
        throw std::invalid_argument("generic recompute output status is invalid");
    }
    if (succeeded == 0) {
        std::string diagnostic = reader.string();
        if (diagnostic.empty()) {
            throw std::invalid_argument("generic recompute failure diagnostic is empty");
        }
        reader.finish();
        return std::make_unique<const GenericRecomputeOperation>(
            target, stableIdentity, std::move(diagnostic));
    }
    const auto count = static_cast<std::size_t>(reader.u32());
    if (count > expectedOutputs.size()) {
        throw std::invalid_argument("generic recompute returned too many output properties");
    }
    std::set<std::string> seen;
    std::vector<DecodedOutput> outputs;
    outputs.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::string name = reader.string();
        std::string type = reader.string();
        std::string value = reader.value();
        const auto expected = expectedOutputs.find(name);
        if (expected == expectedOutputs.end() || expected->second != type
            || !seen.insert(name).second) {
            throw std::invalid_argument("generic recompute returned an undeclared output");
        }
        outputs.push_back({std::move(name), type, restoreProperty(type, value)});
    }
    reader.finish();
    return std::make_unique<const GenericRecomputeOperation>(
        target,
        stableIdentity,
        std::move(outputs),
        authoritativeTransientSchema);
}

std::vector<App::DocumentRevisionPublicationRequest> decodeLegacyPublicationEffects(
    const App::GeometryArchive& archive,
    const std::string& target,
    const std::map<std::string, std::string>& expectedOutputs,
    std::vector<App::DocumentRevisionPublicationRequest> effects)
{
    const auto& section = requireSection(archive, "recompute.outputs", 1);
    BinaryReader reader(section.bytes);
    if (reader.u32() != ProtocolMagic || reader.u32() != ProtocolVersion
        || reader.string() != target) {
        throw std::invalid_argument("generic recompute output binding is invalid");
    }
    const auto succeeded = reader.u32();
    if (succeeded > 1) {
        throw std::invalid_argument("generic recompute output status is invalid");
    }
    if (succeeded == 0) {
        const std::string diagnostic = reader.string();
        if (diagnostic.empty()) {
            throw std::invalid_argument("generic recompute failure diagnostic is empty");
        }
        reader.finish();
        std::erase_if(effects, [&](const auto& effect) {
            return effect.key != App::DocumentRevisionKey::objectModel(target)
                && effect.key.kind
                    != App::DocumentRevisionKind::UnknownModelMutation;
        });
        for (auto& effect : effects) {
            effect.revisionDelta = 1;
        }
        return effects;
    }
    const auto count = static_cast<std::size_t>(reader.u32());
    if (count > expectedOutputs.size()) {
        throw std::invalid_argument("generic recompute returned too many output properties");
    }
    std::set<std::string> seen;
    for (std::size_t index = 0; index < count; ++index) {
        const std::string name = reader.string();
        const std::string type = reader.string();
        static_cast<void>(reader.value());
        const auto expected = expectedOutputs.find(name);
        if (expected == expectedOutputs.end() || expected->second != type
            || !seen.insert(name).second) {
            throw std::invalid_argument("generic recompute returned an undeclared output");
        }
    }
    reader.finish();

    const auto outputDelta = static_cast<App::DocumentRevision>(count) + 2;
    for (auto& effect : effects) {
        if (effect.key == App::DocumentRevisionKey::objectModel(target)) {
            effect.revisionDelta = outputDelta;
        }
        else if (effect.key.kind
                 == App::DocumentRevisionKind::UnknownModelMutation) {
            effect.revisionDelta = 2;
        }
    }
    return effects;
}

std::vector<App::DocumentObject*> collectClosure(
    const App::Document& document,
    App::DocumentObject& target)
{
    std::vector<App::DocumentObject*> pending {&target};
    std::unordered_set<App::DocumentObject*> seen;
    std::vector<App::DocumentObject*> closure;
    while (!pending.empty()) {
        auto* object = pending.back();
        pending.pop_back();
        if (!seen.insert(object).second) {
            continue;
        }
        if (!object || object->getDocument() != &document
            || !object->isAttachedToDocument()) {
            throw std::invalid_argument(
                "generic recompute has an unresolved cross-document dependency");
        }
        if (!object->canRecomputeOnWorker()) {
            throw std::invalid_argument(
                "generic recompute object has not opted into isolated execution: "
                + std::string(object->getNameInDocument()));
        }
        std::unique_ptr<App::DocumentObject> registeredType(
            static_cast<App::DocumentObject*>(object->getTypeId().createInstance()));
        if (!registeredType || typeid(*registeredType) != typeid(*object)) {
            throw std::invalid_argument(
                "generic recompute object runtime type is not serializable: "
                + std::string(object->getNameInDocument()));
        }
        closure.push_back(object);
        for (auto* dependency : object->getOutList()) {
            if (!dependency || dependency->getDocument() != &document) {
                throw std::invalid_argument(
                    "generic recompute has an unresolved cross-document dependency");
            }
            pending.push_back(dependency);
        }
    }
    std::ranges::sort(closure, [](const auto* left, const auto* right) {
        return std::string_view(left->getNameInDocument())
            < std::string_view(right->getNameInDocument());
    });
    return closure;
}

App::CollaborativeOperationPreparation prepareGenericRecompute(
    const App::Document& document,
    const App::CollaborativeOperationIntent& intent)
{
    const auto legacyMode = intent.arguments.find("legacy_revision_semantics");
    const auto forceMode = intent.arguments.find("force_execution");
    if (intent.arguments.empty() || intent.arguments.size() > 3
        || !intent.arguments.contains("feature")
        || std::ranges::any_of(intent.arguments, [](const auto& argument) {
               return argument.first != "feature"
                   && argument.first != "legacy_revision_semantics"
                   && argument.first != "force_execution";
           })) {
        throw std::invalid_argument(
            "generic recompute requires a feature and optional revision/force modes");
    }
    const bool preserveLegacyRevisionSemantics = legacyMode != intent.arguments.end();
    if (preserveLegacyRevisionSemantics && legacyMode->second != "1") {
        throw std::invalid_argument("generic recompute revision mode is invalid");
    }
    const bool forceExecution = forceMode != intent.arguments.end();
    if (forceExecution && forceMode->second != "1") {
        throw std::invalid_argument("generic recompute force mode is invalid");
    }
    const std::string targetName = intent.arguments.at("feature");
    auto* target = document.getObject(targetName.c_str());
    if (!target || !target->isAttachedToDocument() || target->getDocument() != &document) {
        throw std::invalid_argument("generic recompute target does not exist");
    }

    if (!forceExecution && target->isTouched() && target->mustRecompute() == 0) {
        const std::string stableIdentity = document.collaborationObjectIdentity(*target);
        std::vector<App::DocumentRevisionKey> reads {
            App::DocumentRevisionKey::objectExistence(targetName),
            App::DocumentRevisionKey::objectStructure(targetName),
            App::DocumentRevisionKey::objectModel(targetName),
            App::DocumentRevisionKey::unknownModelMutation()};
        for (const auto& [propertyName, property] : namedProperties(*target)) {
            static_cast<void>(property);
            reads.push_back(
                App::DocumentRevisionKey::objectProperty(targetName, propertyName));
        }
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());

        std::vector<App::DocumentRevisionKey> writes {
            App::DocumentRevisionKey::objectModel(targetName)};
        std::vector<App::DocumentRevisionPublicationRequest> effects {
            {App::DocumentRevisionKey::objectModel(targetName), stableIdentity}};
        if (preserveLegacyRevisionSemantics) {
            writes.push_back(App::DocumentRevisionKey::unknownModelMutation());
            effects.push_back(
                {App::DocumentRevisionKey::unknownModelMutation(), std::nullopt});
        }
        App::CollaborativeOperationPreparation::DetachedTask task =
            [targetName, stableIdentity](const std::stop_token stopToken) {
                if (stopToken.stop_requested()) {
                    throw std::runtime_error(
                        "generic recompute bookkeeping preparation was cancelled");
                }
                return std::make_unique<const GenericRecomputeBookkeepingOperation>(
                    targetName, stableIdentity);
            };
        return {std::move(reads),
                std::move(writes),
                std::move(effects),
                std::move(task),
                App::PreparationPolicy::DetachedInProcess};
    }

    auto closure = collectClosure(document, *target);
    const auto manifests = manifestsFor(closure);
    const auto targetManifest = std::ranges::find(
        manifests, targetName, &ObjectManifest::name);
    if (targetManifest == manifests.end()) {
        throw std::logic_error("generic recompute target is absent from its closure");
    }

    std::map<std::string, std::string> expectedOutputs;
    for (const auto& propertyManifest : targetManifest->properties) {
        if (!propertyManifest.output) {
            continue;
        }
        auto* property = target->getPropertyByName(propertyManifest.name.c_str());
        if (!property || property->isDerivedFrom<App::PropertyLinkBase>()
            || property->isDerivedFrom<App::PropertyPythonObject>()) {
            throw std::invalid_argument(
                "generic recompute refuses structural or Python output property: "
                + propertyManifest.name);
        }
        expectedOutputs.emplace(propertyManifest.name, propertyManifest.type);
    }

    std::ostringstream snapshot(std::ios::out | std::ios::binary);
    const_cast<App::Document&>(document).exportObjectsForIsolatedRecompute(
        closure, snapshot);
    const std::string snapshotBytes = snapshot.str();
    if (snapshotBytes.empty() || snapshotBytes.size() > MaxPayloadBytes) {
        throw std::invalid_argument("generic recompute document closure is empty or oversized");
    }

    App::GeometryArchive input;
    const auto* snapshotBegin = reinterpret_cast<const std::uint8_t*>(snapshotBytes.data());
    input.sections.push_back(
        {"document.fcstd",
         std::vector<std::uint8_t>(snapshotBegin, snapshotBegin + snapshotBytes.size())});
    input.sections.push_back({"recompute.params", encodeParameters(targetName, manifests)});

    std::vector<App::DocumentRevisionKey> reads {
        App::DocumentRevisionKey::documentStructure(),
        App::DocumentRevisionKey::unknownModelMutation()};
    for (const auto* object : closure) {
        const std::string name = object->getNameInDocument();
        reads.push_back(App::DocumentRevisionKey::objectExistence(name));
        reads.push_back(App::DocumentRevisionKey::objectStructure(name));
        reads.push_back(App::DocumentRevisionKey::objectModel(name));
        for (const auto& [propertyName, property] : namedProperties(*object)) {
            static_cast<void>(property);
            reads.push_back(App::DocumentRevisionKey::objectProperty(name, propertyName));
        }
    }
    std::sort(reads.begin(), reads.end());
    reads.erase(std::unique(reads.begin(), reads.end()), reads.end());

    std::vector<App::DocumentRevisionKey> writes {
        App::DocumentRevisionKey::objectModel(targetName)};
    std::vector<App::DocumentRevisionPublicationRequest> effects {
        {App::DocumentRevisionKey::objectModel(targetName),
         document.collaborationObjectIdentity(*target)}};
    for (const auto& [name, type] : expectedOutputs) {
        static_cast<void>(type);
        auto key = App::DocumentRevisionKey::objectProperty(targetName, name);
        writes.push_back(key);
        effects.push_back({std::move(key), document.collaborationObjectIdentity(*target)});
    }
    if (preserveLegacyRevisionSemantics) {
        writes.push_back(App::DocumentRevisionKey::unknownModelMutation());
        effects.push_back(
            {App::DocumentRevisionKey::unknownModelMutation(), std::nullopt});
    }

    App::GeometryJobRequest request;
    request.operationType = std::string(App::GenericIsolatedRecomputeOperationType);
    request.policy = App::PreparationPolicy::IsolatedProcess;
    request.coalescingKey = document.collaborationObjectIdentity(*target);
    request.inputDigest = sectionDigest(input.sections);
    request.coalescing = App::GeometryJobCoalescing::LatestWins;
    request.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);

    auto publicationEffectsTemplate = effects;
    auto operationExpectedOutputs = expectedOutputs;
    const bool authoritativeTransientSchema =
        usesAuthoritativeTransientRecomputeSchema(*target);
    App::CollaborativeOperationPreparation::IsolatedTask isolated {
        std::move(request),
        std::move(input),
        [targetName,
         stableIdentity = document.collaborationObjectIdentity(*target),
         expectedOutputs = std::move(operationExpectedOutputs),
         authoritativeTransientSchema](
            const App::GeometryArchive& output) {
            return decodeResult(output,
                                targetName,
                                stableIdentity,
                                expectedOutputs,
                                authoritativeTransientSchema);
        },
        preserveLegacyRevisionSemantics
            ? App::CollaborativeOperationPreparation::IsolatedPublicationEffectDecoder(
                  [targetName,
                   expectedOutputs,
                   effects = std::move(publicationEffectsTemplate)](
                      const App::GeometryArchive& output) mutable {
                      return decodeLegacyPublicationEffects(
                          output, targetName, expectedOutputs, effects);
                  })
            : App::CollaborativeOperationPreparation::IsolatedPublicationEffectDecoder {}};
    return {std::move(reads), std::move(writes), std::move(effects), std::move(isolated)};
}

App::GeometryArchive executeGenericRecompute(
    const App::GeometryArchive& input,
    const std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        throw std::runtime_error("generic recompute cancelled before document import");
    }
    const auto& documentSection = requireSection(input, "document.fcstd", 2);
    const auto& parameterSection = requireSection(input, "recompute.params", 2);
    auto [targetName, manifests] = decodeParameters(parameterSection.bytes);

    App::DocumentInitFlags flags;
    flags.createView = false;
    flags.temporary = true;
    App::Document* detached = App::GetApplication().newDocument(
        "GenericRecomputeWorker", nullptr, flags);
    if (!detached) {
        throw std::runtime_error("generic recompute could not create its detached document");
    }
    const std::string detachedName = detached->getName();
    BOOST_SCOPE_EXIT_ALL(&) {
        try {
            static_cast<void>(App::GetApplication().closeDocument(detachedName.c_str()));
        }
        catch (...) {
        }
    };

    std::string archiveBytes(
        reinterpret_cast<const char*>(documentSection.bytes.data()),
        documentSection.bytes.size());
    std::istringstream archiveStream(archiveBytes, std::ios::in | std::ios::binary);
    App::MergeDocuments importer(detached);
    static_cast<void>(importer.importObjects(archiveStream));
    validateDetachedSchema(*detached, manifests);
    auto baseline = capturePropertySnapshots(*detached, manifests);

    auto* target = detached->getObject(targetName.c_str());
    if (!target || !target->canRecomputeOnWorker()) {
        throw std::runtime_error("generic recompute target is not worker-safe after import");
    }
    if (stopToken.stop_requested()) {
        throw std::runtime_error("generic recompute cancelled before feature execution");
    }
    const int result = App::Internal::GenericIsolatedRecomputeAccess::execute(
        *detached, *target);
    const char* failureDescription = result == 0
        ? nullptr
        : detached->getErrorDescription(target);
    const std::string failureDiagnostic = result == 0
        ? std::string {}
        : failureDescription && *failureDescription
            ? std::string(failureDescription)
            : "detached feature recompute failed";
    if (stopToken.stop_requested()) {
        throw std::runtime_error("generic recompute cancelled after feature execution");
    }

    validateDetachedSchema(*detached, manifests);
    if (result != 0) {
        App::GeometryArchive output;
        output.sections.push_back(
            {"recompute.outputs", encodeFailure(targetName, failureDiagnostic)});
        return output;
    }
    const auto targetManifest = std::ranges::find(
        manifests, targetName, &ObjectManifest::name);
    if (targetManifest == manifests.end()) {
        throw std::runtime_error("generic recompute target manifest disappeared");
    }
    for (const auto& objectManifest : manifests) {
        const auto* object = detached->getObject(objectManifest.name.c_str());
        for (const auto& propertyManifest : objectManifest.properties) {
            const auto* property = object->getPropertyByName(propertyManifest.name.c_str());
            const bool allowedOutput = objectManifest.name == targetName
                && propertyManifest.output;
            if (!allowedOutput
                && !sameSerializedProperty(
                    *property,
                    *baseline.at(objectManifest.name).at(propertyManifest.name))) {
                throw std::runtime_error(
                    "generic recompute produced an undeclared property side effect: "
                    + objectManifest.name + "." + propertyManifest.name);
            }
        }
    }

    App::GeometryArchive output;
    output.sections.push_back(
        {"recompute.outputs",
         encodeOutputs(targetName, *target, *targetManifest, baseline)});
    return output;
}

}  // namespace

namespace App::Internal
{

void ensureGenericIsolatedRecomputeRegistered()
{
    static std::once_flag registered;
    std::call_once(registered, [] {
        static_cast<void>(CollaborativeOperationRegistrar::registerAdapter(
            std::string(GenericIsolatedRecomputeOperationType), prepareGenericRecompute));
        GeometryWorkerOperationRegistry::instance().registerOperation(
            std::string(GenericIsolatedRecomputeOperationType), executeGenericRecompute);
    });
}

DocumentRecomputeRequest makeGenericIsolatedRecomputeRequest(
    Document& document,
    const std::vector<DocumentObject*>& features,
    const std::string_view provenance,
    const std::string_view coalescingPrefix,
    const bool preserveLegacyRevisionSemantics,
    const bool forceExecution)
{
    std::map<std::string, DocumentObject*> byName;
    for (auto* object : features) {
        if (!object || object->getDocument() != &document || !object->isAttachedToDocument()
            || !object->getNameInDocument()) {
            throw std::invalid_argument(
                "generic recompute plan contains a detached or cross-document feature");
        }
        if (!byName.emplace(object->getNameInDocument(), object).second) {
            throw std::invalid_argument("generic recompute plan contains a duplicate feature");
        }
    }

    DocumentRecomputeRequest request;
    request.coalescingKey = std::string(coalescingPrefix);
    // Compatibility recompute publishes the broad UnknownModelMutation key.
    // Preparing every independent node against the same revision would make
    // the first successful sibling commit stale all remaining siblings. Keep
    // detached execution pointer-free while refreshing each later capture
    // after the preceding coordinator-owned commit.
    request.refreshRevisionFenceAfterEachCommit = preserveLegacyRevisionSemantics;
    for (const auto& [name, object] : byName) {
        DocumentRecomputeFeatureRequest node;
        node.featureId = name;
        node.operationId = "generic-recompute:" + name;
        node.intent.operationType = std::string(GenericIsolatedRecomputeOperationType);
        node.intent.arguments.emplace("feature", name);
        if (preserveLegacyRevisionSemantics) {
            node.intent.arguments.emplace("legacy_revision_semantics", "1");
        }
        if (forceExecution) {
            node.intent.arguments.emplace("force_execution", "1");
        }
        node.provenance = std::string(provenance);
        for (auto* dependency : object->getOutList()) {
            if (dependency && dependency->getNameInDocument()
                && byName.contains(dependency->getNameInDocument())) {
                node.dependencies.emplace_back(dependency->getNameInDocument());
            }
        }
        std::ranges::sort(node.dependencies);
        node.dependencies.erase(
            std::unique(node.dependencies.begin(), node.dependencies.end()),
            node.dependencies.end());
        request.coalescingKey += name + ";";
        request.features.push_back(std::move(node));
    }
    return request;
}

DocumentRecomputeRequest makeGenericIsolatedRecomputeRequest(
    Document& document,
    DocumentObject& feature,
    const bool recursive,
    const bool preserveLegacyRevisionSemantics)
{
    if (!feature.isAttachedToDocument() || feature.getDocument() != &document
        || !feature.getNameInDocument()) {
        throw std::invalid_argument("generic recompute feature is not attached to this document");
    }

    std::vector<DocumentObject*> selected {&feature};
    if (recursive) {
        auto dependents = feature.getInListRecursive();
        selected.insert(selected.end(), dependents.begin(), dependents.end());
    }
    return makeGenericIsolatedRecomputeRequest(
        document,
        selected,
        "App::Document::recomputeFeature isolated adapter",
        "generic-feature:",
        preserveLegacyRevisionSemantics,
        true);
}

}  // namespace App::Internal
