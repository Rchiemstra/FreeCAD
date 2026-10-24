// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Precision.hxx>
#include <nlohmann/json.hpp>
#include <App/Document.h>
#include <Base/Writer.h>
#include "BSplineOffset.h"
#include "OffsetChain.h"
#include "GeometryFacade.h"
#include "Sketch.h"
#include "LinkedOffset.h"
#include "SketchObject.h"

namespace Sketcher
{
using Json = nlohmann::json;
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
    LinkedOffsetMember, uuid, type, reversed, seam, signature
)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
    LinkedOffsetParameters,
    generator,
    distance,
    tolerance,
    normal,
    closed,
    orientation,
    seam,
    join,
    joinTolerance,
    maxWork,
    maxDepth,
    maxInputPoles,
    maxOutputPoles,
    maxSegments,
    maxDegree,
    maxValidationSpans,
    maxRefinementDepth
)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(
    LinkedOffsetRelation, id, sources, outputs, parameters
)

namespace
{
// Require every version-1 field and its JSON type before conversion. In particular,
// json::get<int>() alone accepts fractional values and narrowing integer overflow.
void requireShape(const Json& value, const Json& model)
{
    if (model.is_object()) {
        if (!value.is_object()) {
            throw std::invalid_argument("Expected an object.");
        }
        for (const auto& item : model.items()) {
            requireShape(value.at(item.key()), item.value());
        }
    }
    else if (model.is_array()) {
        if (!value.is_array() || value.size() != model.size()) {
            throw std::invalid_argument("Invalid array size.");
        }
        for (size_t i = 0; i < model.size(); ++i) {
            requireShape(value[i], model[i]);
        }
    }
    else if (model.is_number_integer()) {
        if (!value.is_number_integer() || value.get<double>() < std::numeric_limits<int>::min()
            || value.get<double>() > std::numeric_limits<int>::max()) {
            throw std::invalid_argument("Expected a bounded integer.");
        }
    }
    else if (model.is_number()) {
        if (!value.is_number() || !std::isfinite(value.get<double>())) {
            throw std::invalid_argument("Expected a finite number.");
        }
    }
    else if (value.type() != model.type()) {
        throw std::invalid_argument("Invalid field type.");
    }
}

void validateParameters(const LinkedOffsetParameters& p)
{
    const auto bounded = [](int value, int low, int high) { return value >= low && value <= high; };
    const double normalLength = Base::Vector3d(p.normal[0], p.normal[1], p.normal[2]).Length();
    if ((p.generator != "BSpline" && p.generator != "Chain") || !std::isfinite(p.distance)
        || !std::isfinite(p.tolerance) || p.tolerance < Precision::Confusion()
        || std::abs(p.distance) <= 2 * p.tolerance || !std::isfinite(normalLength)
        || !std::isfinite(p.normal[0]) || !std::isfinite(p.normal[1])
        || !std::isfinite(p.normal[2]) || normalLength <= Precision::Confusion()
        || !bounded(p.orientation, -1, 1) || !std::isfinite(p.seam) || p.join != "G1"
        || !std::isfinite(p.joinTolerance) || p.joinTolerance < 0
        || p.joinTolerance > Precision::Confusion() || !bounded(p.maxWork, 1, 10000000)
        || !bounded(p.maxDepth, 1, 24) || !bounded(p.maxInputPoles, 2, 1024)
        || !bounded(p.maxOutputPoles, 2, 4096) || !bounded(p.maxSegments, 1, 256)
        || !bounded(p.maxDegree, 3, 14) || !bounded(p.maxValidationSpans, 16, 65536)
        || !bounded(p.maxRefinementDepth, 1, 24)) {
        throw std::invalid_argument("Invalid generator parameters.");
    }
}

LinkedOffsetRelation parseRelation(const std::string& record)
{
    const auto json = Json::parse(record);
    requireShape(json.at("id"), Json(""));
    requireShape(json.at("parameters"), Json(LinkedOffsetParameters {}));
    for (const auto* field : {"sources", "outputs"}) {
        const auto& entries = json.at(field);
        if (!entries.is_array() || entries.empty() || entries.size() > 256) {
            throw std::invalid_argument("Invalid member list.");
        }
        for (const auto& entry : entries) {
            requireShape(entry, Json(LinkedOffsetMember {}));
            if (entry.at("uuid").get<std::string>().empty()
                || entry.at("type").get<std::string>().empty()
                || entry.at("signature").get<std::string>().empty()) {
                throw std::invalid_argument("Empty member identity, type or signature.");
            }
        }
    }
    auto relation = json.get<LinkedOffsetRelation>();
    if (relation.id.empty()) {
        throw std::invalid_argument("Empty relation identity.");
    }
    validateParameters(relation.parameters);
    return relation;
}

std::shared_ptr<const LinkedOffsetExtension> extension(const Part::Geometry* geometry)
{
    if (!geometry->hasExtension(LinkedOffsetExtension::getClassTypeId())) {
        return {};
    }
    return std::dynamic_pointer_cast<const LinkedOffsetExtension>(
        geometry->getExtension(LinkedOffsetExtension::getClassTypeId()).lock()
    );
}

std::shared_ptr<LinkedOffsetExtension> ensure(Part::Geometry* geometry)
{
    if (!extension(geometry)) {
        geometry->setExtension(std::make_unique<LinkedOffsetExtension>());
    }
    return std::dynamic_pointer_cast<LinkedOffsetExtension>(
        geometry->getExtension(LinkedOffsetExtension::getClassTypeId()).lock()
    );
}

std::string signature(const Part::Geometry& geometry)
{
    Base::StringWriter writer;
    // Match the precision used by Base::ZipWriter for document persistence.
    writer.Stream() << std::setprecision(std::numeric_limits<double>::digits10 + 1);
    // Full native representation, without extensions/UUIDs, for later stale checks.
    std::unique_ptr<Part::Geometry> bare(geometry.clone());
    for (const auto& weak : bare->getExtensions()) {
        if (const auto ext = weak.lock()) {
            bare->deleteExtension(ext->getTypeId());
        }
    }
    bare->Save(writer);
    return writer.getString();
}

LinkedOffsetMember member(Part::Geometry* geometry, bool reversed)
{
    const auto* curve = dynamic_cast<const Part::GeomCurve*>(geometry);
    const double seam = curve ? (reversed ? curve->getLastParameter() : curve->getFirstParameter())
                              : 0;
    return {
        ensure(geometry)->uuid,
        std::string(geometry->getTypeId().getName()),
        reversed,
        seam,
        signature(*geometry)
    };
}
std::string updateRecord(const std::string&, const LinkedOffsetRelation&);
std::string outputReason(const SketchObject&, const LinkedOffsetInspection&);
}  // namespace

