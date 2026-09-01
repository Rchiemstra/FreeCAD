// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborativeSweepFilletOperations.h"

#include "CollaborativeBooleanOperation.h"
#include "PartFeature.h"
#include "TopoShape.h"
#include "TopoShapeOpCode.h"

#include <App/CollaborativeOperation.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/GeometryWorkerOperationRegistry.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>

#include <BRep_Builder.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepTools.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <QCryptographicHash>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <locale>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

struct ObjectReference
{
    std::string name;
    std::string identity;
};

struct FilletEdge
{
    std::uint32_t index {0};
    double radius1 {0.0};
    double radius2 {0.0};
};

bool validShape(const TopoDS_Shape& shape)
{
    if (shape.IsNull()) {
        return false;
    }
    try {
        return BRepCheck_Analyzer(shape).IsValid();
    }
    catch (const Standard_Failure&) {
        return false;
    }
}

std::vector<std::uint8_t> encodeShape(const TopoDS_Shape& shape)
{
    if (!validShape(shape)) {
        throw std::invalid_argument("isolated Part input shape is null or invalid");
    }
    std::ostringstream stream(std::ios::out | std::ios::binary);
    stream.imbue(std::locale::classic());
    BRepTools::Write(shape, stream);
    if (!stream) {
        throw std::runtime_error("isolated Part BREP encoding failed");
    }
    const auto encoded = stream.str();
    return {encoded.begin(), encoded.end()};
}

TopoDS_Shape decodeShape(const App::GeometryArchiveSection& section)
{
    try {
        if (section.bytes.empty()) {
            throw std::invalid_argument("isolated Part BREP payload is empty");
        }
        const std::string encoded(section.bytes.begin(), section.bytes.end());
        std::istringstream stream(encoded, std::ios::in | std::ios::binary);
        stream.imbue(std::locale::classic());
        BRep_Builder builder;
        TopoDS_Shape shape;
        BRepTools::Read(shape, stream, builder);
        if (!stream || !validShape(shape)) {
            throw std::invalid_argument("isolated Part BREP payload is invalid");
        }
        stream >> std::ws;
        if (!stream.eof()) {
            throw std::invalid_argument("isolated Part BREP payload has trailing data");
        }
        return shape;
    }
    catch (const Standard_Failure& failure) {
        const char* detail = failure.GetMessageString();
        throw std::invalid_argument(
            detail && *detail
                ? std::string("isolated Part BREP payload failed: ") + detail
                : "isolated Part BREP payload failed");
    }
}

const App::GeometryArchiveSection& requireSection(
    const App::GeometryArchive& archive,
    const std::string& name)
{
    const auto found = std::ranges::find_if(archive.sections, [&](const auto& section) {
        return section.name == name;
    });
    if (found == archive.sections.end()) {
        throw std::invalid_argument("isolated Part section is missing: " + name);
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

void appendU32(std::vector<std::uint8_t>& target, const std::uint32_t value)
{
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        target.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

std::uint32_t readU32(const std::vector<std::uint8_t>& source,
                      const std::size_t offset)
{
    if (offset > source.size() || source.size() - offset < 4) {
        throw std::invalid_argument("isolated Part parameters are truncated");
    }
    std::uint32_t value = 0;
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(source[offset + shift / 8]) << shift;
    }
    return value;
}

void appendDouble(std::vector<std::uint8_t>& target, const double value)
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        target.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xffU));
    }
}

double readDouble(const std::vector<std::uint8_t>& source,
                  const std::size_t offset)
{
    if (offset > source.size() || source.size() - offset < 8) {
        throw std::invalid_argument("isolated Part parameters are truncated");
    }
    std::uint64_t bits = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        bits |= static_cast<std::uint64_t>(source[offset + shift / 8]) << shift;
    }
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) {
        throw std::invalid_argument("isolated Part parameter is not finite");
    }
    return value;
}

bool parseBool(const std::string& text, const char* name)
{
    if (text == "true" || text == "1") {
        return true;
    }
    if (text == "false" || text == "0") {
        return false;
    }
    throw std::invalid_argument(std::string("Part ") + name
                                + " must be true, false, 1, or 0");
}

long parseLong(const std::string& text, const char* name)
{
    long value = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (error != std::errc {} || end != text.data() + text.size()) {
        throw std::invalid_argument(std::string("Part ") + name + " is invalid");
    }
    return value;
}

