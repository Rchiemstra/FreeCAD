// SPDX-License-Identifier: LGPL-2.1-or-later

#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <Precision.hxx>
#include <nlohmann/json.hpp>
#include <Base/Writer.h>
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

LinkedOffsetMember member(Part::Geometry* geometry, bool reversed)
{
    Base::StringWriter writer;
    // Match the precision used by Base::ZipWriter for document persistence.
    writer.Stream() << std::setprecision(std::numeric_limits<double>::digits10 + 1);
    // Full native representation, without extensions/UUIDs, for later stale checks.
    std::unique_ptr<Part::Geometry> bare(geometry->clone());
    for (const auto& weak : bare->getExtensions()) {
        if (const auto ext = weak.lock()) {
            bare->deleteExtension(ext->getTypeId());
        }
    }
    bare->Save(writer);
    const auto* curve = dynamic_cast<const Part::GeomCurve*>(geometry);
    const double seam = curve ? (reversed ? curve->getLastParameter() : curve->getFirstParameter())
                              : 0;
    return {
        ensure(geometry)->uuid,
        std::string(geometry->getTypeId().getName()),
        reversed,
        seam,
        writer.getString()
    };
}
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
            if (ext->version == "1") {
                ext->record.clear();
            }
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
            const auto record = Json(relation).dump();
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
        ensure(&geometry)->uuid = LinkedOffsetExtension().uuid;
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
    std::map<std::string, std::vector<std::shared_ptr<const LinkedOffsetExtension>>> groups;
    for (size_t i = 0; i < geometry.size(); ++i) {
        if (const auto ext = extension(geometry[i])) {
            identities[ext->uuid].push_back(int(i));
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
        entry["sourceIndices"] = inspection.sources;
        entry["outputIndices"] = inspection.outputs;
        result.push_back(std::move(entry));
    }
    return result.dump();
}
}  // namespace Sketcher