void remapLinkedOffsetCopies(const std::vector<Part::Geometry*>& copies)
{
    std::map<std::string, std::vector<size_t>> identities;
    std::map<std::string, std::vector<std::shared_ptr<const LinkedOffsetExtension>>> groups;
    for (size_t i = 0; i < copies.size(); ++i) {
        if (const auto old = extension(copies[i])) {
            identities[old->uuid].push_back(i);
            if (!old->membership.empty()) {
                groups[old->membership].push_back(old);
            }
            auto fresh = old->copy();
            auto* ext = static_cast<LinkedOffsetExtension*>(fresh.get());
            ext->uuid = LinkedOffsetExtension().uuid;
            ext->membership.clear();
            ext->invalidatedUuid.clear();
            // Detached copies keep their opaque payload; empty membership makes
            // it inactive. Complete groups below receive a remapped record.
            copies[i]->setExtension(std::move(fresh));
        }
    }
    for (const auto& [id, members] : groups) {
        // Every source and output must occur exactly once in this insertion batch.
        // Partial, malformed and opaque groups stay detached with fresh identities.
        try {
            std::shared_ptr<const LinkedOffsetExtension> owner;
            for (const auto& ext : members) {
                if (ext->version != "1" || (!ext->record.empty() && owner)) {
                    throw std::invalid_argument("Unavailable copied group.");
                }
                if (!ext->record.empty()) {
                    owner = ext;
                }
            }
            if (!owner) {
                continue;
            }
            auto relation = parseRelation(owner->record);
            if (relation.id != id || relation.outputs.front().uuid != owner->uuid
                || relation.outputs.size() != members.size()) {
                continue;
            }
            std::set<std::string> expected;
            std::set<std::string> outputs;
            for (const auto& entry : relation.outputs) {
                outputs.insert(entry.uuid);
            }
            for (const auto& ext : members) {
                if (!outputs.count(ext->uuid)) {
                    throw std::invalid_argument("Unexpected copied output.");
                }
            }
            const auto remap = [&](auto& entries) {
                for (auto& entry : entries) {
                    const auto found = identities.find(entry.uuid);
                    if (found == identities.end() || found->second.size() != 1
                        || !expected.insert(entry.uuid).second) {
                        throw std::invalid_argument("Incomplete copied relation.");
                    }
                    auto* geometry = copies[found->second.front()];
                    if (entry.type != geometry->getTypeId().getName()
                        || extension(geometry)->version != "1") {
                        throw std::invalid_argument("Changed copied member type.");
                    }
                    entry = member(geometry, entry.reversed);
                }
            };
            remap(relation.sources);
            remap(relation.outputs);
            relation.id = LinkedOffsetExtension().uuid;
            relation.parameters.seam = relation.sources.front().seam;
            const auto record = updateRecord(owner->record, relation);
            for (const auto& output : relation.outputs) {
                for (auto* geometry : copies) {
                    if (const auto ext = extension(geometry); ext && ext->uuid == output.uuid) {
                        ensure(geometry)->membership = relation.id;
                        if (output.uuid == relation.outputs.front().uuid) {
                            ensure(geometry)->record = record;
                        }
                    }
                }
            }
        }
        catch (const std::exception&) {
            // Preserve copied geometry even if its relation cannot be copied.
        }
    }
}

