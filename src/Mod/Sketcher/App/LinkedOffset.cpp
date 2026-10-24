// SPDX-License-Identifier: LGPL-2.1-or-later

#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <Base/Writer.h>
#include "LinkedOffset.h"
#include "SketchObject.h"

namespace Sketcher
{
using Json = nlohmann::json;
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    LinkedOffsetMember, uuid, type, reversed, seam, signature
)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
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
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    LinkedOffsetRelation, id, sources, outputs, parameters
)

namespace
{
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
    writer.Stream() << std::setprecision(std::numeric_limits<double>::max_digits10);
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
    std::set<int> indices;
    const auto check = [&](int index) {
        if (index < 0 || size_t(index) >= geometry.size() || !indices.insert(index).second) {
            throw std::invalid_argument("Invalid or repeated offset member.");
        }
        if (const auto ext = extension(geometry[index]); ext && ext->version != 1) {
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
            if (ext->version != 1 && !ext->record.empty()) {
                ignored.insert(ext->membership);
            }
            if (ext->version == 1 && !ext->membership.empty()) {
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
                    inspection.relation = Json::parse(ext->record).get<LinkedOffsetRelation>();
                }
                catch (const Json::exception&) {
                    fail(LinkedOffsetStatus::Ambiguous, "Invalid relation record.");
                }
            }
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
