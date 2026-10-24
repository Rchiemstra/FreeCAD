// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <Base/Vector3D.h>
#include <Mod/Part/App/Geometry.h>
#include <Mod/Sketcher/SketcherGlobal.h>

namespace Sketcher
{

enum class OffsetChainDistanceStatus
{
    Success,
    InvalidInput,
    UnsupportedCurve,
    NotCertified,
    BudgetExceeded
};

struct OffsetChainDistanceOptions
{
    // Positive follows source tangent cross sketchNormal, as in Geom_OffsetCurve.
    double distance = 1.0;
    double tolerance = 0.01;
    Base::Vector3d sketchNormal {0, 0, 1};
    // Enclosure/refinement visits; the source cache also has a 65536-node cap.
    int maxWork = 10000000;
    int maxDepth = 24;
};

struct OffsetChainDistanceResult
{
    OffsetChainDistanceStatus status = OffsetChainDistanceStatus::NotCertified;
    std::string message;
    int work = 0;
};

// Certifies EVERY candidate point against the ENTIRE source, including endpoints.
// Side means (q-s).(source tangent cross sketchNormal) has the sign of distance
// for every nearest source point s (one-sided tangents at span boundaries).
// Inputs are ordered, oriented, connected chains of line segments, circular arcs
// and positive-weight B-splines, open or closed. Geometry is borrowed, unmodified.
// This is only a distance/side certificate: joins, coverage, regularity and
// simplicity still require the separate topology certificate before generation.
// Policies: <= 256 curves per chain; spline degree <= 14, <= 1024 poles,
// <= 256 knot spans; native arc parameters within [-8, 8].
// Invalid/unsupported inputs, uncertain bounds and exhausted budgets all refuse.
SketcherExport OffsetChainDistanceResult certifyOffsetChainDistance(
    const std::vector<const Part::GeomCurve*>& source,
    const std::vector<const Part::GeomCurve*>& candidate,
    const OffsetChainDistanceOptions& options = {}
);

struct OffsetChainPiece
{
    const Part::GeomCurve* geometry;
    // Caller-owned identity, unique even when geometry pointers differ.
    std::size_t identity;
};

using OffsetChainTopologyStatus = OffsetChainDistanceStatus;
using OffsetChainTopologyResult = OffsetChainDistanceResult;

struct OffsetChainTopologyOptions
{
    bool closed = false;
    double joinTolerance = 1e-7;  // At most Precision::Confusion().
    // Strict minimum separation for spans without a common ordered endpoint.
    double clearance = 0.0;
    int maxWork = 1000000;
    int maxDepth = 24;
};

// Certifies the supplied order and orientation; never reorders or welds input.
// Every piece's full native parameter interval is included. Only declared
// neighbors (including a declared closing seam) may share an endpoint. Each
// span is regular/injective; all span pairs need separation or a joint local
// monotonicity proof. Small endpoint gaps must also be provably non-overlapping.
// Every declared junction requires that local proof, even with disjoint boxes.
// Each piece's certified length must exceed four times joinTolerance so endpoint
// welding cannot collapse a piece; closed pieces must pass the same length test.
// Same curve/complexity policies as the distance certificate above. Refuses
// discontinuous splines, ambiguous bounds, degeneracies and exhausted budgets.
SketcherExport OffsetChainTopologyResult certifyOffsetChainTopology(
    const std::vector<OffsetChainPiece>& chain,
    const OffsetChainTopologyOptions& options = {}
);

using OffsetChainStatus = OffsetChainDistanceStatus;

struct OffsetChainOptions
{
    double tolerance = 0.01;
    Base::Vector3d sketchNormal {0, 0, 1};
    bool closed = false;
    double joinTolerance = 1e-7;  // At most Precision::Confusion().
    // Shared by source/final certificates and spline validation, never per piece.
    int maxWork = 10000000;
    int maxDepth = 24;
};

struct OffsetChainOutputPiece
{
    std::unique_ptr<Part::GeomCurve> geometry;
    std::size_t identity;
};

struct OffsetChainResult
{
    OffsetChainStatus status = OffsetChainStatus::NotCertified;
    std::string reason;
    // Detached, owned geometry, populated only after ALL checks succeed.
    std::vector<OffsetChainOutputPiece> pieces;
    OffsetChainDistanceResult distanceCertificate;
    OffsetChainTopologyResult topologyCertificate;
    int work = 0;
};

// Consumes each full native source interval exactly once, in the supplied order.
// Identities must be unique; no reordering, trimming, extension or omitted pieces.
// Requires a regular, simple G1 chain in the plane normal to sketchNormal.
// Positive distance follows tangent cross sketchNormal (the single-spline rule).
// Builds analytic line/arc offsets and makeBSplineOffset fits. Circular offsets
// use the exact rational conic representation to permit algebraic endpoint welding.
// Joins of detached source copies are welded for the source topology proof.
// The distance certificate always uses the untouched original source. Welds endpoints
// only within joinTolerance, then certifies distance/side against the WHOLE source
// and topology on the final geometry, including every welding displacement.
// Refusals return no pieces. Source geometry is borrowed and never modified.
SketcherExport OffsetChainResult makeOffsetChain(
    const std::vector<OffsetChainPiece>& source,
    double distance,
    const OffsetChainOptions& options = {}
);

}  // namespace Sketcher