void invalidateLinkedOffsetAncestry(Part::Geometry& geometry)
{
    if (extension(&geometry)) {
        auto ext = ensure(&geometry);
        ext->invalidatedUuid = ext->uuid;
        ext->uuid = LinkedOffsetExtension().uuid;
    }
}

void invalidateLinkedOffsetAncestry(SketchObject& sketch, int index)
{
    const auto* geometry = sketch.getGeometry(index);
    if (extension(geometry)) {
        std::unique_ptr<Part::Geometry> replacement(geometry->clone());
        invalidateLinkedOffsetAncestry(*replacement);
        sketch.Geometry.set1Value(index, std::move(replacement));
    }
}

void attachLinkedOffset(
    const std::vector<Part::Geometry*>& geometry,
    const std::vector<std::pair<int, bool>>& sources,
    const std::vector<int>& outputs,
    const LinkedOffsetParameters& parameters
)
{
    if (sources.empty() || outputs.empty()) {
        throw std::invalid_argument("An offset needs sources and outputs.");
    }
    validateParameters(parameters);
    std::set<int> indices;
    const auto check = [&](int index) {
        if (index < 0 || size_t(index) >= geometry.size() || !indices.insert(index).second) {
            throw std::invalid_argument("Invalid or repeated offset member.");
        }
        if (const auto ext = extension(geometry[index]); ext && ext->version != "1") {
            throw std::invalid_argument("Future offset metadata is read-only.");
        }
    };
    for (const auto& [index, reversed] : sources) {
        (void)reversed;
        check(index);
        if (const auto ext = extension(geometry[index]); ext && !ext->membership.empty()) {
            throw std::invalid_argument("Linked outputs cannot be linked offset sources.");
        }
    }
    for (int index : outputs) {
        check(index);
    }
    LinkedOffsetRelation relation;
    relation.id = LinkedOffsetExtension().uuid;
    relation.parameters = parameters;
    for (const auto& [index, reversed] : sources) {
        relation.sources.push_back(member(geometry[index], reversed));
    }
    relation.parameters.seam = relation.sources.front().seam;
    for (int index : outputs) {
        // Generated analytic pieces may carry extensions copied from a source.
        geometry[index]->setExtension(std::make_unique<LinkedOffsetExtension>());
        relation.outputs.push_back(member(geometry[index], false));
        auto ext = ensure(geometry[index]);
        ext->membership = relation.id;
        ext->record.clear();
    }
    ensure(geometry[outputs.front()])->record = Json(relation).dump();
}