double parseDouble(const std::string& text, const char* name)
{
    double value = 0.0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (error != std::errc {} || end != text.data() + text.size()
        || !std::isfinite(value)) {
        throw std::invalid_argument(std::string("Part ") + name + " is invalid");
    }
    return value;
}

std::vector<std::string> splitNames(const std::string& value)
{
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(',', begin);
        const auto name = value.substr(begin,
                                       end == std::string::npos
                                           ? std::string::npos
                                           : end - begin);
        if (name.empty()) {
            throw std::invalid_argument("Part Sweep section names must be nonempty");
        }
        result.push_back(name);
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    if (result.empty()) {
        throw std::invalid_argument("Part Sweep requires at least one section");
    }
    return result;
}

std::vector<FilletEdge> parseFilletEdges(const std::string& value)
{
    std::vector<FilletEdge> result;
    for (const auto& item : splitNames(value)) {
        const auto first = item.find(':');
        const auto second = first == std::string::npos
            ? std::string::npos
            : item.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos
            || item.find(':', second + 1) != std::string::npos) {
            throw std::invalid_argument(
                "Part Fillet edges must use edge:radius1:radius2 entries");
        }
        const long index = parseLong(item.substr(0, first), "Fillet edge index");
        const double radius1 = parseDouble(
            item.substr(first + 1, second - first - 1), "Fillet radius1");
        const double radius2 = parseDouble(item.substr(second + 1), "Fillet radius2");
        if (index <= 0 || index > std::numeric_limits<std::uint32_t>::max()
            || radius1 <= 0.0 || radius2 <= 0.0) {
            throw std::invalid_argument("Part Fillet edge and radii must be positive");
        }
        result.push_back(
            {static_cast<std::uint32_t>(index), radius1, radius2});
    }
    return result;
}

const Part::Feature& requireFeature(const App::Document& document,
                                    const std::string& name,
                                    const char* operation,
                                    const char* role)
{
    const auto* feature = dynamic_cast<const Part::Feature*>(
        document.getObject(name.c_str()));
    if (!feature) {
        throw std::invalid_argument(std::string("Part ") + operation + " " + role
                                    + " must identify an attached Part::Feature");
    }
    return *feature;
}

const Part::Feature& resolveFeature(const App::Document& document,
                                    const ObjectReference& reference,
                                    const char* operation,
                                    const char* role)
{
    const auto* feature = dynamic_cast<const Part::Feature*>(
        document.getObject(reference.name.c_str()));
    if (!feature
        || document.collaborationObjectIdentity(*feature) != reference.identity) {
        throw std::runtime_error(std::string("Part ") + operation + " " + role
                                 + " object identity is stale");
    }
    return *feature;
}

Part::Feature& resolveFeature(App::Document& document,
                              const ObjectReference& reference,
                              const char* operation,
                              const char* role)
{
    return const_cast<Part::Feature&>(resolveFeature(
        static_cast<const App::Document&>(document), reference, operation, role));
}

