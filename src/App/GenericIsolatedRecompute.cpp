// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GenericIsolatedRecompute.h"

#include "Application.h"
#include "CollaborativeOperation.h"
#include "Document.h"
#include "DocumentObject.h"
#include "GeometryWorkerOperationRegistry.h"
#include "Link.h"
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
# include <QByteArrayView>
#endif

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
    const auto addData = [&hash](const char* data, const std::size_t size) {
#if QT_VERSION < QT_VERSION_CHECK(6, 3, 0)
        hash.addData(data, static_cast<int>(size));
#else
        hash.addData(QByteArrayView(data, static_cast<qsizetype>(size)));
#endif
    };
    for (const auto& section : sections) {
        const std::uint64_t nameSize = section.name.size();
        const std::uint64_t byteSize = section.bytes.size();
        addData(reinterpret_cast<const char*>(&nameSize), sizeof(nameSize));
        addData(section.name.data(), section.name.size());
        addData(reinterpret_cast<const char*>(&byteSize), sizeof(byteSize));
        if (!section.bytes.empty()) {
            addData(reinterpret_cast<const char*>(section.bytes.data()),
                    section.bytes.size());
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

bool isSketchObjectExecuteRecomputeOutput(const App::DocumentObject& object,
                                          const App::Property& property)
{
    // SketchObject::execute() solves into Geometry, rebuilds its transient
    // external-geometry cache, and builds InternalShape before Shape.  These
    // members predate Prop_Output, so preserve that exact built-in contract
    // without linking FreeCADApp to Sketcher or admitting link-valued
    // ExternalGeometry, user-owned Constraints, or derived Python types.
    const Base::Type sketchObjectType = Base::Type::fromName("Sketcher::SketchObject");
    if (sketchObjectType.isBad() || object.getTypeId() != sketchObjectType) {
        return false;
    }
    return object.getPropertyByName("Geometry") == &property
        || object.getPropertyByName("InternalShape") == &property
        || object.getPropertyByName("ExternalGeo") == &property;
}

bool isPartDesignAddSubShapeRecomputeOutput(const App::DocumentObject& object,
                                            const App::Property& property)
{
    // FeatureAddSub implementations build AddSubShape as their execute-owned
    // additive/subtractive tool cache before producing Shape. Keep the
    // compatibility contract to that exact nonstructural member and registered
    // ancestry without introducing an App-to-PartDesign link.
    const Base::Type addSubType = Base::Type::fromName("PartDesign::FeatureAddSub");
    return !addSubType.isBad()
        && object.getTypeId().isDerivedFrom(addSubType)
        && object.getPropertyByName("AddSubShape") == &property;
}

bool isPartDesignSuppressedShapeRecomputeOutput(const App::DocumentObject& object,
                                                 const App::Property& property)
{
    // PartDesign::Feature::recompute() unconditionally clears this cache for
    // active features and rebuilds it for suppressed ones before delegating
    // to execute(). It is therefore an execute-owned geometry output even
    // though the legacy property predates Prop_Output.
    const Base::Type featureType = Base::Type::fromName("PartDesign::Feature");
    return !featureType.isBad()
        && object.getTypeId().isDerivedFrom(featureType)
        && object.getPropertyByName("SuppressedShape") == &property;
}

bool isPartDesignDirectionRecomputeOutput(const App::DocumentObject& object,
                                          const App::Property& property)
{
    // FeatureExtrude::computeDirection() always writes Direction, including
    // custom-vector mode (where a zero vector falls back to the profile normal).
    // Keep this exact built-in property in a stable manifest contract so an
    // expression-driven UseCustomVector mode change cannot alter the schema
    // while the detached recompute is running.
    const Base::Type featureExtrudeType = Base::Type::fromName("PartDesign::FeatureExtrude");
    return !featureExtrudeType.isBad()
        && object.getTypeId().isDerivedFrom(featureExtrudeType)
        && object.getPropertyByName("Direction") == &property;
}

bool isPartDesignProfilePlacementRecomputeOutput(const App::DocumentObject& object,
                                                  const App::Property& property)
{
    // ProfileBased feature execution calls positionByPrevious() and derives
    // the feature Placement from its base feature, support, or sketch. This is
    // an established execute-owned output even though Placement predates
    // Prop_Output. Restrict the compatibility declaration to that ancestry and
    // exact built-in member.
    const Base::Type profileBasedType = Base::Type::fromName("PartDesign::ProfileBased");
    return !profileBasedType.isBad()
        && object.getTypeId().isDerivedFrom(profileBasedType)
        && object.getPropertyByName("Placement") == &property;
}

bool isAttachExtensionPlacementRecomputeOutput(const App::DocumentObject& object,
                                               const App::Property& property)
{
    // AttachExtension::extensionExecute() runs positionBySupport(), which
    // derives the extended object's Placement from the attachment engine and
    // writes it back. Placement predates Prop_Output, so without this
    // declaration an attached feature publishes nothing and the caller keeps
    // the identity placement it started with.
    //
    // Keyed on the registered extension alone, deliberately not on the current
    // MapMode: MapMode is a mutable value and the manifest schema has to stay
    // stable for the whole detached recompute. A deactivated attachment simply
    // leaves Placement unchanged, which publishes nothing either way.
    const Base::Type attachExtensionType = Base::Type::fromName("Part::AttachExtension");
    return !attachExtensionType.isBad()
        && object.hasExtension(attachExtensionType)
        && object.getPropertyByName("Placement") == &property;
}

bool isSafePlainAppLinkBookkeepingTarget(const App::DocumentObject& object)
{
    // A plain native App::Link to a non-Python object has no model execute
    // work when its virtual mustExecute() is false. Its native extension would
    // only touch the transient view notification property; disabled
    // copy-on-change has no setup work. This exact contract lets a newly set
    // persistent link clear its Enforce/Touch bookkeeping without attempting
    // to execute or serialize its dependency graph in the worker.
    // Derived links, arrays, copy-on-change links, unresolved links, and links
    // that can invoke a Python proxy remain fail-closed.
    const Base::Type linkType = Base::Type::fromName("App::Link");
    if (linkType.isBad() || object.getTypeId() != linkType
        || object.mustExecute() != 0) {
        return false;
    }
    const auto* link = dynamic_cast<const App::Link*>(&object);
    if (!link || link->ElementCount.getValue() != 0
        || link->getLinkCopyOnChangeValue()
            != App::LinkBaseExtension::CopyOnChangeDisabled
        || link->getLinkCopyOnChangeSourceValue()
        || link->getLinkCopyOnChangeGroupValue()
        || std::ranges::any_of(namedProperties(object), [](const auto& entry) {
               return entry.second->testStatus(App::Property::PropDynamic);
           })) {
        return false;
    }
    auto* linked = link->getTrueLinkedObject(true);
    if (!linked) {
        return false;
    }
    return !freecad_cast<App::PropertyPythonObject*>(
        linked->getPropertyByName("Proxy"));
}

bool isNullProxyExternalLinkHolderBookkeepingTarget(
    const App::DocumentObject& object)
{
    // An exact App::FeaturePython with no Proxy cannot run Python execute
    // code: FeaturePythonT::execute() falls through to the base
    // DocumentObject no-op. Restrict this compatibility case further to the
    // worker-snapshot pattern it serves: no extensions, and a non-empty
    // dependency set made solely of the safe plain external App::Links above.
    // Any derived Python feature, live Proxy, extension, or other dependency
    // remains isolated and fail-closed.
    const Base::Type featurePythonType = Base::Type::fromName("App::FeaturePython");
    if (featurePythonType.isBad() || object.getTypeId() != featurePythonType
        || object.hasExtensions()
        || !object.ExpressionEngine.getExpressions().empty()) {
        return false;
    }
    const auto* proxy = freecad_cast<App::PropertyPythonObject*>(
        object.getPropertyByName("Proxy"));
    if (!proxy || !proxy->getValue().isNone()) {
        return false;
    }
    if (std::ranges::any_of(namedProperties(object), [proxy](
            const std::pair<std::string, App::Property*>& entry) {
            return entry.second != proxy
                && entry.second->isDerivedFrom<App::PropertyPythonObject>();
        })) {
        return false;
    }
    const auto dependencies = object.getOutList();
    return !dependencies.empty()
        && std::ranges::all_of(dependencies, [&object](const auto* dependency) {
               return dependency
                   && dependency->getDocument() == object.getDocument()
                   && isSafePlainAppLinkBookkeepingTarget(*dependency);
           });
}

bool owesUnconditionalExecuteWork(const App::DocumentObject& object)
{
    // mustExecute() answers "must my dependents re-run because I changed", not
    // "is my own execute() a no-op". The in-process recompute never conflated
    // the two: it executes every touched object regardless. So a class whose
    // execute() does unconditional work is invisible to the mustRecompute()
    // shortcut below, and short-circuiting it purges the touch and silently
    // drops the work the caller asked for. Every such class found so far is
    // listed here; the shortcut stays unsound for any that is not, which is
    // why it is a deny-list on proven offenders rather than a heuristic.
    //
    // AttachExtension::extensionExecute() re-derives the extended object's
    // Placement on every execution -- its isTouched_Mapping() is hardcoded
    // true precisely because the attachment inputs sit behind links whose
    // changes never touch AttachmentSupport itself.
    const Base::Type attachExtensionType = Base::Type::fromName("Part::AttachExtension");
    if (!attachExtensionType.isBad() && object.hasExtension(attachExtensionType)) {
        return true;
    }

    // AssemblyObject::execute() runs the joint solver whenever the
    // SolveOnRecompute preference is set, which is the default. It declares no
    // mustExecute() of its own, so a touched assembly reports 0 and would
    // never solve. It also opts out of worker execution, so today it reaches
    // the in-process branch above before this one; keep the entry so the
    // shortcut cannot silently reclaim it if that opt-out is ever lifted.
    const Base::Type assemblyType = Base::Type::fromName("Assembly::AssemblyObject");
    return !assemblyType.isBad() && object.getTypeId().isDerivedFrom(assemblyType);
}

bool isBookkeepingOnlyTarget(const App::DocumentObject& object)
{
    if (owesUnconditionalExecuteWork(object)) {
        return false;
    }
    return object.mustRecompute() == 0
        || isSafePlainAppLinkBookkeepingTarget(object)
        || isNullProxyExternalLinkHolderBookkeepingTarget(object);
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
    // Transient structural outputs such as PartDesign::Feature::_Body are
    // reconstructed ownership hints, not worker publications.  Keep them in
    // the manifest and baseline so an execute-time mutation still fails
    // closed. Nonstructural transient outputs remain eligible because legacy
    // execute() implementations legitimately update those runtime caches.
    const bool transientStructuralOutput =
        property.testStatus(App::Property::PropTransient)
        && (property.isDerivedFrom<App::PropertyLinkBase>()
            || property.isDerivedFrom<App::PropertyPythonObject>());
    const bool compatibleOutput = object.isOutputProperty(&property)
        && !transientStructuralOutput;
    return compatibleOutput
        || expressionOutput
        || isPartFeatureShapeRecomputeOutput(object, property)
        || isSketchObjectExecuteRecomputeOutput(object, property)
        || isPartDesignAddSubShapeRecomputeOutput(object, property)
        || isPartDesignSuppressedShapeRecomputeOutput(object, property)
        || isPartDesignDirectionRecomputeOutput(object, property)
        || isPartDesignProfilePlacementRecomputeOutput(object, property)
        || isAttachExtensionPlacementRecomputeOutput(object, property);
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

struct PropertySnapshot
{
    std::unique_ptr<App::Property> value;
    std::string serialized;
    // Only populated for doc-file-backed properties, where the plain
    // serialization cannot represent the value.
    std::string archived;
    bool payloadInDocFile {false};
};

using PropertySnapshots =
    std::map<std::string, std::map<std::string, PropertySnapshot>>;

std::string dumpProperty(App::Property& property)
{
    std::ostringstream stream(std::ios::out | std::ios::binary);
    property.dumpToStream(stream, 1);
    return stream.str();
}

// Base::StringWriter::writeFiles() is a no-op, so a property that stores its
// payload through SaveDocFile -- PropertyMeshKernel, Points::PropertyPointKernel,
// PropertyFileIncluded and friends -- serializes to nothing but a constant file
// reference such as <Mesh file="MeshKernel.bms"/>. That string is identical for
// an empty mesh and a finished one, so a serialized comparison silently reports
// "unchanged", the worker publishes no geometry, and the caller keeps whatever
// it started with. Detect the case from the writer's own file requests: a class
// derived from StringWriter can see the requests Save() registered.
class DocFilePayloadProbe final: public Base::StringWriter
{
public:
    bool registeredDocFiles() const
    {
        return !FileList.empty();
    }
};

bool serializesPayloadToDocFile(const App::Property& property)
{
    DocFilePayloadProbe probe;
    property.Save(probe);
    return probe.registeredDocFiles();
}

// Reduce a Persistence archive to what it actually carries: each entry's name
// and its uncompressed content, in archive order. Nothing else -- not the
// timestamp, not the compressed size, not the CRC -- takes part in a value
// comparison.
//
// XML-only comparison is not an option: a property that stores its payload
// through SaveDocFile serializes to a constant file reference, so its XML is
// identical for an empty payload and a computed one.
std::string canonicalArchiveContents(const std::string& archiveBytes)
{
    // ZipWriter stamps the current local time into every entry --
    // zipoutputstreambuf.cpp packs tm_hour/tm_min/(tm_sec >> 1), giving two
    // second resolution -- so two archives of identical content serialized on
    // opposite sides of a boundary differ in bytes alone, and an unchanged
    // input then reads as an undeclared side effect.
    //
    // Walk the records and zero only the modification time and date. Every
    // other byte stays exactly as written: names, sizes, CRCs and the deflate
    // payload all still take part in the comparison, so a real value change is
    // still a change. Anything that does not parse as a zip falls through to
    // the raw bytes rather than collapsing to a value that compares equal.
    const auto size = archiveBytes.size();
    const auto byteAt = [&archiveBytes](const std::size_t at) -> std::uint32_t {
        return static_cast<unsigned char>(archiveBytes[at]);
    };
    const auto u16 = [&byteAt](const std::size_t at) -> std::uint32_t {
        return byteAt(at) | (byteAt(at + 1) << 8);
    };
    const auto u32 = [&byteAt](const std::size_t at) -> std::uint32_t {
        return byteAt(at) | (byteAt(at + 1) << 8) | (byteAt(at + 2) << 16)
            | (byteAt(at + 3) << 24);
    };

    std::string canonical = archiveBytes;
    std::size_t at = 0;
    bool sawEntry = false;
    while (at + 4 <= size) {
        const std::uint32_t signature = u32(at);
        if (signature == 0x04034b50U) {  // local file header
            if (at + 30 > size) {
                return archiveBytes;
            }
            if ((u16(at + 6) & 0x0008U) != 0) {
                // A trailing data descriptor carries the sizes, so the payload
                // cannot be skipped from this header alone. ZipWriter does not
                // emit those, but refuse rather than mis-walk if one appears.
                return archiveBytes;
            }
            canonical[at + 10] = 0;
            canonical[at + 11] = 0;
            canonical[at + 12] = 0;
            canonical[at + 13] = 0;
            const std::size_t advance =
                std::size_t {30} + u16(at + 26) + u16(at + 28) + u32(at + 18);
            if (at + advance > size) {
                return archiveBytes;
            }
            at += advance;
            sawEntry = true;
        }
        else if (signature == 0x02014b50U) {  // central directory header
            if (at + 46 > size) {
                return archiveBytes;
            }
            canonical[at + 12] = 0;
            canonical[at + 13] = 0;
            canonical[at + 14] = 0;
            canonical[at + 15] = 0;
            const std::size_t advance =
                std::size_t {46} + u16(at + 28) + u16(at + 30) + u16(at + 32);
            if (at + advance > size) {
                return archiveBytes;
            }
            at += advance;
        }
        else if (signature == 0x06054b50U) {  // end of central directory
            break;
        }
        else {
            return archiveBytes;
        }
    }
    if (!sawEntry) {
        // Nothing recognizable was normalized, so comparing canonical forms
        // would compare two unchanged copies and tell us nothing new.
        return archiveBytes;
    }
    return canonical;
}

std::string valueArchive(const App::Property& property)
{
    return canonicalArchiveContents(dumpProperty(const_cast<App::Property&>(property)));
}

bool requiresDetachedValueSnapshot(const App::Property& property)
{
    // PropertyPartShape::Paste() is intentionally a cheap value copy and can
    // share mutable TopoShape internals. A baseline made with Paste() can then
    // change together with Shape during execute(), hiding the geometry output.
    // Use the persistence round-trip already trusted for worker outputs to
    // obtain an independent value without linking FreeCADApp to Part.
    const Base::Type partShapeType = Base::Type::fromName("Part::PropertyPartShape");
    return !partShapeType.isBad()
        && property.getTypeId().isDerivedFrom(partShapeType);
}

std::unique_ptr<App::Property> detachedValueSnapshot(App::Property& property)
{
    std::unique_ptr<App::Property> copy(
        static_cast<App::Property*>(property.getTypeId().createInstance()));
    if (!copy) {
        throw std::runtime_error("generic recompute property cannot be copied");
    }
    const auto bytes = dumpProperty(property);
    std::istringstream stream(bytes, std::ios::in | std::ios::binary);
    copy->restoreFromStream(stream);
    return copy;
}

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
            Base::StringWriter writer;
            property->Save(writer);
            const bool payloadInDocFile = !requiresDetachedValueSnapshot(*property)
                && serializesPayloadToDocFile(*property);
            std::unique_ptr<App::Property> copy;
            if (!property->isDerivedFrom<App::PropertyLinkBase>()) {
                copy.reset(static_cast<App::Property*>(
                    property->getTypeId().createInstance()));
                if (!copy) {
                    throw std::runtime_error(
                        "generic recompute property cannot be copied: " + name);
                }
                if (requiresDetachedValueSnapshot(*property)) {
                    copy = detachedValueSnapshot(*property);
                }
                else {
                    // Some legacy Copy() implementations return a base property
                    // (for example PropertyAngle inherits PropertyFloat::Copy),
                    // so paste into the registered concrete type. Link properties
                    // require an attached owning container and are compared from
                    // their same-document persistence representation instead.
                    copy->Paste(*property);
                }
            }
            properties.emplace(
                name,
                PropertySnapshot {std::move(copy),
                                  writer.getString(),
                                  payloadInDocFile ? valueArchive(*property) : std::string {},
                                  payloadInDocFile});
        }
    }
    return result;
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

    // A doc-file-backed payload is invisible to the XML, so compare the
    // archives that carry it rather than two identical file references.
    if (serializesPayloadToDocFile(left) || serializesPayloadToDocFile(right)) {
        return valueArchive(left) == valueArchive(right);
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

bool sameCapturedProperty(const App::Property& current,
                          const PropertySnapshot& baseline)
{
    if (!baseline.value) {
        Base::StringWriter currentWriter;
        current.Save(currentWriter);
        return currentWriter.getString() == baseline.serialized;
    }
    if (current.getTypeId() != baseline.value->getTypeId()) {
        return false;
    }
    if (requiresDetachedValueSnapshot(current)) {
        // Part shapes are doc-file backed as well, but they keep their
        // established normalized comparison: raw archive bytes would report
        // legitimate serialization normalization as a value change.
        auto normalized = detachedValueSnapshot(const_cast<App::Property&>(current));
        return normalized->isSame(*baseline.value);
    }
    if (baseline.payloadInDocFile) {
        // The XML carries only a file reference, so neither the serialized
        // fallback below nor isSame()'s memory-size heuristic can tell an empty
        // payload from a computed one. Compare the archives that hold it.
        return valueArchive(current) == baseline.archived;
    }
    if (current.isSame(*baseline.value)) {
        return true;
    }

    // Paste() does not necessarily preserve property configuration flags.
    // Compare the fallback serialization with the exact pre-execute property,
    // not with its value-only snapshot, so configured link properties retain
    // their original flags for fail-closed side-effect detection.
    Base::StringWriter currentWriter;
    current.Save(currentWriter);
    return currentWriter.getString() == baseline.serialized;
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
        if (!sameCapturedProperty(
                *property, targetBaseline.at(propertyManifest.name))) {
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
        // These values were produced by DocumentObject::execute() in the
        // detached document. Apply them under the same document and object
        // recomputing statuses as an in-process recompute. In particular,
        // Part::Feature::onChanged() otherwise treats a pasted Shape as a user
        // edit and derives Placement from the shape's top-level transform,
        // which can silently reset a deactivated sketch's independently
        // assigned Placement.
        Base::ObjectStatusLocker<App::Document::Status, App::Document> recomputing(
            App::Document::Recomputing, &document);
        Base::ObjectStatusLocker<App::ObjectStatus, App::DocumentObject> executing(
            App::Recompute, target);
        if (_authoritativeTransientSchema) {
            if (!usesAuthoritativeTransientRecomputeSchema(*target)) {
                throw std::runtime_error(
                    "generic recompute transient-schema target contract no longer matches");
            }
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
        document.settleRecomputedFeature(*target);
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
        if (!isBookkeepingOnlyTarget(*target)) {
            throw std::runtime_error(
                "generic recompute bookkeeping target became executable before commit");
        }
        _applied = false;
        _counted = target->mustRecompute();
        document.settleRecomputedFeature(*target);
        _applied = true;
    }

    bool recomputeCountedFeature() const noexcept override
    {
        return _counted;
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
    mutable bool _counted {true};
};

class GenericRecomputeInProcessOperation final: public App::CollaborativeOperation
{
public:
    GenericRecomputeInProcessOperation(std::string target,
                                       std::string stableIdentity,
                                       const bool ownerThreadRequested)
        : _target(std::move(target))
        , _stableIdentity(std::move(stableIdentity))
        , _ownerThreadRequested(ownerThreadRequested)
    {}

    std::string_view typeId() const noexcept override
    {
        return App::GenericIsolatedRecomputeOperationType;
    }

    void apply(App::Document& document) const override
    {
        auto* target = requireTarget(document);
        // A target that never opts into worker execution must still be here at
        // commit time -- that is the contract this operation was prepared
        // under. When the caller asked for the owner thread, or an un-opted
        // member of the target's dependency closure forced the fallback, the
        // venue was not the target's own choice, so opting in is expected and
        // the check does not apply.
        if (!_ownerThreadRequested && target->canRecomputeOnWorker()) {
            throw std::runtime_error(
                "generic recompute in-process target opted into isolated execution before commit");
        }
        _applied = false;
        // Same statuses the isolated result application uses, so onChanged()
        // observers cannot mistake an execute-owned write for a user edit.
        Base::ObjectStatusLocker<App::Document::Status, App::Document> recomputing(
            App::Document::Recomputing, &document);
        Base::ObjectStatusLocker<App::ObjectStatus, App::DocumentObject> executing(
            App::Recompute, target);
        const int result =
            App::Internal::GenericIsolatedRecomputeAccess::executeAuthoritative(
                document, *target);
        _failed = result != 0;
        if (_failed) {
            // A scripted execute() that raises is an ordinary modelling error,
            // not a broken commit: record it the way the isolated path records
            // a worker failure and leave the rest of the plan intact.
            const char* diagnostic = document.getErrorDescription(target);
            _failureDiagnostic = diagnostic && *diagnostic
                ? std::string(diagnostic)
                : std::string("in-process feature recompute failed");
            App::Internal::GenericIsolatedRecomputeAccess::applyFailure(
                document,
                *target,
                *_failureDiagnostic);
        }
        else {
            // _recomputeFeature() only clears the error; settling the object is
            // the caller's job here exactly as it is for an applied isolated
            // result. Without this the feature stays touched forever, so it
            // re-executes on every later recompute and the document never
            // reports itself settled.
            target->purgeError();
            document.settleRecomputedFeature(*target);
        }
        _applied = true;
    }

    App::CollaborativePostconditionResult checkPostcondition(
        const App::Document& document) const override
    {
        try {
            const auto* target = requireTarget(document);
            if (!_applied) {
                return {false, "in-process feature recompute did not run"};
            }
            if (_failed) {
                // The failure path deliberately leaves the feature dirty so the
                // next recompute retries it, matching the isolated path.
                return {target->isTouched(),
                        target->isTouched()
                            ? std::string {}
                            : "in-process recompute failure state was not applied"};
            }
            return {!target->isTouched(),
                    target->isTouched()
                        ? "in-process feature recompute did not clear the touch state"
                        : std::string {}};
        }
        catch (const std::exception& error) {
            return {false, error.what()};
        }
    }

    // The coordinator classifies a node by asking the operation how the
    // recompute itself went -- a commit that applied a *failure* cleanly is
    // still a failed recompute. The isolated result operation reports this;
    // without the same answer here the owner-thread venue would report every
    // raising execute() as a success.
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
            throw std::runtime_error("generic recompute in-process target identity is stale");
        }
        return target;
    }

    std::string _target;
    std::string _stableIdentity;
    bool _ownerThreadRequested {false};
    mutable bool _applied {false};
    mutable bool _failed {false};
    mutable std::optional<std::string> _failureDiagnostic;
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
        const auto* registeredObject = registeredType.get();
        if (!registeredObject || typeid(*registeredObject) != typeid(*object)) {
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

// Walks the same out-list graph collectClosure() walks, asking only "can every
// object in it run in the worker?". A closure member that has not opted in
// cannot be reproduced in the detached process, so the whole job must take the
// owner-thread venue -- the same rationale as the target-level opt-out below,
// widened from the target alone to its dependency closure. Every other
// closure rule (cross-document links, unserializable runtime types) stays
// fail-closed inside collectClosure() itself: those are refusals, not venue
// choices, so this probe does not attempt to evaluate them and simply skips
// what it cannot judge, leaving collectClosure() to reject it later if the
// worker venue is still chosen.
bool closureOptsIntoWorkerExecution(const App::Document& document, App::DocumentObject& target)
{
    std::vector<App::DocumentObject*> pending {&target};
    std::unordered_set<App::DocumentObject*> seen;
    while (!pending.empty()) {
        auto* object = pending.back();
        pending.pop_back();
        if (!object || !seen.insert(object).second) {
            continue;
        }
        if (object->getDocument() != &document || !object->isAttachedToDocument()) {
            // Not this probe's call: collectClosure() rejects a cross-document
            // or detached member fail-closed, so leave that refusal to it.
            continue;
        }
        if (!object->canRecomputeOnWorker()) {
            return false;
        }
        for (auto* dependency : object->getOutList()) {
            pending.push_back(dependency);
        }
    }
    return true;
}

App::CollaborativeOperationPreparation prepareGenericRecompute(
    const App::Document& document,
    const App::CollaborativeOperationIntent& intent)
{
    const auto legacyMode = intent.arguments.find("legacy_revision_semantics");
    const auto forceMode = intent.arguments.find("force_execution");
    const auto ownerThreadMode = intent.arguments.find("owner_thread_execution");
    if (intent.arguments.empty() || intent.arguments.size() > 4
        || !intent.arguments.contains("feature")
        || std::ranges::any_of(intent.arguments, [](const auto& argument) {
               return argument.first != "feature"
                   && argument.first != "legacy_revision_semantics"
                   && argument.first != "force_execution"
                   && argument.first != "owner_thread_execution";
           })) {
        throw std::invalid_argument(
            "generic recompute requires a feature and optional revision/force/venue modes");
    }
    const bool preserveLegacyRevisionSemantics = legacyMode != intent.arguments.end();
    if (preserveLegacyRevisionSemantics && legacyMode->second != "1") {
        throw std::invalid_argument("generic recompute revision mode is invalid");
    }
    const bool forceExecution = forceMode != intent.arguments.end();
    if (forceExecution && forceMode->second != "1") {
        throw std::invalid_argument("generic recompute force mode is invalid");
    }
    // Spawning a worker process costs ~2s of interpreter and library startup,
    // which is three orders of magnitude more than executing an ordinary
    // feature. The compatibility facades behind Document::recompute() and
    // Document::recomputeFeature() are every recompute FreeCAD has ever done,
    // so they ask for the owner thread and pay that cost only where a caller
    // genuinely wants the detached process.
    const bool ownerThreadExecution = ownerThreadMode != intent.arguments.end();
    if (ownerThreadExecution && ownerThreadMode->second != "1") {
        throw std::invalid_argument("generic recompute execution venue is invalid");
    }
    const std::string targetName = intent.arguments.at("feature");
    auto* target = document.getObject(targetName.c_str());
    if (!target || !target->isAttachedToDocument() || target->getDocument() != &document) {
        throw std::invalid_argument("generic recompute target does not exist");
    }

    // The two exact-type predicates below are proven-inert contracts -- an
    // App::FeaturePython with a null Proxy literally cannot run Python execute
    // code -- so they keep their cheap bookkeeping path even though the type
    // never opts into worker execution. Only the general mustRecompute()
    // shortcut, which proves nothing about execute(), is deferred until after
    // the opt-out branch.
    const bool provenInertBookkeepingContract =
        isSafePlainAppLinkBookkeepingTarget(*target)
        || isNullProxyExternalLinkHolderBookkeepingTarget(*target);

    // Preparing the target to execute on the owner thread inside the
    // coordinator's commit boundary. Three callers want it: an object that
    // never opts into worker execution (below), a caller that asked for the
    // owner thread because a detached process is not worth ~2s of startup,
    // and -- venueForcedByClosure -- a target that did opt in but whose
    // dependency closure contains something that did not, so the worker
    // cannot reproduce the job at all.
    const auto prepareOwnerThreadExecution = [&document, &targetName, target,
                                              preserveLegacyRevisionSemantics,
                                              ownerThreadExecution](
                                                 bool venueForcedByClosure = false) {
        // The object has not opted into worker execution, so its execute()
        // cannot be reproduced in the detached process at all: for every
        // ordinary Python scripted feature the behaviour lives in a proxy the
        // archive cannot carry, and FeaturePython only opts in when that proxy
        // implements supportsAsyncRecompute(). Routing it to the worker makes
        // collectClosure() refuse the job and the coordinator record a failed
        // node, which leaves the feature permanently touched and invalid --
        // Draft, Arch, FEM, Assembly's joints and every user macro included.
        // Run execute() on the owner thread inside the coordinator's commit
        // boundary instead, which is what the in-process recompute did.
        //
        // This is checked before the bookkeeping short-circuit: a scripted
        // feature's mustExecute() reports nothing about the work its proxy
        // owes, so purging the touch instead of executing would drop it.
        const std::string stableIdentity = document.collaborationObjectIdentity(*target);
        std::vector<App::DocumentRevisionKey> reads {
            App::DocumentRevisionKey::documentStructure(),
            App::DocumentRevisionKey::objectExistence(targetName),
            App::DocumentRevisionKey::objectStructure(targetName),
            App::DocumentRevisionKey::objectModel(targetName),
            App::DocumentRevisionKey::unknownModelMutation()};
        std::vector<App::DocumentRevisionKey> writes {
            App::DocumentRevisionKey::objectModel(targetName)};
        std::vector<App::DocumentRevisionPublicationRequest> effects {
            {App::DocumentRevisionKey::objectModel(targetName), stableIdentity}};
        // A target that opts into worker execution has a declarable output
        // set -- the same predicate the worker manifest uses -- so declare
        // exactly that. Publishing every property instead would announce
        // dozens of revisions the recompute never wrote, and the venue a
        // caller picked must not change what a recompute is seen to touch.
        //
        // A target that opts out is an ordinary scripted feature whose
        // execute() lives in a proxy: nothing here can tell what it writes, so
        // every property stays declared.
        const bool outputsAreDeclarable = target->canRecomputeOnWorker();
        for (const auto& [propertyName, property] : namedProperties(*target)) {
            auto key = App::DocumentRevisionKey::objectProperty(targetName, propertyName);
            reads.push_back(key);
            if (outputsAreDeclarable) {
                // A declared effect publishes whether or not the recompute
                // wrote anything, because the effect set is frozen at
                // preparation. The isolated venue can afford per-property
                // effects only because it decodes the worker's result first
                // and refines them down before committing; a failed detached
                // recompute therefore publishes just the object's model key.
                // Nothing here knows the outcome yet, so announcing a property
                // would publish a revision for a value a raising execute()
                // never produced. Take the object's model key as the whole
                // declaration and let it stand for whatever execute() wrote.
                continue;
            }
            writes.push_back(key);
            effects.push_back({std::move(key), stableIdentity});
        }
        static_cast<void>(outputsAreDeclarable);
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        std::sort(writes.begin(), writes.end());
        writes.erase(std::unique(writes.begin(), writes.end()), writes.end());
        if (preserveLegacyRevisionSemantics) {
            writes.push_back(App::DocumentRevisionKey::unknownModelMutation());
            effects.push_back(
                {App::DocumentRevisionKey::unknownModelMutation(), std::nullopt});
        }
        // The operation's commit-time guard only tolerates an opted-in target
        // when the venue was not the target's own choice (see apply() above).
        // That is true both when the caller asked for the owner thread and
        // when the closure forced it, so either one satisfies the guard.
        const bool targetOptOutNotRequired = ownerThreadExecution || venueForcedByClosure;
        App::CollaborativeOperationPreparation::DetachedTask task =
            [targetName, stableIdentity, targetOptOutNotRequired](
                const std::stop_token stopToken) {
                if (stopToken.stop_requested()) {
                    throw std::runtime_error(
                        "generic recompute in-process preparation was cancelled");
                }
                return std::make_unique<const GenericRecomputeInProcessOperation>(
                    targetName, stableIdentity, targetOptOutNotRequired);
            };
        return App::CollaborativeOperationPreparation {std::move(reads),
                                                       std::move(writes),
                                                       std::move(effects),
                                                       std::move(task),
                                                       App::PreparationPolicy::DetachedInProcess};
    };

    if (!target->canRecomputeOnWorker() && !provenInertBookkeepingContract) {
        return prepareOwnerThreadExecution();
    }

    if (!forceExecution && target->isTouched()
        && isBookkeepingOnlyTarget(*target)) {
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

    if (ownerThreadExecution) {
        // Same decision tree as the detached venue -- the opt-out and
        // bookkeeping branches above already ran -- only the venue differs.
        return prepareOwnerThreadExecution();
    }

    if (!closureOptsIntoWorkerExecution(document, *target)) {
        // The target itself opted in -- the branch at the top of this
        // function already ruled out the opposite -- but something in its
        // dependency closure did not, and collectClosure() below would
        // refuse the whole job for that reason alone. Refusing here would
        // turn an opted-in group holding one un-opted scripted feature (a
        // JointGroup holding a Python joint, an Origin holding nothing but
        // still walked, ...) into a permanently failed node instead of the
        // owner-thread fallback that exists for exactly this shape. Cross-
        // document links and unserializable runtime types are not handled
        // here and still fail closed inside collectClosure() when the
        // worker venue is chosen.
        return prepareOwnerThreadExecution(/*venueForcedByClosure=*/true);
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
    {
        // The archive was written by this exact program version out of an
        // already-restored document, so every deprecated-property migration
        // has already been applied to it.  Letting onDocumentRestored() run
        // them again would rewrite live values (for example FeatureExtrude
        // would re-derive SideType from the residual Midplane flag) and the
        // worker would then execute different semantics than the caller.
        Base::ObjectStatusLocker<App::Document::Status, App::Document> schemaTransfer(
            App::Document::CurrentSchemaTransfer, detached);
        static_cast<void>(importer.importObjects(archiveStream));
    }
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
            // Target execution may refresh an execute-owned cache on a closure
            // dependency (for example Pad reads a Sketch and rebuilds its
            // InternalShape). These exact declared outputs are harmless in the
            // disposable worker and are not published unless they belong to
            // the target. All non-output dependency state remains immutable.
            if (!propertyManifest.output
                && !sameCapturedProperty(
                    *property,
                    baseline.at(objectManifest.name).at(propertyManifest.name))) {
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

namespace
{

/** The edges the recompute plan must order its nodes by.
 *
 * Mirrors the getOutListFineGrained() lambda in Document::buildDependencyList().
 * The coarse out list reports a dependency on the whole object, which in
 * fine-grained mode reports cycles that do not exist: an input property is
 * written by its dependents, so depending on the object as a whole turns a
 * legal property-level edge into a cycle and the plan is rejected outright.
 */
std::vector<DocumentObject*> planDependencies(DocumentObject& object)
{
    if (!GetApplication().isFineGrainedRecomputeEnabled()) {
        return object.getOutList();
    }
    std::vector<DocumentObject*> outList;
    std::unordered_set<DocumentObject*> seen;
    for (const auto& [fromObj, fromProp, toObj, toProp] : object.getOutListProp(0)) {
        if (!toObj) {
            continue;
        }
        if (toProp.empty()) {
            if (seen.insert(toObj).second) {
                outList.push_back(toObj);
            }
            continue;
        }
        if (!seen.contains(toObj) && !toObj->isInputProperty(toProp)) {
            seen.insert(toObj);
            outList.push_back(toObj);
        }
    }
    return outList;
}

// Kahn's algorithm cannot tell a cycle member from an acyclic consumer sitting
// downstream of one: both are simply missing from its ordered set. Relaxing
// every edge of every missing node therefore deletes valid dependencies -- with
// Z_Group <-> Z_Source -> C_First -> B_Middle -> A_Result nothing is ordered at
// all, every edge goes, and the coordinator's name-ordered ready scan
// (DocumentRecomputeCoordinator.cpp:389, over a std::map) then runs A_Result
// before B_Middle while an upstream failure no longer blocks its dependents.
//
// Strongly connected components separate the two cases exactly. Kosaraju is
// used rather than Tarjan because both adjacency directions already exist at
// the call site -- dependencies forward, dependents reversed -- and both of its
// passes are plain iterative sweeps, so a deep dependency chain cannot overflow
// the stack the way a recursive Tarjan would.
std::map<std::string, std::size_t> stronglyConnectedComponents(
    const std::vector<DocumentRecomputeFeatureRequest>& features,
    const std::map<std::string, std::vector<std::string>>& dependents)
{
    std::map<std::string, const std::vector<std::string>*> forward;
    for (const auto& feature : features) {
        forward.emplace(feature.featureId, &feature.dependencies);
    }

    // Pass one: record each node after all of its forward successors are done.
    // Each entry is stacked twice, once to expand and once to record.
    std::set<std::string> seen;
    std::vector<std::string> finished;
    finished.reserve(features.size());
    for (const auto& feature : features) {
        if (seen.contains(feature.featureId)) {
            continue;
        }
        std::vector<std::pair<std::string, bool>> stack {{feature.featureId, false}};
        while (!stack.empty()) {
            auto [name, expanded] = std::move(stack.back());
            stack.pop_back();
            if (expanded) {
                finished.push_back(std::move(name));
                continue;
            }
            if (!seen.insert(name).second) {
                continue;
            }
            stack.emplace_back(name, true);
            for (const auto& next : *forward.at(name)) {
                if (!seen.contains(next)) {
                    stack.emplace_back(next, false);
                }
            }
        }
    }

    // Pass two: sweep the reversed graph in reverse finishing order. Every node
    // reached in one sweep forms one strongly connected component.
    std::map<std::string, std::size_t> component;
    std::size_t nextComponent = 0;
    for (auto entry = finished.rbegin(); entry != finished.rend(); ++entry) {
        if (component.contains(*entry)) {
            continue;
        }
        std::vector<std::string> stack {*entry};
        while (!stack.empty()) {
            const auto name = std::move(stack.back());
            stack.pop_back();
            if (!component.emplace(name, nextComponent).second) {
                continue;
            }
            const auto reverse = dependents.find(name);
            if (reverse == dependents.end()) {
                continue;
            }
            for (const auto& previous : reverse->second) {
                if (!component.contains(previous)) {
                    stack.push_back(previous);
                }
            }
        }
        ++nextComponent;
    }
    return component;
}

}  // namespace

DocumentRecomputeRequest makeGenericIsolatedRecomputeRequest(
    Document& document,
    const std::vector<DocumentObject*>& features,
    const std::string_view provenance,
    const std::string_view coalescingPrefix,
    const bool preserveLegacyRevisionSemantics,
    const bool forceExecution,
    const bool ownerThreadExecution)
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
        if (ownerThreadExecution) {
            node.intent.arguments.emplace("owner_thread_execution", "1");
        }
        // Mirror the venue prepareGenericRecompute() will actually pick: the
        // caller's flag, or the target's own opt-out, whichever forces the
        // owner thread. A caller inspecting the plan before it runs needs to
        // know this up front, not only after execute() has already run.
        node.ownerThreadExecution = ownerThreadExecution || !object->canRecomputeOnWorker();
        node.provenance = std::string(provenance);
        for (auto* dependency : planDependencies(*object)) {
            if (dependency && dependency != object && dependency->getDocument() == &document
                && dependency->getNameInDocument()
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

    // The legacy loop tolerated a cyclic graph: topologicalSort() gave up and
    // partialTopologicalSort() ordered what it could, so a group that owns an
    // object which links back at it still recomputed. The coordinator instead
    // rejects a cyclic plan outright, which would turn that document into a
    // hard error. Order what can be ordered and drop only the edges that close
    // a cycle, so the cyclic nodes still run -- in an arbitrary order among
    // themselves, exactly as the partial sort left them.
    {
        std::map<std::string, std::size_t> indegree;
        std::map<std::string, std::vector<std::string>> dependents;
        for (const auto& feature : request.features) {
            indegree.emplace(feature.featureId, feature.dependencies.size());
        }
        for (const auto& feature : request.features) {
            for (const auto& dependency : feature.dependencies) {
                dependents[dependency].push_back(feature.featureId);
            }
        }
        std::set<std::string> ready;
        for (const auto& [featureId, degree] : indegree) {
            if (degree == 0) {
                ready.insert(featureId);
            }
        }
        std::set<std::string> ordered;
        while (!ready.empty()) {
            const auto current = ready.extract(ready.begin()).value();
            ordered.insert(current);
            for (const auto& dependent : dependents[current]) {
                if (--indegree.at(dependent) == 0) {
                    ready.insert(dependent);
                }
            }
        }
        if (ordered.size() != request.features.size()) {
            const auto component =
                stronglyConnectedComponents(request.features, dependents);
            std::map<std::size_t, std::size_t> componentSize;
            for (const auto& [featureId, id] : component) {
                static_cast<void>(featureId);
                ++componentSize[id];
            }
            for (auto& feature : request.features) {
                const auto own = component.at(feature.featureId);
                const bool selfEdge =
                    std::ranges::find(feature.dependencies, feature.featureId)
                    != feature.dependencies.end();
                if (componentSize.at(own) < 2 && !selfEdge) {
                    // Acyclic. Ordered late only because a cycle sits upstream;
                    // every one of its edges is valid and the coordinator needs
                    // all of them to keep it behind its inputs and to block it
                    // when one of them fails.
                    continue;
                }
                // Drop only the edges that stay inside this cycle. Edges leaving
                // it, and every edge into it from downstream, are preserved:
                // the condensation of a directed graph is a DAG, so what is left
                // is acyclic and the coordinator will accept it.
                std::erase_if(feature.dependencies,
                              [&component, own](const std::string& dependency) {
                                  return component.at(dependency) == own;
                              });
            }
        }
    }
    return request;
}

DocumentRecomputeRequest makeGenericIsolatedRecomputeRequest(
    Document& document,
    DocumentObject& feature,
    const bool recursive,
    const bool preserveLegacyRevisionSemantics,
    const bool ownerThreadExecution)
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
        true,
        ownerThreadExecution);
}

std::string canonicalRecomputeArchiveContents(const std::string& archiveBytes)
{
    return canonicalArchiveContents(archiveBytes);
}

}  // namespace App::Internal
