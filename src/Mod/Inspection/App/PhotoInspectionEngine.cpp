// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Inspection::Photo
{
namespace
{

bool finite(const double value)
{
    return std::isfinite(value);
}

bool finite(const Vector2d& point)
{
    return finite(point.x) && finite(point.y);
}

double squaredDistance(const Vector2d& lhs, const Vector2d& rhs)
{
    const double x = lhs.x - rhs.x;
    const double y = lhs.y - rhs.y;
    return x * x + y * y;
}

Vector2d nearestPointOnSegment(const Vector2d& point, const Vector2d& start, const Vector2d& end)
{
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    const double squaredLength = dx * dx + dy * dy;
    if (squaredLength <= std::numeric_limits<double>::epsilon()) {
        return start;
    }
    const double parameter
        = std::clamp(((point.x - start.x) * dx + (point.y - start.y) * dy) / squaredLength, 0.0, 1.0);
    return {start.x + parameter * dx, start.y + parameter * dy};
}

bool pointInsidePolygon(const Vector2d& point, const std::vector<Vector2d>& polygon)
{
    bool inside = false;
    for (std::size_t current = 0, previous = polygon.size() - 1; current < polygon.size();
         previous = current++) {
        const Vector2d& a = polygon[current];
        const Vector2d& b = polygon[previous];
        const bool crosses = ((a.y > point.y) != (b.y > point.y))
            && (point.x < (b.x - a.x) * (point.y - a.y) / ((b.y - a.y) + 0.0) + a.x);
        if (crosses) {
            inside = !inside;
        }
    }
    return inside;
}

Diagnostic diagnostic(const DiagnosticCode code, const DiagnosticSeverity severity, std::string message)
{
    return {code, severity, std::move(message)};
}

}  // namespace

Vector2d RigidTransform2d::apply(const Vector2d& point) const
{
    return {
        cosine * point.x - sine * point.y + translationX,
        sine * point.x + cosine * point.y + translationY,
    };
}

bool RigidTransform2d::isFinite() const
{
    return finite(cosine) && finite(sine) && finite(translationX) && finite(translationY);
}

std::optional<RigidTransform2d> fitRigidTransform(
    const std::vector<Vector2d>& source,
    const std::vector<Vector2d>& target
)
{
    if (source.size() != target.size() || source.size() < 2) {
        return std::nullopt;
    }

    Vector2d sourceCenter;
    Vector2d targetCenter;
    for (std::size_t index = 0; index < source.size(); ++index) {
        if (!finite(source[index]) || !finite(target[index])) {
            return std::nullopt;
        }
        sourceCenter.x += source[index].x;
        sourceCenter.y += source[index].y;
        targetCenter.x += target[index].x;
        targetCenter.y += target[index].y;
    }
    const double inverseCount = 1.0 / static_cast<double>(source.size());
    sourceCenter.x *= inverseCount;
    sourceCenter.y *= inverseCount;
    targetCenter.x *= inverseCount;
    targetCenter.y *= inverseCount;

    double cosineTerm = 0.0;
    double sineTerm = 0.0;
    double sourceSpread = 0.0;
    for (std::size_t index = 0; index < source.size(); ++index) {
        const double sx = source[index].x - sourceCenter.x;
        const double sy = source[index].y - sourceCenter.y;
        const double tx = target[index].x - targetCenter.x;
        const double ty = target[index].y - targetCenter.y;
        cosineTerm += sx * tx + sy * ty;
        sineTerm += sx * ty - sy * tx;
        sourceSpread += sx * sx + sy * sy;
    }
    if (!finite(sourceSpread) || sourceSpread <= std::numeric_limits<double>::epsilon()) {
        return std::nullopt;
    }

    const double magnitude = std::hypot(cosineTerm, sineTerm);
    if (!finite(magnitude) || magnitude <= std::numeric_limits<double>::epsilon()) {
        return std::nullopt;
    }

    RigidTransform2d result;
    result.cosine = cosineTerm / magnitude;
    result.sine = sineTerm / magnitude;
    result.translationX = targetCenter.x
        - (result.cosine * sourceCenter.x - result.sine * sourceCenter.y);
    result.translationY = targetCenter.y
        - (result.sine * sourceCenter.x + result.cosine * sourceCenter.y);
    if (!result.isFinite()) {
        return std::nullopt;
    }
    return result;
}

ValidationResult comparePolylineCycle(
    const PolylineCycle& nominal,
    const PolylineCycle& measured,
    const std::size_t cycleIndex,
    std::vector<DeviationSample>& output
)
{
    if (nominal.points.size() < 3 || measured.points.empty()) {
        return ValidationResult::failure(
            DiagnosticCode::InvalidGeometry,
            "nominal and measured cycles must contain points"
        );
    }
    if (measured.points.size() > maximumPointCount - output.size()) {
        return ValidationResult::failure(
            DiagnosticCode::ResourceLimit,
            "measured contour exceeds the point-count limit"
        );
    }
    for (const Vector2d& point : nominal.points) {
        if (!finite(point)) {
            return ValidationResult::failure(
                DiagnosticCode::NonFiniteValue,
                "nominal contour contains a non-finite point"
            );
        }
    }

    for (std::size_t sampleIndex = 0; sampleIndex < measured.points.size(); ++sampleIndex) {
        const Vector2d& measuredPoint = measured.points[sampleIndex];
        if (!finite(measuredPoint)) {
            return ValidationResult::failure(
                DiagnosticCode::NonFiniteValue,
                "measured contour contains a non-finite point"
            );
        }

        Vector2d nearest;
        double nearestSquared = std::numeric_limits<double>::infinity();
        for (std::size_t edge = 0; edge < nominal.points.size(); ++edge) {
            const Vector2d candidate = nearestPointOnSegment(
                measuredPoint,
                nominal.points[edge],
                nominal.points[(edge + 1) % nominal.points.size()]
            );
            const double candidateSquared = squaredDistance(measuredPoint, candidate);
            if (candidateSquared < nearestSquared) {
                nearestSquared = candidateSquared;
                nearest = candidate;
            }
        }
        if (!finite(nearestSquared)) {
            return ValidationResult::failure(
                DiagnosticCode::NumericalFailure,
                "nearest-contour calculation failed"
            );
        }

        const bool inside = pointInsidePolygon(measuredPoint, nominal.points);
        const bool excessMaterial = nominal.hole ? inside : !inside;
        double signedDeviation = std::sqrt(nearestSquared);
        if (!excessMaterial) {
            signedDeviation = -signedDeviation;
        }
        output.push_back({cycleIndex, sampleIndex, measuredPoint, nearest, signedDeviation});
    }
    return ValidationResult::success();
}

ConformanceDecision evaluateMeasurement(Measurement& measurement)
{
    if (!finite(measurement.nominalMm) || !finite(measurement.actualMm)
        || !finite(measurement.lowerToleranceMm) || !finite(measurement.upperToleranceMm)
        || !finite(measurement.expandedUncertaintyMm)
        || measurement.lowerToleranceMm > measurement.upperToleranceMm
        || measurement.expandedUncertaintyMm < 0.0) {
        measurement.decision = ConformanceDecision::Inconclusive;
        return measurement.decision;
    }

    const double deviation = measurement.actualMm - measurement.nominalMm;
    const double low = deviation - measurement.expandedUncertaintyMm;
    const double high = deviation + measurement.expandedUncertaintyMm;
    if (low >= measurement.lowerToleranceMm && high <= measurement.upperToleranceMm) {
        measurement.decision = ConformanceDecision::Pass;
    }
    else if (high < measurement.lowerToleranceMm || low > measurement.upperToleranceMm) {
        measurement.decision = ConformanceDecision::Fail;
    }
    else {
        measurement.decision = ConformanceDecision::Inconclusive;
    }
    return measurement.decision;
}

AnalysisResult analyzePhotoInspection(
    const AnalysisInput& input,
    CancellationCallback cancelled,
    ProgressCallback progress
)
{
    AnalysisResult result;
    result.generation = input.generation;
    result.projectionGeometrySha256 = input.nominalProjection.sha256;

    if (input.nominalProjection.sha256.size() != 64 || input.nominalProjection.snapshot.cycles.empty()
        || input.measuredCycles.size() != input.nominalProjection.snapshot.cycles.size()
        || !finite(input.options.lowerToleranceMm) || !finite(input.options.upperToleranceMm)
        || !finite(input.options.expandedUncertaintyMm)
        || input.options.lowerToleranceMm > input.options.upperToleranceMm
        || input.options.expandedUncertaintyMm < 0.0) {
        result.status = OperationStatus::InvalidInput;
        result.diagnostics.push_back(diagnostic(
            DiagnosticCode::InvalidSchema,
            DiagnosticSeverity::Error,
            "analysis input, identity, tolerance, or cycle count is invalid"
        ));
        return result;
    }

    const std::size_t cycleCount = input.measuredCycles.size();
    for (std::size_t cycleIndex = 0; cycleIndex < cycleCount; ++cycleIndex) {
        if (cancelled && cancelled()) {
            result.status = OperationStatus::Cancelled;
            result.decision = ConformanceDecision::NotEvaluated;
            result.deviations.clear();
            result.measurements.clear();
            result.diagnostics.push_back(diagnostic(
                DiagnosticCode::Cancelled,
                DiagnosticSeverity::Information,
                "analysis was cancelled"
            ));
            return result;
        }
        if (progress) {
            progress(input.generation, "compare-contours", cycleIndex, cycleCount);
        }
        const ValidationResult comparison = comparePolylineCycle(
            input.nominalProjection.snapshot.cycles[cycleIndex],
            input.measuredCycles[cycleIndex],
            cycleIndex,
            result.deviations
        );
        if (!comparison.valid) {
            result.status = comparison.diagnostic.code == DiagnosticCode::ResourceLimit
                ? OperationStatus::ResourceLimit
                : OperationStatus::InvalidInput;
            result.diagnostics.push_back(comparison.diagnostic);
            return result;
        }
    }
    if (progress) {
        progress(input.generation, "compare-contours", cycleCount, cycleCount);
    }

    result.measurements.reserve(result.deviations.size());
    bool anyFail = false;
    bool anyInconclusive = false;
    for (const DeviationSample& sample : result.deviations) {
        Measurement measurement;
        measurement.id = "cycle-" + std::to_string(sample.cycleIndex) + "-sample-"
            + std::to_string(sample.sampleIndex);
        measurement.nominalMm = 0.0;
        measurement.actualMm = sample.signedDeviationMm;
        measurement.lowerToleranceMm = input.options.lowerToleranceMm;
        measurement.upperToleranceMm = input.options.upperToleranceMm;
        measurement.expandedUncertaintyMm = input.options.expandedUncertaintyMm;
        const ConformanceDecision decision = evaluateMeasurement(measurement);
        anyFail = anyFail || decision == ConformanceDecision::Fail;
        anyInconclusive = anyInconclusive || decision == ConformanceDecision::Inconclusive;
        result.measurements.push_back(std::move(measurement));
    }

    result.status = OperationStatus::Complete;
    if (!input.options.cameraProfileDecisionCapable || !input.options.printerProfileDecisionCapable) {
        result.decision = ConformanceDecision::Inconclusive;
        result.diagnostics.push_back(diagnostic(
            DiagnosticCode::MissingCalibration,
            DiagnosticSeverity::Warning,
            "validated camera and printer evidence is required for a conformance decision"
        ));
    }
    else if (anyFail) {
        result.decision = ConformanceDecision::Fail;
    }
    else if (anyInconclusive || result.measurements.empty()) {
        result.decision = ConformanceDecision::Inconclusive;
    }
    else {
        result.decision = ConformanceDecision::Pass;
    }
    return result;
}

}  // namespace Inspection::Photo
