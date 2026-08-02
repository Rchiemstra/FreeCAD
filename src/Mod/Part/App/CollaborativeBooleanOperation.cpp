// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborativeBooleanOperation.h"

#include "FCBRepAlgoAPI_BooleanOperation.h"
#include "FCBRepAlgoAPI_Common.h"
#include "FCBRepAlgoAPI_Cut.h"
#include "FCBRepAlgoAPI_Fuse.h"
#include "PartFeature.h"
#include "TopoShapeOpCode.h"

#include <App/CollaborativeOperation.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>

#include <BRepCheck_Analyzer.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Iterator.hxx>

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

enum class BooleanKind
{
    Cut,
    Fuse,
    Common
};

struct ObjectReference
{
    std::string name;
    std::string identity;
};

std::vector<App::DocumentRevisionKey> canonicalKeys(
    std::vector<App::DocumentRevisionKey> keys)
{
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

BooleanKind parseKind(const std::string& value)
{
    if (value == "cut") {
        return BooleanKind::Cut;
    }
    if (value == "fuse") {
        return BooleanKind::Fuse;
    }
    if (value == "common") {
        return BooleanKind::Common;
    }
    throw std::invalid_argument("Part Boolean kind must be cut, fuse, or common");
}

bool isValidShape(const TopoDS_Shape& shape)
{
    if (shape.IsNull()) {
        return false;
    }
    if (shape.ShapeType() == TopAbs_COMPOUND && !TopoDS_Iterator(shape).More()) {
        return false;
    }
    try {
        return BRepCheck_Analyzer(shape).IsValid();
    }
    catch (const Standard_Failure&) {
        return false;
    }
}

Part::TopoShape inputShape(const Part::Feature& feature)
{
    return Part::Feature::getTopoShape(
               &feature,
               Part::ShapeOption::ResolveLink | Part::ShapeOption::Transform);
}

const Part::Feature& requireFeature(const App::Document& document,
                                    const std::string& name,
                                    const char* role)
{
    const auto* object = document.getObject(name.c_str());
    const auto* feature = dynamic_cast<const Part::Feature*>(object);
    if (!feature) {
        throw std::invalid_argument(std::string("Part Boolean ") + role
                                    + " must identify an attached Part::Feature");
    }
    return *feature;
}

const Part::Feature& resolveFeature(const App::Document& document,
                                    const ObjectReference& reference,
                                    const char* role)
{
    const auto* object = document.getObject(reference.name.c_str());
    const auto* feature = dynamic_cast<const Part::Feature*>(object);
    if (!feature
        || document.collaborationObjectIdentity(*feature) != reference.identity) {
        throw std::runtime_error(std::string("Part Boolean ") + role
                                 + " object identity is stale");
    }
    return *feature;
}

Part::Feature& resolveFeature(App::Document& document,
                              const ObjectReference& reference,
                              const char* role)
{
    return const_cast<Part::Feature&>(resolveFeature(
        static_cast<const App::Document&>(document), reference, role));
}

bool hasHazardousInputDependency(const Part::Feature& result,
                                 const Part::Feature& base,
                                 const Part::Feature& tool)
{
    const auto dependents = result.getInListRecursive();
    return std::ranges::any_of(dependents, [&](const auto* dependent) {
        return dependent == static_cast<const App::DocumentObject*>(&base)
            || dependent == static_cast<const App::DocumentObject*>(&tool);
    });
}

std::map<std::string, std::string> freezeRecomputeClosure(
    const App::Document& document,
    const Part::Feature& result)
{
    std::map<std::string, std::string> affected;
    affected.emplace(result.getNameInDocument(),
                     document.collaborationObjectIdentity(result));
    for (const auto* dependent : result.getInListRecursive()) {
        if (!dependent || dependent->getDocument() != &document) {
            continue;
        }
        const char* name = dependent->getNameInDocument();
        if (!name || *name == '\0') {
            throw std::invalid_argument(
                "Part Boolean dependent object has no document name");
        }
        const std::string identity =
            document.collaborationObjectIdentity(*dependent);
        const auto [position, inserted] = affected.emplace(name, identity);
        if (!inserted && position->second != identity) {
            throw std::invalid_argument(
                "Part Boolean dependency graph has ambiguous object identity");
        }
    }
    return affected;
}

Part::TopoShape computeBoolean(BooleanKind kind,
                               const Part::TopoShape& base,
                               const Part::TopoShape& tool)
{
    try {
        std::unique_ptr<FCBRepAlgoAPI_BooleanOperation> operation;
        const char* operationCode = nullptr;
        switch (kind) {
            case BooleanKind::Cut:
                operation =
                    std::make_unique<FCBRepAlgoAPI_Cut>(base.getShape(), tool.getShape());
                operationCode = Part::OpCodes::Cut;
                break;
            case BooleanKind::Fuse:
                operation =
                    std::make_unique<FCBRepAlgoAPI_Fuse>(base.getShape(), tool.getShape());
                operationCode = Part::OpCodes::Fuse;
                break;
            case BooleanKind::Common:
                operation =
                    std::make_unique<FCBRepAlgoAPI_Common>(base.getShape(), tool.getShape());
                operationCode = Part::OpCodes::Common;
                break;
        }
        if (!operation->IsDone()) {
            throw std::runtime_error("Part Boolean calculation did not complete");
        }
        if (!isValidShape(operation->Shape())) {
            throw std::runtime_error("Part Boolean produced a null or invalid shape");
        }

        std::vector<Part::TopoShape> inputs {base, tool};
        Part::TopoShape result(0);
        result.makeElementShape(*operation, inputs, operationCode);
        if (!isValidShape(result.getShape())) {
            throw std::runtime_error("Part Boolean produced a null or invalid mapped shape");
        }
        return result;
    }
    catch (const Standard_Failure& failure) {
        const char* detail = failure.GetMessageString();
        throw std::runtime_error(detail && *detail
                                     ? std::string("Part Boolean calculation failed: ") + detail
                                     : "Part Boolean calculation failed");
    }
}

class CollaborativeBooleanOperation final: public App::CollaborativeOperation
{
public:
    CollaborativeBooleanOperation(ObjectReference base,
                                  ObjectReference tool,
                                  ObjectReference result,
                                  BooleanKind kind)
        : _base(std::move(base))
        , _tool(std::move(tool))
        , _result(std::move(result))
        , _kind(kind)
    {}

    std::string_view typeId() const noexcept override
    {
        return Part::CollaborativeBooleanOperationType;
    }

    void apply(App::Document& document) const override
    {
        auto& base = resolveFeature(document, _base, "base");
        auto& tool = resolveFeature(document, _tool, "tool");
        auto& result = resolveFeature(document, _result, "result");
        if (result.getTypeId() != Part::Feature::getClassTypeId()) {
            throw std::runtime_error("Part Boolean result type changed");
        }
        if (hasHazardousInputDependency(result, base, tool)) {
            throw std::runtime_error(
                "Part Boolean result dependency closure contains an input");
        }

        const Part::TopoShape baseShape = inputShape(base);
        const Part::TopoShape toolShape = inputShape(tool);
        if (!isValidShape(baseShape.getShape())) {
            throw std::runtime_error("Part Boolean base shape is null or invalid");
        }
        if (!isValidShape(toolShape.getShape())) {
            throw std::runtime_error("Part Boolean tool shape is null or invalid");
        }

        result.Shape.setValue(computeBoolean(_kind, baseShape, toolShape));
    }

    App::CollaborativePostconditionResult checkPostcondition(
        const App::Document& document) const override
    {
        try {
            const auto& base = resolveFeature(document, _base, "base");
            const auto& tool = resolveFeature(document, _tool, "tool");
            const auto& result = resolveFeature(document, _result, "result");
            if (result.getTypeId() != Part::Feature::getClassTypeId()) {
                return {false, "Part Boolean result is not an exact Part::Feature"};
            }
            if (hasHazardousInputDependency(result, base, tool)) {
                return {false,
                        "Part Boolean result dependency closure contains an input"};
            }
            if (!isValidShape(result.Shape.getValue())) {
                return {false, "Part Boolean result shape is null, empty, or invalid"};
            }
        }
        catch (...) {
            return {false, "Part Boolean object identity or topology is stale"};
        }
        return {true, {}};
    }

private:
    const ObjectReference _base;
    const ObjectReference _tool;
    const ObjectReference _result;
    const BooleanKind _kind;
};

App::CollaborativeOperationPreparation prepareBoolean(
    const App::Document& document,
    const App::CollaborativeOperationIntent& intent)
{
    static constexpr std::string_view requiredArguments[] = {"base", "tool", "result", "kind"};
    if (intent.arguments.size() != std::size(requiredArguments)
        || std::ranges::any_of(requiredArguments, [&](std::string_view name) {
               return !intent.arguments.contains(std::string(name));
           })) {
        throw std::invalid_argument(
            "Part Boolean intent requires exactly base, tool, result, and kind");
    }

    const std::string baseName = intent.arguments.at("base");
    const std::string toolName = intent.arguments.at("tool");
    const std::string resultName = intent.arguments.at("result");
    if (baseName.empty() || toolName.empty() || resultName.empty()) {
        throw std::invalid_argument("Part Boolean object names must be nonempty");
    }

    const auto& base = requireFeature(document, baseName, "base");
    const auto& tool = requireFeature(document, toolName, "tool");
    const auto& result = requireFeature(document, resultName, "result");
    if (result.getTypeId() != Part::Feature::getClassTypeId()) {
        throw std::invalid_argument(
            "Part Boolean result must be an exact Part::Feature");
    }
    if (hasHazardousInputDependency(result, base, tool)) {
        throw std::invalid_argument(
            "Part Boolean result dependency closure contains an input");
    }
    if (!isValidShape(inputShape(base).getShape())) {
        throw std::invalid_argument("Part Boolean base shape is null or invalid");
    }
    if (!isValidShape(inputShape(tool).getShape())) {
        throw std::invalid_argument("Part Boolean tool shape is null or invalid");
    }

    const BooleanKind kind = parseKind(intent.arguments.at("kind"));
    ObjectReference baseReference {baseName, document.collaborationObjectIdentity(base)};
    ObjectReference toolReference {toolName, document.collaborationObjectIdentity(tool)};
    ObjectReference resultReference {resultName, document.collaborationObjectIdentity(result)};
    if (baseReference.identity.empty() || toolReference.identity.empty()
        || resultReference.identity.empty()) {
        throw std::invalid_argument("Part Boolean object identity must be nonempty");
    }

    const auto affected = freezeRecomputeClosure(document, result);
    std::vector<App::DocumentRevisionKey> reads {
        App::DocumentRevisionKey::objectExistence(baseName),
        App::DocumentRevisionKey::objectModel(baseName),
        App::DocumentRevisionKey::objectExistence(toolName),
        App::DocumentRevisionKey::objectModel(toolName),
        App::DocumentRevisionKey::unknownModelMutation()};
    std::vector<App::DocumentRevisionKey> writes;
    std::vector<App::DocumentRevisionPublicationRequest> effects;
    reads.reserve(reads.size() + affected.size() * 3);
    writes.reserve(affected.size() + 1);
    effects.reserve(affected.size() + 1);
    for (const auto& [affectedName, affectedIdentity] : affected) {
        reads.push_back(App::DocumentRevisionKey::objectExistence(affectedName));
        reads.push_back(App::DocumentRevisionKey::objectModel(affectedName));
        reads.push_back(App::DocumentRevisionKey::objectStructure(affectedName));
        const auto model = App::DocumentRevisionKey::objectModel(affectedName);
        writes.push_back(model);
        effects.push_back({model, affectedIdentity});
    }
    const auto resultStructure =
        App::DocumentRevisionKey::objectStructure(resultName);
    writes.push_back(resultStructure);
    effects.push_back({resultStructure, resultReference.identity});
    reads = canonicalKeys(std::move(reads));
    writes = canonicalKeys(std::move(writes));
    std::sort(effects.begin(), effects.end(), [](const auto& left, const auto& right) {
        return left.key < right.key;
    });

    return {std::move(reads),
            std::move(writes),
            std::move(effects),
            std::make_unique<const CollaborativeBooleanOperation>(std::move(baseReference),
                                                                   std::move(toolReference),
                                                                   std::move(resultReference),
                                                                   kind)};
}

}  // namespace

void Part::ensureCollaborativeBooleanOperationRegistered()
{
    static std::once_flag registered;
    std::call_once(registered, [] {
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(CollaborativeBooleanOperationType),
            prepareBoolean));
    });
}