std::vector<LinkedOffsetInspection> listLinkedOffsets(const SketchObject& sketch)
{
    const auto& geometry = sketch.Geometry.getValues();
    std::map<std::string, std::vector<int>> identities;
    std::set<std::string> ignored;
    std::set<std::string> invalidated;
    std::map<std::string, std::vector<std::shared_ptr<const LinkedOffsetExtension>>> groups;
    for (size_t i = 0; i < geometry.size(); ++i) {
        if (const auto ext = extension(geometry[i])) {
            identities[ext->uuid].push_back(int(i));
            invalidated.insert(ext->invalidatedUuid);
            if (ext->version != "1" && !ext->membership.empty()) {
                ignored.insert(ext->membership);
            }
            if (ext->version == "1" && !ext->membership.empty()) {
                groups[ext->membership].push_back(ext);
            }
        }
    }
    std::vector<LinkedOffsetInspection> result;
    for (const auto& [id, members] : groups) {
        if (ignored.count(id)) {
            continue;
        }
        LinkedOffsetInspection inspection;
        inspection.relation.id = id;
        const auto fail = [&](LinkedOffsetStatus status, const char* reason) {
            inspection.status = status;
            inspection.reason = reason;
        };
        int owners = 0;
        std::string ownerUuid;
        for (const auto& ext : members) {
            if (!ext->record.empty()) {
                ++owners;
                ownerUuid = ext->uuid;
                try {
                    inspection.relation = parseRelation(ext->record);
                }
                catch (const std::exception&) {
                    fail(LinkedOffsetStatus::InvalidRecord, "Invalid version-1 relation record.");
                }
            }
        }
        if (inspection.status == LinkedOffsetStatus::InvalidRecord) {
            result.push_back(std::move(inspection));
            continue;
        }
        if (owners == 0) {
            fail(LinkedOffsetStatus::MissingOutput, "Designated output is missing.");
        }
        else if (owners != 1 || inspection.relation.id != id) {
            fail(LinkedOffsetStatus::Ambiguous, "Multiple or inconsistent relation owners.");
        }
        std::set<std::string> expectedIds;
        for (const auto& entry : inspection.relation.sources) {
            if (!expectedIds.insert(entry.uuid).second) {
                fail(LinkedOffsetStatus::Ambiguous, "Repeated relation member.");
            }
        }
        for (const auto& entry : inspection.relation.outputs) {
            if (!expectedIds.insert(entry.uuid).second) {
                fail(LinkedOffsetStatus::Ambiguous, "Repeated relation member.");
            }
        }
        std::set<std::string> outputIds;
        for (const auto& entry : inspection.relation.outputs) {
            outputIds.insert(entry.uuid);
        }
        for (const auto& ext : members) {
            if (ext->uuid.empty() || identities.at(ext->uuid).size() != 1
                || (owners == 1 && !outputIds.count(ext->uuid))) {
                fail(LinkedOffsetStatus::Ambiguous, "Unexpected or duplicate output member.");
            }
        }
        // Duplicate UUIDs take precedence over missing/type-changed members.
        for (const auto& uuid : expectedIds) {
            const auto found = identities.find(uuid);
            if (found != identities.end() && found->second.size() > 1) {
                fail(LinkedOffsetStatus::Ambiguous, "Duplicate geometry UUID.");
            }
        }
        if (owners == 1 && (inspection.relation.sources.empty()
            || inspection.relation.outputs.empty()
            || inspection.relation.outputs.front().uuid != ownerUuid)) {
            fail(LinkedOffsetStatus::Ambiguous, "Invalid designated output.");
        }
        const auto resolve = [&](const auto& expected, bool output, auto& resolved) {
            for (const auto& entry : expected) {
                const auto found = identities.find(entry.uuid);
                if (found == identities.end()) {
                    if (invalidated.count(entry.uuid)) {
                        fail(LinkedOffsetStatus::TopologyChanged,
                             "Member topology changed; repair the source or Detach the offset.");
                        return;
                    }
                    fail(
                        output ? LinkedOffsetStatus::MissingOutput
                               : LinkedOffsetStatus::MissingSource,
                        output ? "Output geometry is missing." : "Source geometry is missing."
                    );
                    return;
                }
                const int index = found->second.front();
                if (extension(geometry[index])->version != "1") {
                    fail(LinkedOffsetStatus::InvalidRecord, "Member metadata is unavailable.");
                    return;
                }
                if (entry.type != geometry[index]->getTypeId().getName()) {
                    fail(LinkedOffsetStatus::TypeChanged, "Geometry type changed.");
                    return;
                }
                if (output && extension(geometry[index])->membership != id) {
                    fail(LinkedOffsetStatus::MissingOutput, "Output membership is missing.");
                    return;
                }
                resolved.push_back(index);
            }
        };
        if (inspection.status == LinkedOffsetStatus::Ok) {
            resolve(inspection.relation.sources, false, inspection.sources);
        }
        if (inspection.status == LinkedOffsetStatus::Ok) {
            resolve(inspection.relation.outputs, true, inspection.outputs);
        }
        result.push_back(std::move(inspection));
    }
    return result;
}