std::vector<App::DocumentRevisionKey> canonicalKeys(
    std::vector<App::DocumentRevisionKey> keys)
{
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

struct DependencyContract
{
    std::vector<App::DocumentRevisionKey> reads;
    std::vector<App::DocumentRevisionKey> writes;
    std::vector<App::DocumentRevisionPublicationRequest> effects;
};

DependencyContract dependencies(const App::Document& document,
                                const std::vector<const Part::Feature*>& inputs,
                                const Part::Feature& result)
{
    DependencyContract contract;
    contract.reads = {App::DocumentRevisionKey::documentStructure(),
                      App::DocumentRevisionKey::unknownModelMutation()};
    for (const auto* input : inputs) {
        const std::string name = input->getNameInDocument();
        contract.reads.push_back(App::DocumentRevisionKey::objectExistence(name));
        contract.reads.push_back(App::DocumentRevisionKey::objectModel(name));
        contract.reads.push_back(App::DocumentRevisionKey::objectStructure(name));
    }
    std::map<std::string, std::string> affected;
    affected.emplace(result.getNameInDocument(),
                     document.collaborationObjectIdentity(result));
    for (const auto* dependent : result.getInListRecursive()) {
        if (dependent && dependent->getDocument() == &document) {
            const char* name = dependent->getNameInDocument();
            if (!name || *name == '\0') {
                throw std::invalid_argument(
                    "Part collaborative dependent object has no document name");
            }
            affected.emplace(name,
                             document.collaborationObjectIdentity(*dependent));
        }
    }
    for (const auto& [name, identity] : affected) {
        contract.reads.push_back(App::DocumentRevisionKey::objectExistence(name));
        contract.reads.push_back(App::DocumentRevisionKey::objectModel(name));
        contract.reads.push_back(App::DocumentRevisionKey::objectStructure(name));
        const auto model = App::DocumentRevisionKey::objectModel(name);
        contract.writes.push_back(model);
        contract.effects.push_back({model, identity});
    }
    const auto resultStructure =
        App::DocumentRevisionKey::objectStructure(result.getNameInDocument());
    contract.writes.push_back(resultStructure);
    contract.effects.push_back(
        {resultStructure, document.collaborationObjectIdentity(result)});
    contract.reads = canonicalKeys(std::move(contract.reads));
    contract.writes = canonicalKeys(std::move(contract.writes));
    std::sort(contract.effects.begin(), contract.effects.end(), [](const auto& a, const auto& b) {
        return a.key < b.key;
    });
    return contract;
}

void rejectResultDependency(const Part::Feature& result,
                            const std::vector<const Part::Feature*>& inputs,
                            const char* operation)
{
    const auto dependents = result.getInListRecursive();
    if (std::ranges::any_of(inputs, [&](const auto* input) {
            return input == &result
                || std::ranges::find(dependents, input) != dependents.end();
        })) {
        throw std::invalid_argument(std::string("Part ") + operation
                                    + " result dependency closure contains an input");
    }
}

class CollaborativeShapeOperation final: public App::CollaborativeOperation
{
public:
    CollaborativeShapeOperation(std::string type,
                                std::string operation,
                                std::vector<ObjectReference> inputs,
                                ObjectReference result,
                                Part::TopoShape computed)
        : _type(std::move(type))
        , _operation(std::move(operation))
        , _inputs(std::move(inputs))
        , _result(std::move(result))
        , _computed(std::move(computed))
    {}

    std::string_view typeId() const noexcept override
    {
        return _type;
    }

    void apply(App::Document& document) const override
    {
        _appliedDigest.reset();
        std::vector<const Part::Feature*> inputs;
        inputs.reserve(_inputs.size());
        for (const auto& reference : _inputs) {
            inputs.push_back(&resolveFeature(document, reference, _operation.c_str(), "input"));
        }
        auto& result = resolveFeature(document, _result, _operation.c_str(), "result");
        if (result.getTypeId() != Part::Feature::getClassTypeId()) {
            throw std::runtime_error("Part collaborative result type changed");
        }
        rejectResultDependency(result, inputs, _operation.c_str());
        BRepBuilderAPI_Copy copy(_computed.getShape(), Standard_True, Standard_False);
        if (!copy.IsDone() || copy.Shape().IsNull()) {
            throw std::runtime_error("Part collaborative result application copy failed");
        }
        result.Shape.setValue(Part::TopoShape(copy.Shape()));
        // The detached worker result is the authoritative value for this exact
        // Part::Feature.  Leaving it touched makes the commit coordinator run a
        // redundant live recompute, which can rewrite valid fillet topology.
        result.purgeError();
        result.purgeTouched();
        _appliedDigest =
            Part::Internal::collaborativeGeometryShapeDigest(result.Shape.getValue());
    }

    App::CollaborativePostconditionResult checkPostcondition(
        const App::Document& document) const override
    {
        try {
            std::vector<const Part::Feature*> inputs;
            for (const auto& reference : _inputs) {
                inputs.push_back(
                    &resolveFeature(document, reference, _operation.c_str(), "input"));
            }
            const auto& result = resolveFeature(
                document, _result, _operation.c_str(), "result");
            if (result.getTypeId() != Part::Feature::getClassTypeId()
                || !validShape(result.Shape.getValue())) {
                return {false, "Part collaborative result is invalid"};
            }
            rejectResultDependency(result, inputs, _operation.c_str());
            if (!_appliedDigest
                || Part::Internal::collaborativeGeometryShapeDigest(
                       result.Shape.getValue())
                    != *_appliedDigest) {
                return {false, "Part collaborative result no longer matches worker output"};
            }
        }
        catch (...) {
            return {false, "Part collaborative object identity or topology is stale"};
        }
        return {true, {}};
    }

private:
    const std::string _type;
    const std::string _operation;
    const std::vector<ObjectReference> _inputs;
    const ObjectReference _result;
    const Part::TopoShape _computed;
    mutable std::optional<Part::Internal::CollaborativeBooleanDigest> _appliedDigest;
};

App::GeometryArchive resultArchive(const Part::TopoShape& result)
{
    if (!validShape(result.getShape())) {
        throw std::runtime_error("isolated Part worker produced an invalid shape");
    }
    App::GeometryArchive output;
    output.sections.push_back({"result.brep", encodeShape(result.getShape())});
    App::GeometryArchiveSection history;
    history.name = "element-history";
    App::GeometryArchiveError error;
    if (!App::GeometryArchiveCodec::encodeElementHistory({}, history, error)) {
        throw std::runtime_error(error.code + ": " + error.message);
    }
    output.sections.push_back(std::move(history));
    return output;
}

Part::TopoShape decodeResult(const App::GeometryArchive& output,
                             const std::string_view expectedType)
{
    if (output.metadata.operationType != expectedType || output.sections.size() != 2) {
        throw std::invalid_argument("isolated Part result contract is invalid");
    }
    App::GeometryElementHistory history;
    App::GeometryArchiveError error;
    if (!App::GeometryArchiveCodec::decodeElementHistory(
            requireSection(output, "element-history"), history, error)
        || !history.generated.empty() || !history.modified.empty()
        || !history.deleted.empty()) {
        throw std::invalid_argument(error.code.empty()
                                        ? "isolated Part result history is invalid"
                                        : error.code + ": " + error.message);
    }
    return Part::TopoShape(decodeShape(requireSection(output, "result.brep")));
}

App::GeometryArchive executeSweep(const App::GeometryArchive& input,
                                  const std::stop_token stopToken)
{
    if (input.metadata.operationType != Part::CollaborativeSweepOperationType
        || input.sections.size() < 3 || stopToken.stop_requested()) {
        throw std::invalid_argument("isolated Part Sweep request contract is invalid");
    }
    const auto& parameters = requireSection(input, "parameters").bytes;
    if (parameters.size() != 4 || parameters[0] > 1 || parameters[1] > 1
        || parameters[2] > 2 || parameters[3] > 1) {
        throw std::invalid_argument("isolated Part Sweep parameters are invalid");
    }
    std::vector<Part::TopoShape> shapes;
    shapes.emplace_back(decodeShape(requireSection(input, "spine.brep")));
    for (std::size_t index = 0; index + 2 < input.sections.size(); ++index) {
        shapes.emplace_back(decodeShape(requireSection(
            input, "section." + std::to_string(index) + ".brep")));
    }
    Part::TopoShape result(0);
    result.makeElementPipeShell(
        shapes,
        parameters[0] ? Part::MakeSolid::makeSolid : Part::MakeSolid::noSolid,
        parameters[1] ? Standard_True : Standard_False,
        static_cast<Part::TransitionMode>(parameters[2]),
        Part::OpCodes::Sweep);
    if (parameters[3]) {
        result.linearize(Part::LinearizeFace::linearizeFaces,
                         Part::LinearizeEdge::noEdges);
    }
    if (stopToken.stop_requested()) {
        throw std::runtime_error("isolated Part Sweep operation was cancelled");
    }
    return resultArchive(result);
}

App::GeometryArchive executeFillet(const App::GeometryArchive& input,
                                   const std::stop_token stopToken)
{
    if (input.metadata.operationType != Part::CollaborativeFilletOperationType
        || input.sections.size() != 2 || stopToken.stop_requested()) {
        throw std::invalid_argument("isolated Part Fillet request contract is invalid");
    }
    const auto& parameters = requireSection(input, "parameters").bytes;
    if (parameters.size() < 4) {
        throw std::invalid_argument("isolated Part Fillet parameters are truncated");
    }
    const std::uint32_t count = readU32(parameters, 0);
    if (count == 0 || count > 1'000'000
        || parameters.size() != 4ULL + static_cast<std::uint64_t>(count) * 20ULL) {
        throw std::invalid_argument("isolated Part Fillet edge count is invalid");
    }
    Part::TopoShape base(decodeShape(requireSection(input, "base.brep")));
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(base.getShape(), TopAbs_EDGE, edges);
    BRepFilletAPI_MakeFillet fillet(base.getShape());
    for (std::uint32_t item = 0; item < count; ++item) {
        const std::size_t offset = 4 + static_cast<std::size_t>(item) * 20;
        const std::uint32_t index = readU32(parameters, offset);
        const double radius1 = readDouble(parameters, offset + 4);
        const double radius2 = readDouble(parameters, offset + 12);
        if (index == 0 || index > static_cast<std::uint32_t>(edges.Extent())
            || radius1 <= 0.0 || radius2 <= 0.0) {
            throw std::invalid_argument("isolated Part Fillet edge selection is invalid");
        }
        fillet.Add(radius1, radius2, TopoDS::Edge(edges.FindKey(index)));
    }
    const TopoDS_Shape raw = fillet.Shape();
    if (raw.IsNull()) {
        throw std::runtime_error("isolated Part Fillet produced a null shape");
    }
    Part::TopoShape result(0);
    result.makeElementShape(fillet, base, Part::OpCodes::Fillet);
    if (stopToken.stop_requested()) {
        throw std::runtime_error("isolated Part Fillet operation was cancelled");
    }
    return resultArchive(result);
}

App::CollaborativeOperationPreparation prepareSweep(
    const App::Document& document,
    const App::CollaborativeOperationIntent& intent)
{
    static constexpr std::string_view required[] = {
        "spine", "sections", "result", "solid", "frenet", "transition", "linearize"};
    if (intent.arguments.size() != std::size(required)
        || std::ranges::any_of(required, [&](const auto name) {
               return !intent.arguments.contains(std::string(name));
           })) {
        throw std::invalid_argument(
            "Part Sweep intent requires spine, sections, result, solid, frenet, transition, and linearize");
    }
    const auto sectionNames = splitNames(intent.arguments.at("sections"));
    const std::string spineName = intent.arguments.at("spine");
    const std::string resultName = intent.arguments.at("result");
    if (spineName.empty() || resultName.empty()) {
        throw std::invalid_argument("Part Sweep object names must be nonempty");
    }
    const auto& spine = requireFeature(document, spineName, "Sweep", "spine");
    const auto& result = requireFeature(document, resultName, "Sweep", "result");
    if (result.getTypeId() != Part::Feature::getClassTypeId()) {
        throw std::invalid_argument("Part Sweep result must be an exact Part::Feature");
    }
    std::vector<const Part::Feature*> inputs {&spine};
    for (const auto& name : sectionNames) {
        inputs.push_back(&requireFeature(document, name, "Sweep", "section"));
    }
    rejectResultDependency(result, inputs, "Sweep");

    App::GeometryArchive archive;
    archive.sections.push_back(
        {"spine.brep", encodeShape(Part::Feature::getTopoShape(
                                       &spine,
                                       Part::ShapeOption::ResolveLink
                                           | Part::ShapeOption::Transform)
                                       .getShape())});
    for (std::size_t index = 1; index < inputs.size(); ++index) {
        archive.sections.push_back(
            {"section." + std::to_string(index - 1) + ".brep",
             encodeShape(Part::Feature::getTopoShape(
                             inputs[index],
                             Part::ShapeOption::ResolveLink
                                 | Part::ShapeOption::Transform)
                             .getShape())});
    }
    const long transition = parseLong(intent.arguments.at("transition"), "Sweep transition");
    if (transition < 0 || transition > 2) {
        throw std::invalid_argument("Part Sweep transition must be 0, 1, or 2");
    }
    archive.sections.push_back(
        {"parameters",
         {static_cast<std::uint8_t>(parseBool(intent.arguments.at("solid"), "Sweep solid")),
          static_cast<std::uint8_t>(parseBool(intent.arguments.at("frenet"), "Sweep frenet")),
          static_cast<std::uint8_t>(transition),
          static_cast<std::uint8_t>(parseBool(intent.arguments.at("linearize"), "Sweep linearize"))}});

    std::vector<ObjectReference> references;
    for (const auto* input : inputs) {
        references.push_back(
            {input->getNameInDocument(), document.collaborationObjectIdentity(*input)});
    }
    ObjectReference resultReference {
        resultName, document.collaborationObjectIdentity(result)};
    if (resultReference.identity.empty()
        || std::ranges::any_of(references, [](const auto& reference) {
               return reference.identity.empty();
           })) {
        throw std::invalid_argument("Part Sweep object identity must be nonempty");
    }
    auto contract = dependencies(document, inputs, result);
    App::GeometryJobRequest request;
    request.operationType = std::string(Part::CollaborativeSweepOperationType);
    request.coalescingKey = resultReference.identity;
    request.inputDigest = sectionDigest(archive.sections);
    request.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    App::CollaborativeOperationPreparation::IsolatedTask isolated {
        std::move(request),
        std::move(archive),
        [references = std::move(references), resultReference = std::move(resultReference)](
            const App::GeometryArchive& output) mutable {
            return std::make_unique<const CollaborativeShapeOperation>(
                std::string(Part::CollaborativeSweepOperationType),
                "Sweep",
                std::move(references),
                std::move(resultReference),
                decodeResult(output, Part::CollaborativeSweepOperationType));
        }};
    return {std::move(contract.reads),
            std::move(contract.writes),
            std::move(contract.effects),
            std::move(isolated)};
}

App::CollaborativeOperationPreparation prepareFillet(
    const App::Document& document,
    const App::CollaborativeOperationIntent& intent)
{
    static constexpr std::string_view required[] = {"base", "result", "edges"};
    if (intent.arguments.size() != std::size(required)
        || std::ranges::any_of(required, [&](const auto name) {
               return !intent.arguments.contains(std::string(name));
           })) {
        throw std::invalid_argument("Part Fillet intent requires base, result, and edges");
    }
    const std::string baseName = intent.arguments.at("base");
    const std::string resultName = intent.arguments.at("result");
    const auto selectedEdges = parseFilletEdges(intent.arguments.at("edges"));
    const auto& base = requireFeature(document, baseName, "Fillet", "base");
    const auto& result = requireFeature(document, resultName, "Fillet", "result");
    if (result.getTypeId() != Part::Feature::getClassTypeId()) {
        throw std::invalid_argument("Part Fillet result must be an exact Part::Feature");
    }
    std::vector<const Part::Feature*> inputs {&base};
    rejectResultDependency(result, inputs, "Fillet");
    App::GeometryArchive archive;
    archive.sections.push_back(
        {"base.brep", encodeShape(Part::Feature::getTopoShape(
                                      &base,
                                      Part::ShapeOption::ResolveLink
                                          | Part::ShapeOption::Transform)
                                      .getShape())});
    std::vector<std::uint8_t> parameters;
    appendU32(parameters, static_cast<std::uint32_t>(selectedEdges.size()));
    for (const auto& edge : selectedEdges) {
        appendU32(parameters, edge.index);
        appendDouble(parameters, edge.radius1);
        appendDouble(parameters, edge.radius2);
    }
    archive.sections.push_back({"parameters", std::move(parameters)});
    std::vector<ObjectReference> references {
        {baseName, document.collaborationObjectIdentity(base)}};
    ObjectReference resultReference {
        resultName, document.collaborationObjectIdentity(result)};
    if (references.front().identity.empty() || resultReference.identity.empty()) {
        throw std::invalid_argument("Part Fillet object identity must be nonempty");
    }
    auto contract = dependencies(document, inputs, result);
    App::GeometryJobRequest request;
    request.operationType = std::string(Part::CollaborativeFilletOperationType);
    request.coalescingKey = resultReference.identity;
    request.inputDigest = sectionDigest(archive.sections);
    request.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    App::CollaborativeOperationPreparation::IsolatedTask isolated {
        std::move(request),
        std::move(archive),
        [references = std::move(references), resultReference = std::move(resultReference)](
            const App::GeometryArchive& output) mutable {
            return std::make_unique<const CollaborativeShapeOperation>(
                std::string(Part::CollaborativeFilletOperationType),
                "Fillet",
                std::move(references),
                std::move(resultReference),
                decodeResult(output, Part::CollaborativeFilletOperationType));
        }};
    return {std::move(contract.reads),
            std::move(contract.writes),
            std::move(contract.effects),
            std::move(isolated)};
}

}  // namespace

void Part::ensureCollaborativeSweepFilletOperationsRegistered()
{
    static std::once_flag registered;
    std::call_once(registered, [] {
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(CollaborativeSweepOperationType), prepareSweep));
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(CollaborativeFilletOperationType), prepareFillet));
        auto& workers = App::Internal::GeometryWorkerOperationRegistry::instance();
        workers.registerOperation(std::string(CollaborativeSweepOperationType),
                                  executeSweep);
        workers.registerOperation(std::string(CollaborativeFilletOperationType),
                                  executeFillet);
    });
}
