// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Part/PartGlobal.h>

#include <atomic>
#include <string>

#include <Base/Vector3D.h>
#include <Precision.hxx>
#include <TopoDS_Shape.hxx>

namespace Part
{

enum class InterferenceKind
{
    Clear,
    ClearanceViolation,
    Contact,
    Penetration,
    InvalidInput,
    Inconclusive,
    Cancelled
};

struct InterferenceOptions
{
    /** Nonnegative minimum clearance. Zero still reports contact and penetration. */
    double clearance = 0.0;
    /** Internal linear tolerance; defaults to Precision::Confusion(). */
    double linearTolerance = Precision::Confusion();
    /** Optional cooperative cancellation flag checked between OCCT stages. */
    const std::atomic<bool>* cancelFlag = nullptr;
    /**
     * When true, skip BRepCheck_Analyzer (caller already validated shapes once,
     * e.g. assembly leaf collection).
     */
    bool skipGeometryValidation = false;
};

struct InterferenceResult
{
    InterferenceKind kind = InterferenceKind::Inconclusive;
    double minimumDistance = -1.0;
    Base::Vector3d pointOnFirst;
    Base::Vector3d pointOnSecond;
    TopoDS_Shape commonShape;
    double overlapVolume = 0.0;
    std::string diagnostic;
};

/**
 * Classify the geometric relationship between two shapes.
 *
 * Uses BRepExtrema_DistShapeShape, then an exact Common (without size-scaled
 * auto-fuzzy) when the distance is within kernel tolerance.
 */
PartExport InterferenceResult classifyInterference(
    const TopoDS_Shape& first,
    const TopoDS_Shape& second,
    const InterferenceOptions& options = {}
);

}  // namespace Part
