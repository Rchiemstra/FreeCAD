// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionMeasurement.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace Inspection::Photo
{
namespace
{

constexpr double pi = 3.141592653589793238462643383279502884;

Diagnostic error(const DiagnosticCode code, std::string message)
{
    return {code, DiagnosticSeverity::Error, std::move(message)};
}

bool finite(const Vector2d& point)
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool solve3x3(
    std::array<std::array<double, 3>, 3> matrix,
    std::array<double, 3> right,
    std::array<double, 3>& solution
)
{
    for (std::size_t pivot = 0; pivot < 3; ++pivot) {
        std::size_t best = pivot;
        for (std::size_t row = pivot + 1; row < 3; ++row) {
            if (std::abs(matrix[row][pivot]) > std::abs(matrix[best][pivot])) {
                best = row;
            }
        }
        if (!std::isfinite(matrix[best][pivot]) || std::abs(matrix[best][pivot]) <= 1.0e-14) {
            return false;
        }
        std::swap(matrix[pivot], matrix[best]);
        std::swap(right[pivot], right[best]);
        for (std::size_t row = pivot + 1; row < 3; ++row) {
            const double factor = matrix[row][pivot] / matrix[pivot][pivot];
            for (std::size_t column = pivot; column < 3; ++column) {
                matrix[row][column] -= factor * matrix[pivot][column];
            }
            right[row] -= factor * right[pivot];
        }
    }
    for (int row = 2; row >= 0; --row) {
        double value = right[static_cast<std::size_t>(row)];
        for (std::size_t column = static_cast<std::size_t>(row) + 1; column < 3; ++column) {
            value -= matrix[static_cast<std::size_t>(row)][column] * solution[column];
        }
        solution[static_cast<std::size_t>(row)] = value
            / matrix[static_cast<std::size_t>(row)][static_cast<std::size_t>(row)];
    }
    return std::isfinite(solution[0]) && std::isfinite(solution[1]) && std::isfinite(solution[2]);
}

}  // namespace

ValidationResult combineUncertaintyBudget(UncertaintyBudget& budget)
{
    if (!std::isfinite(budget.coverageFactor) || budget.coverageFactor <= 0.0
        || budget.components.empty() || budget.components.size() > 128) {
        return ValidationResult::failure(
            DiagnosticCode::InvalidSchema,
            "uncertainty budget or coverage factor is invalid"
        );
    }
    std::map<std::string, double> correlated;
    double independentSquares = 0.0;
    for (const UncertaintyComponent& component : budget.components) {
        if (component.name.empty() || component.name.size() > 64
            || component.correlationGroup.size() > 64
            || !std::isfinite(component.standardUncertaintyMm)
            || component.standardUncertaintyMm < 0.0) {
            return ValidationResult::failure(
                DiagnosticCode::InvalidSchema,
                "uncertainty component is invalid"
            );
        }
        if (component.correlationGroup.empty()) {
            independentSquares += component.standardUncertaintyMm * component.standardUncertaintyMm;
        }
        else {
            correlated[component.correlationGroup] += component.standardUncertaintyMm;
        }
    }
    for (const auto& [group, absoluteSum] : correlated) {
        (void)group;
        independentSquares += absoluteSum * absoluteSum;
    }
    budget.combinedStandardUncertaintyMm = std::sqrt(independentSquares);
    budget.expandedUncertaintyMm = budget.combinedStandardUncertaintyMm * budget.coverageFactor;
    if (!std::isfinite(budget.expandedUncertaintyMm)) {
        return ValidationResult::failure(
            DiagnosticCode::NumericalFailure,
            "uncertainty combination overflowed"
        );
    }
    return ValidationResult::success();
}

CircleMeasurement fitCircleFeature(
    const std::vector<Vector2d>& points,
    const double minimumAngularCoverageRadians
)
{
    CircleMeasurement result;
    if (points.size() < 3 || points.size() > maximumPointCount
        || !std::isfinite(minimumAngularCoverageRadians) || minimumAngularCoverageRadians <= 0.0
        || minimumAngularCoverageRadians > 2.0 * pi) {
        result.diagnostic = error(DiagnosticCode::InvalidSchema, "circle-fit input is invalid");
        return result;
    }

    std::array<std::array<double, 3>, 3> normal {};
    std::array<double, 3> right {};
    for (const Vector2d& point : points) {
        if (!finite(point)) {
            result.diagnostic = error(DiagnosticCode::NonFiniteValue, "circle-fit point is non-finite");
            return result;
        }
        const std::array<double, 3> row {point.x, point.y, 1.0};
        const double target = -(point.x * point.x + point.y * point.y);
        for (std::size_t r = 0; r < 3; ++r) {
            right[r] += row[r] * target;
            for (std::size_t c = 0; c < 3; ++c) {
                normal[r][c] += row[r] * row[c];
            }
        }
    }
    std::array<double, 3> parameters {};
    if (!solve3x3(normal, right, parameters)) {
        result.status = OperationStatus::Inconclusive;
        result.diagnostic = error(DiagnosticCode::NumericalFailure, "circle points are rank deficient");
        return result;
    }
    result.center = {-parameters[0] / 2.0, -parameters[1] / 2.0};
    const double squaredRadius = result.center.x * result.center.x
        + result.center.y * result.center.y - parameters[2];
    if (!std::isfinite(squaredRadius) || squaredRadius <= 0.0) {
        result.status = OperationStatus::Inconclusive;
        result.diagnostic = error(DiagnosticCode::NumericalFailure, "circle radius is not positive");
        return result;
    }
    result.radiusMm = std::sqrt(squaredRadius);
    result.diameterMm = result.radiusMm * 2.0;

    std::vector<double> angles;
    angles.reserve(points.size());
    double residualSquares = 0.0;
    for (const Vector2d& point : points) {
        const double distance = std::hypot(point.x - result.center.x, point.y - result.center.y);
        const double residual = distance - result.radiusMm;
        residualSquares += residual * residual;
        double angle = std::atan2(point.y - result.center.y, point.x - result.center.x);
        if (angle < 0.0) {
            angle += 2.0 * pi;
        }
        angles.push_back(angle);
    }
    result.rmsResidualMm = std::sqrt(residualSquares / static_cast<double>(points.size()));
    std::sort(angles.begin(), angles.end());
    double maximumGap = angles.front() + 2.0 * pi - angles.back();
    for (std::size_t index = 1; index < angles.size(); ++index) {
        maximumGap = std::max(maximumGap, angles[index] - angles[index - 1]);
    }
    result.angularCoverageRadians = 2.0 * pi - maximumGap;
    if (result.angularCoverageRadians < minimumAngularCoverageRadians) {
        result.status = OperationStatus::Inconclusive;
        result.diagnostic = error(
            DiagnosticCode::UnsupportedGeometry,
            "partial arc cannot be classified as a hole"
        );
        return result;
    }
    result.status = OperationStatus::Complete;
    return result;
}

ExtentMeasurement measureExtents(const std::vector<PolylineCycle>& cycles)
{
    ExtentMeasurement result;
    if (cycles.empty() || cycles.size() > maximumCycleCount) {
        result.diagnostic = error(DiagnosticCode::InvalidGeometry, "no contours to measure");
        return result;
    }
    result.minimumXmm = std::numeric_limits<double>::infinity();
    result.minimumYmm = std::numeric_limits<double>::infinity();
    result.maximumXmm = -std::numeric_limits<double>::infinity();
    result.maximumYmm = -std::numeric_limits<double>::infinity();
    std::size_t pointCount = 0;
    bool haveOuterPoint = false;
    for (const PolylineCycle& cycle : cycles) {
        pointCount += cycle.points.size();
        if (pointCount > maximumPointCount) {
            result.status = OperationStatus::ResourceLimit;
            result.diagnostic = error(DiagnosticCode::ResourceLimit, "contours exceed the point limit");
            return result;
        }
        if (cycle.hole) {
            continue;
        }
        for (const Vector2d& point : cycle.points) {
            if (!finite(point)) {
                result.diagnostic
                    = error(DiagnosticCode::NonFiniteValue, "contour point is non-finite");
                return result;
            }
            haveOuterPoint = true;
            result.minimumXmm = std::min(result.minimumXmm, point.x);
            result.maximumXmm = std::max(result.maximumXmm, point.x);
            result.minimumYmm = std::min(result.minimumYmm, point.y);
            result.maximumYmm = std::max(result.maximumYmm, point.y);
        }
    }
    if (!haveOuterPoint) {
        result.diagnostic = error(DiagnosticCode::InvalidGeometry, "no outer contour points");
        return result;
    }
    result.widthMm = result.maximumXmm - result.minimumXmm;
    result.heightMm = result.maximumYmm - result.minimumYmm;
    result.status = OperationStatus::Complete;
    return result;
}

}  // namespace Inspection::Photo