const char* linkedOffsetStatusName(LinkedOffsetStatus status)
{
    switch (status) {
        case LinkedOffsetStatus::Ok:
            return "Ok";
        case LinkedOffsetStatus::MissingSource:
            return "MissingSource";
        case LinkedOffsetStatus::TopologyChanged:
            return "TopologyChanged";
        case LinkedOffsetStatus::Ambiguous:
            return "Ambiguous";
        case LinkedOffsetStatus::TypeChanged:
            return "TypeChanged";
        case LinkedOffsetStatus::MissingOutput:
            return "MissingOutput";
        case LinkedOffsetStatus::InvalidRecord:
            return "InvalidRecord";
    }
    return "Ambiguous";
}

std::string linkedOffsetsJson(const SketchObject& sketch)
{
    Json result = Json::array();
    for (const auto& inspection : listLinkedOffsets(sketch)) {
        Json entry = inspection.relation;
        entry["version"] = 1;
        entry["status"] = linkedOffsetStatusName(inspection.status);
        entry["reason"] = inspection.reason;
        bool stale = false;
        if (inspection.status == LinkedOffsetStatus::Ok) {
            entry["refreshReason"] = outputReason(sketch, inspection);
            for (size_t i = 0; i < inspection.sources.size(); ++i) {
                stale |= signature(*sketch.getGeometry(inspection.sources[i]))
                    != inspection.relation.sources[i].signature;
            }
            if (stale && entry["refreshReason"] == "") {
                entry["refreshReason"] = "Source edited; explicit Refresh required.";
            }
        }
        entry["stale"] = stale;
        entry["sourceIndices"] = inspection.sources;
        entry["outputIndices"] = inspection.outputs;
        result.push_back(std::move(entry));
    }
    return result.dump();
}
namespace
{
// Merge known fields individually: extra keys in version-1 records remain opaque.
std::string updateRecord(const std::string& previous, const LinkedOffsetRelation& relation)
{
    auto record = Json::parse(previous);
    const Json current = relation;
    record["id"] = relation.id;
    record["parameters"].update(current["parameters"]);
    for (const auto* field : {"sources", "outputs"}) {
        for (size_t i = 0; i < current[field].size(); ++i) {
            record[field][i].update(current[field][i]);
        }
    }
    return record.dump();
}

std::string blockToken(const Constraint& constraint)
{
    const auto metadata = Json::parse(constraint.MetaData, nullptr, false);
    if (!metadata.is_object() || !metadata.contains("linkedOffsetBlock")
        || !metadata["linkedOffsetBlock"].is_string()) {
        return {};
    }
    return metadata["linkedOffsetBlock"].get<std::string>();
}

std::string inputSnapshot(const SketchObject& sketch)
{
    Base::StringWriter writer;
    writer.Stream() << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (const auto* curve : sketch.getCompleteGeometry()) {
        curve->Save(writer);
    }
    sketch.Constraints.Save(writer);
    return writer.getString();
}

std::string outputReason(const SketchObject& sketch, const LinkedOffsetInspection& item)
{
    const auto& geometry = sketch.Geometry.getValues();
    const auto record = Json::parse(extension(geometry[item.outputs.front()])->record);
    const auto tokens = record.value("ownedBlocks", Json::array());
    if (!tokens.is_array() || tokens.size() != item.outputs.size()) {
        return "Block ownership is unavailable; Detach before editing this legacy offset.";
    }
    for (size_t i = 0; i < item.outputs.size(); ++i) {
        const int index = item.outputs[i];
        if (signature(*geometry[index]) != item.relation.outputs[i].signature
            || !GeometryFacade::getBlocked(geometry[index])
            || GeometryFacade::getConstruction(geometry[index])) {
            return "Output was edited or unblocked; Detach to preserve the edits.";
        }
        if (!tokens[i].is_string() || tokens[i].get<std::string>().empty()) {
            return "Invalid Block ownership; Detach the offset.";
        }
        int blocks = 0;
        for (const auto* constraint : sketch.Constraints.getValues()) {
            if (!constraint->involvesGeoId(index)) {
                continue;
            }
            if (constraint->Type == Block && constraint->getGeoId(0) == index
                && constraint->isActive && constraint->isDriving
                && blockToken(*constraint) == tokens[i].get<std::string>()) {
                ++blocks;
                continue;
            }
            if (constraint->isDriving || !constraint->isDimensional()
                || constraint->Type == Weight || constraint->Type == SnellsLaw
                || constraint->InternalAlignmentIndex != -1) {
                return "Output has a driving or helper constraint; Detach before refreshing.";
            }
            // Only curve/end-point dimensions have a stable meaning across refits.
            for (size_t j = 0; j < constraint->getElementsSize(); ++j) {
                if (constraint->getGeoId(j) == index
                    && constraint->getPosId(j) != PointPos::none
                    && constraint->getPosId(j) != PointPos::start
                    && constraint->getPosId(j) != PointPos::end) {
                    return "Referenced subelement cannot be mapped; Detach the offset.";
                }
            }
        }
        if (blocks != 1) {
            return "Owned Block was removed or changed; Detach to preserve the edits.";
        }
    }
    return {};
}

void regenerate(std::vector<Part::Geometry*>& geometry, LinkedOffsetInspection& item)
{
    auto& relation = item.relation;
    const auto& p = relation.parameters;
    validateParameters(p);
    if (p.seam != relation.sources.front().seam) {
        throw std::invalid_argument("TopologyChanged: inconsistent stored seam.");
    }
    std::vector<std::unique_ptr<Part::Geometry>> sources;
    std::vector<OffsetChainPiece> chain;
    for (size_t i = 0; i < item.sources.size(); ++i) {
        auto* source = geometry[item.sources[i]];
        if (!extension(source)->membership.empty() || GeometryFacade::isInternalAligned(source)
            || GeometryFacade::getConstruction(source)) {
            throw std::invalid_argument(
                "Linked sources, construction geometry and internal helpers are unsupported.");
        }
        const auto* curve = dynamic_cast<const Part::GeomCurve*>(source);
        if (!curve || (relation.sources[i].reversed ? curve->getLastParameter()
                                                    : curve->getFirstParameter())
                          != relation.sources[i].seam) {
            throw std::invalid_argument("TopologyChanged: source interval or seam changed.");
        }
        sources.emplace_back(source->clone());
        auto* oriented = static_cast<Part::GeomCurve*>(sources.back().get());
        if (relation.sources[i].reversed) {
            oriented->reverse();
        }
        chain.push_back({oriented, i});
    }
    std::vector<std::unique_ptr<Part::GeomCurve>> generated;
    if (p.generator == "BSpline") {
        const auto* source = dynamic_cast<const Part::GeomBSplineCurve*>(chain.front().geometry);
        if (chain.size() != 1 || !source) {
            throw std::invalid_argument("Invalid single B-spline source.");
        }
        const auto native = Handle(Geom_BSplineCurve)::DownCast(source->handle());
        if ((native->IsClosed() || native->IsPeriodic()) != p.closed) {
            throw std::invalid_argument("TopologyChanged: source closure changed.");
        }
        BSplineOffsetOptions options;
        options.distance = p.distance;
        options.tolerance = p.tolerance;
        options.sketchNormal = Base::Vector3d(p.normal[0], p.normal[1], p.normal[2]);
        options.maxInputPoles = p.maxInputPoles;
        options.maxOutputPoles = p.maxOutputPoles;
        options.maxSegments = p.maxSegments;
        options.maxDegree = p.maxDegree;
        options.maxValidationSpans = p.maxValidationSpans;
        options.maxRefinementDepth = p.maxRefinementDepth;
        auto result = makeBSplineOffset(*source, options);
        if (!result.curve) {
            throw std::invalid_argument(result.message);
        }
        if (result.orientation != p.orientation) {
            throw std::invalid_argument("TopologyChanged: source orientation changed.");
        }
        generated.push_back(std::move(result.curve));
    }
    else {
        OffsetChainOptions options;
        options.tolerance = p.tolerance;
        options.sketchNormal = Base::Vector3d(p.normal[0], p.normal[1], p.normal[2]);
        options.closed = p.closed;
        options.joinTolerance = p.joinTolerance;
        options.maxWork = p.maxWork;
        options.maxDepth = p.maxDepth;
        options.splineOptions.maxInputPoles = p.maxInputPoles;
        options.splineOptions.maxOutputPoles = p.maxOutputPoles;
        options.splineOptions.maxSegments = p.maxSegments;
        options.splineOptions.maxDegree = p.maxDegree;
        options.splineOptions.maxValidationSpans = p.maxValidationSpans;
        options.splineOptions.maxRefinementDepth = p.maxRefinementDepth;
        auto result = makeOffsetChain(chain, p.distance, options);
        if (result.status != OffsetChainStatus::Success) {
            throw std::invalid_argument(result.reason);
        }
        if (p.closed) {
            BRepBuilderAPI_MakeWire wire;
            for (const auto& piece : chain) {
                wire.Add(TopoDS::Edge(piece.geometry->toShape()));
            }
            const gp_Dir normal(p.normal[0], p.normal[1], p.normal[2]);
            BRepBuilderAPI_MakeFace face(gp_Pln(gp_Pnt(0, 0, 0), normal), wire.Wire(), false);
            BRepTopAdaptor_FClass2d classifier(face.Face(), Precision::Confusion());
            const auto state = classifier.PerformInfinitePoint();
            if ((state != TopAbs_IN && state != TopAbs_OUT)
                || (state == TopAbs_OUT ? 1 : -1) != p.orientation) {
                throw std::invalid_argument("TopologyChanged: source orientation changed.");
            }
        }
        for (auto& piece : result.pieces) {
            generated.push_back(std::move(piece.geometry));
        }
    }
    if (generated.size() != item.outputs.size()) {
        throw std::invalid_argument("TopologyChanged: output cardinality changed.");
    }
    for (size_t i = 0; i < generated.size(); ++i) {
        auto* target = geometry[item.outputs[i]];
        if (target->getTypeId() != generated[i]->getTypeId()
            || GeometryFacade::isInternalAligned(target)) {
            throw std::invalid_argument("TypeChanged: output type or helper mapping changed.");
        }
        if (relation.outputs[i].reversed) {
            generated[i]->reverse();
        }
        // Change the native handle on the OLD clone. Its tag, Sketcher ID and all
        // extensions survive, even when the fitted spline has a new pole count.
        if (auto* spline = dynamic_cast<Part::GeomBSplineCurve*>(target)) {
            spline->setHandle(Handle(Geom_BSplineCurve)::DownCast(generated[i]->handle()));
        }
        else if (auto* line = dynamic_cast<Part::GeomLineSegment*>(target)) {
            line->setHandle(Handle(Geom_TrimmedCurve)::DownCast(generated[i]->handle()));
        }
        else {
            throw std::invalid_argument("Unsupported output handle; Detach the offset.");
        }
        relation.outputs[i] = member(target, relation.outputs[i].reversed);
    }
    for (size_t i = 0; i < item.sources.size(); ++i) {
        relation.sources[i] = member(geometry[item.sources[i]], relation.sources[i].reversed);
    }
    auto owner = ensure(geometry[item.outputs.front()]);
    owner->record = updateRecord(owner->record, relation);
}

void validateFinal(const SketchObject& sketch, const std::vector<Part::Geometry*>& geometry,
                   std::vector<Constraint*>& constraints)
{
    Sketch validation;
    if (validation.setUpSketch(geometry, constraints, sketch.getExternalGeometryCount()) < 0
        || validation.hasConflicts() || validation.hasRedundancies()
        || validation.hasMalformedConstraints() || validation.solve() != GCS::SolveStatus::Success
        || validation.solve() != GCS::SolveStatus::Success) {
        throw std::invalid_argument("Final solver state is invalid; solve the sketch first.");
    }
    validation.toShape();
    const auto solved = validation.extractGeometry(true, true);
    std::vector<std::unique_ptr<Part::Geometry>> owned;
    for (auto* curve : solved) {
        owned.emplace_back(curve);
    }
    if (solved.size() != geometry.size()) {
        throw std::invalid_argument("Final solver geometry count changed.");
    }
    for (size_t i = 0; i < geometry.size(); ++i) {
        if (signature(*geometry[i]) != signature(*solved[i])) {
            throw std::invalid_argument("Solving would change geometry; solve the sketch first.");
        }
    }
}
}  // namespace

