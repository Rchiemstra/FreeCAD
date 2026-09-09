// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GenericIsolatedRecompute.h"

#include "Application.h"
#include "CollaborativeOperation.h"
#include "Document.h"
#include "DocumentObject.h"
#include "Extension.h"
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
#include <array>
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
    // ExtensionContainer does not override getPropertyNamedList(). Build the
    // named view from its virtual full property list so built-in LinkExtension
    // state is included in manifests, fences, and side-effect validation.
    std::vector<App::Property*> raw;
    object.getPropertyList(raw);
    std::vector<std::pair<std::string, App::Property*>> result;
    result.reserve(raw.size());
    for (auto* property : raw) {
        const char* name = property ? object.getPropertyName(property) : nullptr;
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

bool hasCanonicalExtensionSet(const App::DocumentObject& object,
                              const App::DocumentObject& canonical);

bool isSafePlainAppLinkArchiveInput(const App::DocumentObject& object)
{
    // A plain native App::Link may be captured as a target's structural input,
    // but never used as a bookkeeping-only recompute target: its extension
    // execute path touches _LinkTouched to refresh the GUI. Derived links,
    // arrays, copy-on-change links, unresolved links, noncanonical extensions,
    // and links that can invoke a Python proxy remain fail-closed.
    const Base::Type linkType = Base::Type::fromName("App::Link");
    if (linkType.isBad() || object.getTypeId() != linkType
        || typeid(object) != typeid(App::Link)) {
        return false;
    }
    std::unique_ptr<App::DocumentObject> canonical(
        static_cast<App::DocumentObject*>(linkType.createInstance()));
    if (!canonical || typeid(*canonical) != typeid(App::Link)
        || !hasCanonicalExtensionSet(object, *canonical)
        || object.mustExecute() != 0) {
        return false;
    }
    const auto* link = dynamic_cast<const App::Link*>(&object);
    if (!link || link->ElementCount.getValue() != 0
        || link->getLinkCopyOnChangeValue()
            != App::LinkBaseExtension::CopyOnChangeDisabled
        || link->getLinkCopyOnChangeSourceValue()
        || link->getLinkCopyOnChangeGroupValue()
        || !object.ExpressionEngine.getExpressions().empty()
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

bool isSafePlainDocumentObjectBookkeepingTarget(
    const App::DocumentObject& object)
{
    // The exact base DocumentObject has the base no-op execute() and owns no
    // extension behavior. Its dynamic values are not serialized or copied by
    // the bookkeeping operation; only its recompute status is settled.
    const Base::Type objectType = Base::Type::fromName("App::DocumentObject");
    return !objectType.isBad() && object.getTypeId() == objectType
        && typeid(object) == typeid(App::DocumentObject)
        && !object.hasExtensions()
        && object.ExpressionEngine.getExpressions().empty()
        && object.mustExecute() == 0;
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
                   && isSafePlainAppLinkArchiveInput(*dependency);
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
    // AssemblyObject::execute() runs the joint solver whenever the
    // SolveOnRecompute preference is set, which is the default. It declares no
    // mustExecute() of its own, so a touched assembly reports 0 and would
    // never solve. It also opts out of worker execution, so the isolation
    // check rejects it before this shortcut; keep the entry so the shortcut
    // cannot silently reclaim it if that opt-out is ever lifted.
    const Base::Type assemblyType = Base::Type::fromName("Assembly::AssemblyObject");
    return !assemblyType.isBad() && object.getTypeId().isDerivedFrom(assemblyType);
}

bool isBookkeepingOnlyTarget(const App::DocumentObject& object)
{
    if (owesUnconditionalExecuteWork(object)) {
        return false;
    }
    return object.mustRecompute() == 0
        || isSafePlainDocumentObjectBookkeepingTarget(object)
        || isNullProxyExternalLinkHolderBookkeepingTarget(object);
}

bool hasAuditedWorkerResultTypeId(const App::DocumentObject& object)
{
    static constexpr std::array auditedTypes {
        "App::FeatureTest",
        "App::FeatureTestException",
        "App::FeatureTestColumn",
        "App::FeatureTestRow",
        "App::FeatureTestAbsAddress",
        "App::FeatureTestPlacement",
    };
    return std::ranges::any_of(auditedTypes, [&object](const char* name) {
        const Base::Type type = Base::Type::fromName(name);
        return !type.isBad() && object.getTypeId() == type;
    });
}

bool hasKnownInertBookkeepingTypeId(const App::DocumentObject& object)
{
    static constexpr std::array inertTypes {
        "App::DocumentObject",
        "App::FeaturePython",
    };
    return std::ranges::any_of(inertTypes, [&object](const char* name) {
        const Base::Type type = Base::Type::fromName(name);
        return !type.isBad() && object.getTypeId() == type;
    });
}

bool hasExactRegisteredRuntimeType(const App::DocumentObject& object)
{
    std::unique_ptr<App::DocumentObject> registeredType(
        static_cast<App::DocumentObject*>(object.getTypeId().createInstance()));
    return registeredType && typeid(*registeredType) == typeid(object);
}

bool hasPythonObjectProperty(const App::DocumentObject& object)
{
    return std::ranges::any_of(namedProperties(object), [](const auto& entry) {
        return entry.second->isDerivedFrom<App::PropertyPythonObject>();
    });
}

bool hasAuditedCompleteWorkerResultContract(const App::DocumentObject& object)
{
    // canRecomputeOnWorker() predates the isolated archive protocol and only
    // promised thread affinity. An existing binary or addon override therefore
    // cannot, by itself, prove that every execute-owned result is represented
    // by transferable declared properties. Admit only exact types audited for
    // the stronger protocol; new production types need a typed adapter or an
    // explicit entry here together with native round-trip coverage.
    return hasAuditedWorkerResultTypeId(object)
        && hasExactRegisteredRuntimeType(object)
        && object.canRecomputeOnWorker();
}

bool isDeclaredRecomputeOutput(const App::DocumentObject& object,
                               const App::Property& property)
{
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
    return compatibleOutput;
}

ObjectManifest manifestFor(const App::DocumentObject& object)
{
    if (!object.getNameInDocument()) {
        throw std::runtime_error("generic recompute object is not attached");
    }
    ObjectManifest result;
    result.name = object.getNameInDocument();
    result.type = object.getTypeId().getName();
    for (const auto& [name, property] : namedProperties(object)) {
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
        const auto actualProperties = namedProperties(*found->second);
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

// The full persistence archive round-trips through ZipWriter and therefore does
// carry SaveDocFile payloads. It is deterministic for equal content, so it is a
// sound equality basis where the plain XML is blind.
std::string valueArchive(const App::Property& property)
{
    return dumpProperty(const_cast<App::Property&>(property));
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

bool hasCanonicalExtensionSet(const App::DocumentObject& object,
                              const App::DocumentObject& canonical)
{
    const Base::Type extensionType = App::Extension::getExtensionClassTypeId();
    const auto actualExtensions = object.getExtensionsDerivedFrom(extensionType);
    const auto canonicalExtensions = canonical.getExtensionsDerivedFrom(extensionType);
    if (actualExtensions.size() != canonicalExtensions.size()) {
        return false;
    }
    for (const auto* canonicalExtension : canonicalExtensions) {
        // Only dispatch the virtual type query on the freshly constructed
        // built-in peer. The live object may carry an addon extension whose
        // virtual methods must not run merely to reject the archive request.
        const Base::Type type = canonicalExtension->getExtensionTypeId();
        const auto* actualExtension = object.getExtension(type, false, true);
        if (!actualExtension
            || typeid(*actualExtension) != typeid(*canonicalExtension)) {
            return false;
        }
    }
    return true;
}

bool hasAuditedArchivePropertyTypeId(const App::DocumentObject& object,
                                     const App::Property& property)
{
    // Exact registered value types only. In particular, do not let a future
    // file-backed, Python-backed, or addon Property become serializable merely
    // because a trusted object class acquired a new static member.
    static constexpr std::array auditedTypes {
        "App::PropertyString",
        "App::PropertyExpressionEngine",
        "App::PropertyBool",
        "App::PropertyInteger",
        "App::PropertyFloat",
        "App::PropertyBoolList",
        "App::PropertyPath",
        "App::PropertyStringList",
        "App::PropertyEnumeration",
        "App::PropertyIntegerConstraint",
        "App::PropertyFloatConstraint",
        "App::PropertyColor",
        "App::PropertyColorList",
        "App::PropertyMaterial",
        "App::PropertyMaterialList",
        "App::PropertyDistance",
        "App::PropertyAngle",
        "App::PropertyIntegerList",
        "App::PropertyFloatList",
        "App::PropertyLink",
        "App::PropertyLinkSub",
        "App::PropertyLinkList",
        "App::PropertyLinkSubList",
        "App::PropertyVector",
        "App::PropertyVectorList",
        "App::PropertyMatrix",
        "App::PropertyPlacement",
        "App::PropertyQuantity",
    };
    if (std::ranges::any_of(auditedTypes, [&property](const char* name) {
        const Base::Type type = Base::Type::fromName(name);
        return !type.isBad() && property.getTypeId() == type;
    })) {
        return true;
    }

    // These structural value types occur in the canonical exact App::Link
    // input schema. They are not generally admitted on executable targets.
    const Base::Type linkType = Base::Type::fromName("App::Link");
    if (linkType.isBad() || object.getTypeId() != linkType
        || typeid(object) != typeid(App::Link)) {
        return false;
    }
    static constexpr std::array linkOnlyTypes {
        "App::PropertyXLink",
        "App::PropertyLinkSubHidden",
        "App::PropertyPlacementList",
    };
    return std::ranges::any_of(linkOnlyTypes, [&property](const char* name) {
        const Base::Type type = Base::Type::fromName(name);
        return !type.isBad() && property.getTypeId() == type;
    });
}

bool hasCanonicalAuditedArchiveSchema(const App::DocumentObject& object)
{
    // The object type is already restricted to a built-in audited candidate
    // before this function is called, so constructing its registered peer
    // cannot dispatch an addon constructor on the owner thread.
    std::unique_ptr<App::DocumentObject> registered(
        static_cast<App::DocumentObject*>(object.getTypeId().createInstance()));
    if (!registered || typeid(*registered) != typeid(object)) {
        return false;
    }
    if (!hasCanonicalExtensionSet(object, *registered)) {
        return false;
    }

    const auto actual = namedProperties(object);
    const auto canonical = namedProperties(*registered);
    if (actual.size() != canonical.size()) {
        return false;
    }
    constexpr unsigned long volatileStatusMask =
        (1UL << App::Property::Touched) | (1UL << App::Property::Busy);
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto& [actualName, actualProperty] = actual[index];
        const auto& [canonicalName, canonicalProperty] = canonical[index];
        if (actualName != canonicalName
            || actualProperty->testStatus(App::Property::PropDynamic)
            || !hasAuditedArchivePropertyTypeId(object, *actualProperty)
            || actualProperty->getTypeId() != canonicalProperty->getTypeId()
            || actualProperty->getType() != canonicalProperty->getType()
            || (actualProperty->getStatus() & ~volatileStatusMask)
                != (canonicalProperty->getStatus() & ~volatileStatusMask)) {
            return false;
        }

        // Link scope and internal behavior flags are C++ configuration, not
        // all of which is represented by FCStd. A same-named link property
        // with different runtime configuration would otherwise execute under
        // different dependency semantics in the worker.
        const auto* actualLink =
            dynamic_cast<const App::PropertyLinkBase*>(actualProperty);
        const auto* canonicalLink =
            dynamic_cast<const App::PropertyLinkBase*>(canonicalProperty);
        if (static_cast<bool>(actualLink) != static_cast<bool>(canonicalLink)) {
            return false;
        }
        if (actualLink) {
            if (actualLink->getScope() != canonicalLink->getScope()) {
                return false;
            }
            for (int flag = App::PropertyLinkBase::LinkAllowExternal;
                 flag <= App::PropertyLinkBase::LinkSilentRestore;
                 ++flag) {
                if (actualLink->testFlag(flag) != canonicalLink->testFlag(flag)) {
                    return false;
                }
            }
        }

        // FCStd deliberately omits transient and no-persist values. Require
        // those inputs to equal the registered constructor state; otherwise
        // the worker would silently execute against a different value.
        const auto propertyType = actualProperty->getType();
        if ((propertyType & (App::Prop_Transient | App::Prop_NoPersist)) != 0
            && !sameSerializedProperty(*actualProperty, *canonicalProperty)) {
            return false;
        }
    }
    return true;
}

bool hasAuditedArchiveContract(const App::DocumentObject& object,
                               const App::DocumentObject& target)
{
    const bool auditedExecutable = hasAuditedWorkerResultTypeId(object);
    const Base::Type linkType = Base::Type::fromName("App::Link");
    const bool auditedInputLink = &object != &target && !linkType.isBad()
        && object.getTypeId() == linkType;
    if (!auditedExecutable && !auditedInputLink) {
        return false;
    }
    if (!hasExactRegisteredRuntimeType(object)) {
        return false;
    }
    if (auditedExecutable) {
        if (!object.canRecomputeOnWorker() || object.hasExtensions()
            || hasPythonObjectProperty(object)
            || !object.ExpressionEngine.getExpressions().empty()) {
            return false;
        }
    }
    else if (!isSafePlainAppLinkArchiveInput(object)) {
        return false;
    }
    return hasCanonicalAuditedArchiveSchema(object);
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
                              std::vector<DecodedOutput> outputs)
        : _target(std::move(target))
        , _stableIdentity(std::move(stableIdentity))
        , _outputs(std::move(outputs))
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

std::unique_ptr<const App::CollaborativeOperation> decodeResult(
    const App::GeometryArchive& archive,
    const std::string& target,
    const std::string& stableIdentity,
    const std::map<std::string, std::string>& expectedOutputs)
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
        std::move(outputs));
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
        if (!hasAuditedArchiveContract(*object, target)) {
            throw std::invalid_argument(
                "generic recompute object lacks an audited archive contract: "
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

std::optional<std::vector<App::DocumentRevisionObservation>>
capturePresentationRevisionFence(
    const App::Document& document,
    App::DocumentObject& target)
{
    std::vector<App::DocumentObject*> pending {&target};
    std::unordered_set<App::DocumentObject*> seen;
    std::vector<App::DocumentRevisionKey> keys {
        App::DocumentRevisionKey::documentStructure()};
    std::size_t propertyCount = 0;
    while (!pending.empty()) {
        auto* object = pending.back();
        pending.pop_back();
        if (!seen.insert(object).second) {
            continue;
        }
        if (!object || object->getDocument() != &document
            || !object->isAttachedToDocument() || !object->getNameInDocument()
            || seen.size() > MaxObjects) {
            return std::nullopt;
        }
        if (!hasAuditedArchiveContract(*object, target)) {
            return std::nullopt;
        }
        const std::string name = object->getNameInDocument();
        keys.push_back(App::DocumentRevisionKey::objectExistence(name));
        keys.push_back(App::DocumentRevisionKey::objectStructure(name));
        keys.push_back(App::DocumentRevisionKey::objectModel(name));
        const auto properties = namedProperties(*object);
        if (properties.size() > MaxProperties - propertyCount) {
            return std::nullopt;
        }
        propertyCount += properties.size();
        for (const auto& [propertyName, property] : properties) {
            static_cast<void>(property);
            keys.push_back(
                App::DocumentRevisionKey::objectProperty(name, propertyName));
        }
        for (auto* dependency : object->getOutList()) {
            if (!dependency || dependency->getDocument() != &document
                || !dependency->isAttachedToDocument()) {
                return std::nullopt;
            }
            pending.push_back(dependency);
        }
    }
    std::ranges::sort(keys);
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return document.collaborationRevisions().capture(keys);
}

App::CollaborativeOperationPreparation prepareGenericRecompute(
    const App::Document& document,
    const App::CollaborativeOperationIntent& intent)
{
    const auto legacyMode = intent.arguments.find("legacy_revision_semantics");
    const auto forceMode = intent.arguments.find("force_execution");
    const auto identityArgument = intent.arguments.find("stable_object_identity");
    if (intent.arguments.empty() || intent.arguments.size() > 4
        || !intent.arguments.contains("feature")
        || identityArgument == intent.arguments.end()
        || std::ranges::any_of(intent.arguments, [](const auto& argument) {
               return argument.first != "feature"
                   && argument.first != "legacy_revision_semantics"
                   && argument.first != "force_execution"
                   && argument.first != "stable_object_identity";
           })) {
        throw std::invalid_argument(
            "generic recompute requires a feature identity and optional revision/force modes");
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
    if (identityArgument->second.empty()
        || document.collaborationObjectIdentity(*target)
            != identityArgument->second) {
        throw std::invalid_argument(
            "generic recompute target stable identity is stale");
    }

    // Reject unknown type ids without constructing or invoking addon-owned
    // code merely to answer an explicit asynchronous request.
    const bool auditedWorkerType = hasAuditedWorkerResultTypeId(*target);
    const bool knownInertType = hasKnownInertBookkeepingTypeId(*target);
    if (!auditedWorkerType && !knownInertType) {
        throw std::invalid_argument(
            "generic recompute object has not opted into isolated execution: "
            + targetName);
    }
    if (!hasExactRegisteredRuntimeType(*target)) {
        throw std::invalid_argument(
            "generic recompute target runtime type is not serializable: "
            + targetName);
    }
    const bool provenInertBookkeepingContract = knownInertType
        && (isSafePlainDocumentObjectBookkeepingTarget(*target)
            || isNullProxyExternalLinkHolderBookkeepingTarget(*target));
    if (!auditedWorkerType && !provenInertBookkeepingContract) {
        throw std::invalid_argument(
            "generic recompute object has not opted into isolated execution: "
            + targetName);
    }
    if (auditedWorkerType
        && (!hasAuditedCompleteWorkerResultContract(*target)
            || target->hasExtensions() || hasPythonObjectProperty(*target)
            || !target->ExpressionEngine.getExpressions().empty()
            || !hasCanonicalAuditedArchiveSchema(*target))) {
        throw std::invalid_argument(
            "generic recompute target lacks an audited archive contract: "
            + targetName);
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
    App::CollaborativeOperationPreparation::IsolatedTask isolated {
        std::move(request),
        std::move(input),
        [targetName,
         stableIdentity = document.collaborationObjectIdentity(*target),
         expectedOutputs = std::move(operationExpectedOutputs)](
            const App::GeometryArchive& output) {
            return decodeResult(output,
                                targetName,
                                stableIdentity,
                                expectedOutputs);
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
    auto* target = detached->getObject(targetName.c_str());
    if (!target) {
        throw std::runtime_error("generic recompute target disappeared after import");
    }
    for (const auto& manifest : manifests) {
        auto* object = detached->getObject(manifest.name.c_str());
        if (!object || !hasAuditedArchiveContract(*object, *target)) {
            throw std::runtime_error(
                "generic recompute closure is not worker-safe after import");
        }
    }
    if (!hasAuditedCompleteWorkerResultContract(*target)) {
        throw std::runtime_error("generic recompute target is not worker-safe after import");
    }
    // No property Save/Paste snapshot work is allowed until the imported
    // closure has passed the same exact schema boundary as its live source.
    auto baseline = capturePropertySnapshots(*detached, manifests);
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

std::optional<std::vector<DocumentRevisionObservation>>
captureGenericIsolatedRecomputePresentationFence(
    const Document& document,
    DocumentObject& feature)
{
    return capturePresentationRevisionFence(document, feature);
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

}  // namespace

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
        node.stableObjectIdentity = document.collaborationObjectIdentity(*object);
        node.presentationObjectModelRevision =
            document.collaborationRevisions().current(
                DocumentRevisionKey::objectModel(name));
        if (auto presentationFence =
                capturePresentationRevisionFence(document, *object)) {
            node.presentationRevisionFence = std::move(*presentationFence);
            node.presentationRevisionFenceComplete = true;
        }
        node.operationId = "generic-recompute:" + name;
        node.intent.operationType = std::string(GenericIsolatedRecomputeOperationType);
        node.intent.arguments.emplace("feature", name);
        node.intent.arguments.emplace(
            "stable_object_identity", node.stableObjectIdentity);
        if (preserveLegacyRevisionSemantics) {
            node.intent.arguments.emplace("legacy_revision_semantics", "1");
        }
        if (forceExecution) {
            node.intent.arguments.emplace("force_execution", "1");
        }
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
            for (auto& feature : request.features) {
                if (ordered.contains(feature.featureId)) {
                    continue;
                }
                std::erase_if(feature.dependencies,
                              [&ordered](const std::string& dependency) {
                                  return !ordered.contains(dependency);
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

DocumentRecomputeRequest makeGenericIsolatedRecomputeRequest(
    Document& document,
    DocumentObject& feature,
    const bool recursive,
    const bool preserveLegacyRevisionSemantics,
    const bool ownerThreadExecution)
{
    if (ownerThreadExecution) {
        throw std::invalid_argument(
            "generic recompute no longer accepts a caller-selected live execution venue");
    }
    return makeGenericIsolatedRecomputeRequest(
        document, feature, recursive, preserveLegacyRevisionSemantics);
}

DocumentRecomputeRequest makeGenericIsolatedRecomputeRequest(
    Document& document,
    const std::vector<DocumentObject*>& features,
    const std::string_view provenance,
    const std::string_view coalescingPrefix,
    const bool preserveLegacyRevisionSemantics,
    const bool forceExecution,
    const bool ownerThreadExecution)
{
    if (ownerThreadExecution) {
        throw std::invalid_argument(
            "generic recompute no longer accepts a caller-selected live execution venue");
    }
    return makeGenericIsolatedRecomputeRequest(
        document,
        features,
        provenance,
        coalescingPrefix,
        preserveLegacyRevisionSemantics,
        forceExecution);
}

}  // namespace App::Internal
