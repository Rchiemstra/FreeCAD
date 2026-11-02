// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <memory>
#include <string>

#include <Base/Vector3D.h>
#include <Mod/Part/App/Geometry.h>
#include <Mod/Sketcher/SketcherGlobal.h>

namespace Sketcher
{

enum class BSplineOffsetStatus
{
    Success,
    InvalidOptions,
    UnsupportedCurve,
    ComplexityExceeded,
    ZeroTangent,
    SingularOffset,
    SelfIntersection,
    VerificationFailed,
    ApproximationFailed,
    SeamCorner,
    SourceSelfIntersection
};

struct BSplineOffsetOptions
{
    // Internal length units (mm). Positive follows tangent cross sketchNormal,
    // as in Geom_OffsetCurve. Reversing either orientation reverses the side.
    double distance = 1.0;
    double tolerance = 0.01;
    Base::Vector3d sketchNormal {0.0, 0.0, 1.0};
    int maxInputPoles = 128;
    int maxOutputPoles = 256;
    int maxSegments = 64;
    int maxDegree = 8;
    // Bounds adaptive validation as well as fitting. Exhaustion is failure.
    int maxValidationSpans = 4096;
    int maxRefinementDepth = 20;
};

struct BSplineOffsetComplexity
{
    int inputPoles = 0;
    int outputPoles = 0;
    int segments = 0;
    int degree = 0;
    int validationSpans = 0;
};

struct BSplineOffsetResult
{
    BSplineOffsetStatus status = BSplineOffsetStatus::ApproximationFailed;
    std::string message;
    // Set only on success; owned detached geometry, with no document/constraints.
    std::unique_ptr<Part::GeomBSplineCurve> curve;
    // Conservative sum of kernel error and independent correspondence bound.
    double achievedMaxError = 0.0;
    BSplineOffsetComplexity complexity;
    // Signed orientation about sketchNormal for closed curves; zero for open.
    int orientation = 0;
};

// Constant-time option and size checks, before copying or extracting poles.
SketcherExport BSplineOffsetStatus checkBSplineOffsetInput(
    const Part::GeomBSplineCurve& source,
    const BSplineOffsetOptions& options = {}
);

// Accepts one planar spline with C2 interior, including positive rational
// weights. Closed and periodic splines must be simple with a G1 or better seam.
// Never modifies source. No partial geometry is returned on failure.
// Tolerance must be >= 1e-7 mm and < |distance| / 2. Limits are finite policies,
// not a wall-clock timeout for OCCT. No GUI, solver or document side effects.
SketcherExport BSplineOffsetResult
makeBSplineOffset(const Part::GeomBSplineCurve& source, const BSplineOffsetOptions& options = {});

}  // namespace Sketcher
