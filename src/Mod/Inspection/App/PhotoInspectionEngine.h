// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <Mod/Inspection/InspectionGlobal.h>

#include "PhotoInspectionTypes.h"

namespace Inspection::Photo
{

struct InspectionExport RigidTransform2d
{
    double cosine {1.0};
    double sine {0.0};
    double translationX {0.0};
    double translationY {0.0};

    Vector2d apply(const Vector2d& point) const;
    bool isFinite() const;
};

struct InspectionExport DeviationSample
{
    std::size_t cycleIndex {0};
    std::size_t sampleIndex {0};
    Vector2d measuredPoint;
    Vector2d nearestNominalPoint;
    double signedDeviationMm {0.0};
};

struct InspectionExport Measurement
{
    std::string id;
    double nominalMm {0.0};
    double actualMm {0.0};
    double lowerToleranceMm {0.0};
    double upperToleranceMm {0.0};
    double expandedUncertaintyMm {0.0};
    ConformanceDecision decision {ConformanceDecision::NotEvaluated};
};

struct InspectionExport ComparisonOptions
{
    double lowerToleranceMm {-0.5};
    double upperToleranceMm {0.5};
    double expandedUncertaintyMm {0.25};
    bool cameraProfileDecisionCapable {false};
    bool printerProfileDecisionCapable {false};
};

struct InspectionExport AnalysisInput
{
    std::uint64_t generation {0};
    CanonicalProjection nominalProjection;
    std::vector<PolylineCycle> measuredCycles;
    ComparisonOptions options;
};

struct InspectionExport AnalysisResult
{
    std::uint64_t generation {0};
    OperationStatus status {OperationStatus::InvalidInput};
    ConformanceDecision decision {ConformanceDecision::NotEvaluated};
    std::string projectionGeometrySha256;
    std::vector<Diagnostic> diagnostics;
    std::vector<DeviationSample> deviations;
    std::vector<Measurement> measurements;
};

using CancellationCallback = std::function<bool()>;
using ProgressCallback
    = std::function<void(std::uint64_t, const std::string&, std::size_t, std::size_t)>;

InspectionExport std::optional<RigidTransform2d> fitRigidTransform(
    const std::vector<Vector2d>& source,
    const std::vector<Vector2d>& target
);

InspectionExport ValidationResult comparePolylineCycle(
    const PolylineCycle& nominal,
    const PolylineCycle& measured,
    std::size_t cycleIndex,
    std::vector<DeviationSample>& output
);

InspectionExport ConformanceDecision evaluateMeasurement(Measurement& measurement);

InspectionExport AnalysisResult analyzePhotoInspection(
    const AnalysisInput& input,
    CancellationCallback cancelled = {},
    ProgressCallback progress = {}
);

}  // namespace Inspection::Photo
