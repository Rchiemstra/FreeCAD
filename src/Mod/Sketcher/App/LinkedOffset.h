// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <utility>
#include <vector>
#include <Mod/Part/App/Geometry.h>
#include "LinkedOffsetExtension.h"

namespace Sketcher
{
class SketchObject;
class Constraint;

struct LinkedOffsetMember
{
    std::string uuid;
    std::string type;
    bool reversed = false;
    double seam = 0;
    std::string signature;
};

struct LinkedOffsetParameters
{
    std::string generator = "BSpline";
    double distance = 1;
    double tolerance = 0.01;
    std::array<double, 3> normal {0, 0, 1};
    bool closed = false;
    int orientation = 0;
    double seam = 0;
    std::string join = "G1";
    double joinTolerance = 1e-7;
    int maxWork = 10000000;
    int maxDepth = 24;
    int maxInputPoles = 128;
    int maxOutputPoles = 256;
    int maxSegments = 64;
    int maxDegree = 8;
    int maxValidationSpans = 4096;
    int maxRefinementDepth = 20;
};

struct LinkedOffsetRelation
{
    std::string id;
    std::vector<LinkedOffsetMember> sources;
    std::vector<LinkedOffsetMember> outputs;
    LinkedOffsetParameters parameters;
};

enum class LinkedOffsetStatus
{
    Ok,
    MissingSource,
    TopologyChanged,
    Ambiguous,
    TypeChanged,
    MissingOutput,
    InvalidRecord
};

struct LinkedOffsetInspection
{
    LinkedOffsetRelation relation;
    LinkedOffsetStatus status = LinkedOffsetStatus::Ok;
    std::string reason;
    std::vector<int> sources;
    std::vector<int> outputs;
};

// Call only on detached geometry, then publish via the Geometry property in the
// caller's single transaction. Indices are used only at attachment, never saved.
SketcherExport void attachLinkedOffset(
    const std::vector<Part::Geometry*>& geometry,
    const std::vector<std::pair<int, bool>>& sources,
    const std::vector<int>& outputs,
    const LinkedOffsetParameters& parameters
);
SketcherExport std::vector<LinkedOffsetInspection> listLinkedOffsets(const SketchObject& sketch);
// Insertion only: pass one detached copy batch, never snapshots or solver clones.
SketcherExport void remapLinkedOffsetCopies(const std::vector<Part::Geometry*>& copies);
// Topology changes cannot inherit source ancestry, even when indices/types survive.
SketcherExport void invalidateLinkedOffsetAncestry(Part::Geometry& geometry);
SketcherExport void invalidateLinkedOffsetAncestry(SketchObject& sketch, int index);
SketcherExport const char* linkedOffsetStatusName(LinkedOffsetStatus status);
enum class LinkedOffsetOperation { Refresh, Edit, Detach };
// Empty ids selects all relations. Stages the entire batch before one transaction.
SketcherExport void operateLinkedOffsets(
    SketchObject& sketch, const std::vector<std::string>& ids, LinkedOffsetOperation operation,
    double distance = 1, double tolerance = 0.01
);
// Only freshly created detached Blocks may be claimed as relation-owned.
SketcherExport void ownLinkedOffsetBlocks(
    const std::vector<Part::Geometry*>& geometry, const std::vector<Constraint*>& blocks
);
// Detached JSON snapshot shared by the read-only Python adapter.
SketcherExport std::string linkedOffsetsJson(const SketchObject& sketch);
void registerLinkedOffsetPython(PyObject* module);

}  // namespace Sketcher