void ownLinkedOffsetBlocks(const std::vector<Part::Geometry*>& geometry,
                          const std::vector<Constraint*>& blocks)
{
    if (blocks.empty()) {
        throw std::invalid_argument("Missing owned Blocks.");
    }
    auto owner = ensure(geometry[blocks.front()->getGeoId(0)]);
    auto record = Json::parse(owner->record);
    record["ownedBlocks"] = Json::array();
    for (auto* block : blocks) {
        const auto token = LinkedOffsetExtension().uuid;
        block->MetaData = Json {{"linkedOffsetBlock", token}}.dump();
        ensure(geometry[block->getGeoId(0)])->blockToken = token;
        record["ownedBlocks"].push_back(token);
    }
    owner->record = record.dump();
}

void operateLinkedOffsets(SketchObject& sketch, const std::vector<std::string>& ids,
                         LinkedOffsetOperation operation, double distance, double tolerance)
{
    auto* document = sketch.getDocument();
    if (!document || document->hasPendingTransaction() || document->transacting()) {
        throw std::invalid_argument("Finish the current document transaction first.");
    }
    const auto before = inputSnapshot(sketch);
    auto relations = listLinkedOffsets(sketch);
    std::set<std::string> requested(ids.begin(), ids.end());
    if (requested.size() != ids.size()) {
        throw std::invalid_argument("Repeated relation selection.");
    }
    std::vector<LinkedOffsetInspection> selected;
    for (auto& item : relations) {
        if (ids.empty() || requested.erase(item.relation.id)) {
            selected.push_back(std::move(item));
        }
    }
    if (!requested.empty() || selected.empty()) {
        throw std::invalid_argument("Select an existing linked offset relation.");
    }
    std::vector<std::unique_ptr<Part::Geometry>> ownedGeometry;
    std::vector<Part::Geometry*> geometry;
    for (const auto* curve : sketch.getCompleteGeometry()) {
        ownedGeometry.emplace_back(curve->clone());
        geometry.push_back(ownedGeometry.back().get());
    }
    std::vector<std::unique_ptr<Constraint>> ownedConstraints;
    std::vector<Constraint*> constraints;
    for (const auto* constraint : sketch.Constraints.getValues()) {
        ownedConstraints.emplace_back(constraint->clone());
        constraints.push_back(ownedConstraints.back().get());
    }
    for (auto& item : selected) {
        if (operation != LinkedOffsetOperation::Detach) {
            if (item.status != LinkedOffsetStatus::Ok) {
                throw std::invalid_argument(item.reason);
            }
            const auto reason = outputReason(sketch, item);
            if (!reason.empty()) {
                throw std::invalid_argument(reason);
            }
            if (operation == LinkedOffsetOperation::Edit) {
                item.relation.parameters.distance = distance;
                item.relation.parameters.tolerance = tolerance;
            }
            regenerate(geometry, item);
        }
        else {
            // Detach also works with missing sources/outputs. Use membership, never
            // guessed indices. Only tokens proven by a readable record own Blocks.
            Json tokens = Json::array();
            for (auto* curve : geometry) {
                if (const auto ext = extension(curve); ext && ext->version == "1"
                    && ext->membership == item.relation.id && !ext->record.empty()) {
                    auto record = Json::parse(ext->record, nullptr, false);
                    if (record.is_object() && record.contains("ownedBlocks")
                        && record["ownedBlocks"].is_array()) {
                        tokens = record["ownedBlocks"];
                    }
                }
            }
            for (size_t i = 0; i < size_t(sketch.Geometry.getSize()); ++i) {
                const auto ext = extension(geometry[i]);
                if (!ext || ext->version != "1" || ext->membership != item.relation.id) {
                    continue;
                }
                constraints.erase(std::remove_if(constraints.begin(), constraints.end(),
                    [&](const auto* c) {
                        const auto token = blockToken(*c);
                        return c->Type == Block && c->getGeoId(0) == int(i) && !token.empty()
                            && (token == ext->blockToken
                                || std::find(tokens.begin(), tokens.end(), token) != tokens.end());
                    }), constraints.end());
                auto detached = ensure(geometry[i]);
                detached->membership.clear();
                detached->blockToken.clear();
                // Keep the entire opaque payload; inspection ignores detached records.
                GeometryFacade::getFacade(geometry[i])->setBlocked(
                    std::any_of(constraints.begin(), constraints.end(), [&](const auto* c) {
                        return c->Type == Block && c->getGeoId(0) == int(i) && c->isActive;
                    }));
            }
        }
    }
    validateFinal(sketch, geometry, constraints);
    geometry.resize(sketch.Geometry.getSize());
    if (before != inputSnapshot(sketch)) {
        throw std::invalid_argument("Sketch inputs changed while staging; retry explicit refresh.");
    }
    // No ordinary refusal below this point. Allocation/notification failures can
    // abort installation, but App::Document cannot restore redo after a mutation.
    if (!document->openTransaction(std::string("Explicit linked offset operation"))) {
        throw std::runtime_error("Unable to open the linked offset transaction.");
    }
    try {
        // lvalue overloads clone: local ownership remains safe even if a notifier throws.
        sketch.Geometry.setValues(geometry);
        sketch.Constraints.setValues(constraints);
        document->commitTransaction();
    }
    catch (...) {
        document->abortTransaction();
        throw;
    }
}
}  // namespace Sketcher
