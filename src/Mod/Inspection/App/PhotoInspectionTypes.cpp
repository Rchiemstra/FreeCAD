// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionTypes.h"

#include <QByteArray>
#include <QCryptographicHash>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <type_traits>
#include <tuple>
#include <utility>

namespace Inspection::Photo
{
namespace
{

constexpr std::array<std::uint8_t, 8> projectionMagic {
    'F',
    'C',
    'P',
    'H',
    'O',
    'T',
    'O',
    '\0',
};

bool finite(const double value)
{
    return std::isfinite(value);
}

bool finite(const Vector2d& value)
{
    return finite(value.x) && finite(value.y);
}

bool finite(const Vector3d& value)
{
    return finite(value.x) && finite(value.y) && finite(value.z);
}

Vector3d subtract(const Vector3d& lhs, const Vector3d& rhs)
{
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Vector3d multiply(const Vector3d& value, const double factor)
{
    return {value.x * factor, value.y * factor, value.z * factor};
}

double dot(const Vector3d& lhs, const Vector3d& rhs)
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Vector3d cross(const Vector3d& lhs, const Vector3d& rhs)
{
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

std::optional<Vector3d> normalized(const Vector3d& value)
{
    if (!finite(value)) {
        return std::nullopt;
    }
    const double squared = dot(value, value);
    if (!finite(squared) || squared <= std::numeric_limits<double>::epsilon()) {
        return std::nullopt;
    }
    const double inverseLength = 1.0 / std::sqrt(squared);
    const Vector3d result = multiply(value, inverseLength);
    if (!finite(result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::int64_t> quantize(const double value, const double quantum)
{
    if (!finite(value) || !finite(quantum) || quantum <= 0.0 || std::abs(value) > coordinateLimitMm) {
        return std::nullopt;
    }
    const long double scaled = static_cast<long double>(value) / quantum;
    if (scaled < static_cast<long double>(std::numeric_limits<std::int64_t>::min())
        || scaled > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(std::llround(scaled));
}

template<typename Integer>
void appendLittleEndian(std::vector<std::uint8_t>& output, const Integer value)
{
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned bits {};
    std::memcpy(&bits, &value, sizeof(value));
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output.push_back(static_cast<std::uint8_t>((bits >> (index * 8U)) & 0xffU));
    }
}

void appendString(std::vector<std::uint8_t>& output, const std::string& value)
{
    appendLittleEndian(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

double signedArea(const std::vector<Vector2d>& points)
{
    long double twiceArea = 0.0;
    for (std::size_t index = 0; index < points.size(); ++index) {
        const Vector2d& current = points[index];
        const Vector2d& next = points[(index + 1) % points.size()];
        twiceArea += static_cast<long double>(current.x) * next.y
            - static_cast<long double>(next.x) * current.y;
    }
    return static_cast<double>(twiceArea * 0.5);
}

using QuantizedPoint = std::pair<std::int64_t, std::int64_t>;

bool lexicographicallyLessRotation(
    const std::vector<QuantizedPoint>& points,
    const std::size_t lhsStart,
    const std::size_t rhsStart
)
{
    for (std::size_t offset = 0; offset < points.size(); ++offset) {
        const auto& lhs = points[(lhsStart + offset) % points.size()];
        const auto& rhs = points[(rhsStart + offset) % points.size()];
        if (lhs != rhs) {
            return lhs < rhs;
        }
    }
    return false;
}

struct EncodedCycle
{
    bool hole {false};
    std::vector<QuantizedPoint> points;
    std::vector<std::uint8_t> bytes;
};

ValidationResult encodeCycle(const PolylineCycle& source, const double quantum, EncodedCycle& output)
{
    std::vector<Vector2d> points = source.points;
    if (points.size() > 1 && points.front().x == points.back().x
        && points.front().y == points.back().y) {
        points.pop_back();
    }
    if (points.size() < 3) {
        return ValidationResult::failure(
            DiagnosticCode::InvalidGeometry,
            "a projection cycle requires at least three distinct points"
        );
    }

    for (const Vector2d& point : points) {
        if (!finite(point) || std::abs(point.x) > coordinateLimitMm
            || std::abs(point.y) > coordinateLimitMm) {
            return ValidationResult::failure(
                DiagnosticCode::NonFiniteValue,
                "projection cycle contains a non-finite or out-of-range point"
            );
        }
    }

    const double area = signedArea(points);
    if (!finite(area) || std::abs(area) <= quantum * quantum) {
        return ValidationResult::failure(
            DiagnosticCode::InvalidGeometry,
            "projection cycle has zero or numerically collapsed area"
        );
    }

    const bool shouldBeCounterClockwise = !source.hole;
    if ((area > 0.0) != shouldBeCounterClockwise) {
        std::reverse(points.begin(), points.end());
    }

    output.hole = source.hole;
    output.points.reserve(points.size());
    for (const Vector2d& point : points) {
        const auto x = quantize(point.x, quantum);
        const auto y = quantize(point.y, quantum);
        if (!x || !y) {
            return ValidationResult::failure(
                DiagnosticCode::NonFiniteValue,
                "projection point cannot be represented on the canonical grid"
            );
        }
        output.points.emplace_back(*x, *y);
    }

    for (std::size_t index = 0; index < output.points.size(); ++index) {
        const auto& current = output.points[index];
        const auto& next = output.points[(index + 1) % output.points.size()];
        if (current == next) {
            return ValidationResult::failure(
                DiagnosticCode::InvalidGeometry,
                "projection cycle has a duplicate or tolerance-collapsed edge"
            );
        }
    }

    std::size_t first = 0;
    for (std::size_t index = 1; index < output.points.size(); ++index) {
        if (lexicographicallyLessRotation(output.points, index, first)) {
            first = index;
        }
    }
    std::rotate(output.points.begin(), output.points.begin() + first, output.points.end());

    output.bytes.push_back(output.hole ? 1U : 0U);
    appendLittleEndian(output.bytes, static_cast<std::uint32_t>(output.points.size()));
    for (const auto& [x, y] : output.points) {
        appendLittleEndian(output.bytes, x);
        appendLittleEndian(output.bytes, y);
    }
    return ValidationResult::success();
}

ValidationResult encodeQuantizedVector3(
    std::vector<std::uint8_t>& output,
    const Vector3d& value,
    const double quantum,
    const char* description
)
{
    const auto x = quantize(value.x, quantum);
    const auto y = quantize(value.y, quantum);
    const auto z = quantize(value.z, quantum);
    if (!x || !y || !z) {
        return ValidationResult::failure(
            DiagnosticCode::NonFiniteValue,
            std::string(description) + " is not finite or representable"
        );
    }
    appendLittleEndian(output, *x);
    appendLittleEndian(output, *y);
    appendLittleEndian(output, *z);
    return ValidationResult::success();
}

}  // namespace

ValidationResult ValidationResult::success()
{
    return {true, {}};
}

ValidationResult ValidationResult::failure(const DiagnosticCode code, std::string message)
{
    return {
        false,
        {
            code,
            DiagnosticSeverity::Error,
            std::move(message),
        },
    };
}

std::optional<CanonicalFrame> makeCanonicalFrame(const Vector3d& pointOnPlane, const Vector3d& planeNormal)
{
    if (!finite(pointOnPlane)) {
        return std::nullopt;
    }
    auto normal = normalized(planeNormal);
    if (!normal) {
        return std::nullopt;
    }

    const std::array<double, 3> absolute {
        std::abs(normal->x),
        std::abs(normal->y),
        std::abs(normal->z),
    };
    std::size_t largest = 0;
    if (absolute[1] > absolute[largest]) {
        largest = 1;
    }
    if (absolute[2] > absolute[largest]) {
        largest = 2;
    }
    const std::array<double, 3> components {normal->x, normal->y, normal->z};
    if (components[largest] < 0.0) {
        *normal = multiply(*normal, -1.0);
    }

    const std::array<Vector3d, 3> globalAxes {
        Vector3d {1.0, 0.0, 0.0},
        Vector3d {0.0, 1.0, 0.0},
        Vector3d {0.0, 0.0, 1.0},
    };
    std::size_t leastParallel = 0;
    double leastAlignment = std::abs(dot(globalAxes[0], *normal));
    for (std::size_t index = 1; index < globalAxes.size(); ++index) {
        const double alignment = std::abs(dot(globalAxes[index], *normal));
        if (alignment < leastAlignment) {
            leastAlignment = alignment;
            leastParallel = index;
        }
    }

    const Vector3d projectedAxis = subtract(
        globalAxes[leastParallel],
        multiply(*normal, dot(globalAxes[leastParallel], *normal))
    );
    const auto xAxis = normalized(projectedAxis);
    if (!xAxis) {
        return std::nullopt;
    }
    const auto yAxis = normalized(cross(*normal, *xAxis));
    if (!yAxis) {
        return std::nullopt;
    }

    const Vector3d origin = multiply(*normal, dot(pointOnPlane, *normal));
    if (!finite(origin)) {
        return std::nullopt;
    }
    return CanonicalFrame {origin, *xAxis, *yAxis, *normal, 1};
}

Vector2d projectToFrame(const CanonicalFrame& frame, const Vector3d& worldPoint)
{
    const Vector3d relative = subtract(worldPoint, frame.origin);
    return {dot(relative, frame.xAxis), dot(relative, frame.yAxis)};
}

ValidationResult canonicalizeProjection(const ProjectionSnapshot& input, CanonicalProjection& output)
{
    if (input.cycles.empty()) {
        return ValidationResult::failure(
            DiagnosticCode::InvalidGeometry,
            "projection contains no cycles"
        );
    }
    if (input.cycles.size() > maximumCycleCount) {
        return ValidationResult::failure(
            DiagnosticCode::ResourceLimit,
            "projection exceeds the cycle-count limit"
        );
    }
    if (!finite(input.planarityToleranceMm) || input.planarityToleranceMm <= 0.0
        || !finite(input.coordinateQuantumMm) || input.coordinateQuantumMm <= 0.0) {
        return ValidationResult::failure(
            DiagnosticCode::NonFiniteValue,
            "projection tolerances must be finite and positive"
        );
    }

    std::size_t pointCount = 0;
    for (const PolylineCycle& cycle : input.cycles) {
        if (cycle.points.size() > maximumPointCount - pointCount) {
            return ValidationResult::failure(
                DiagnosticCode::ResourceLimit,
                "projection exceeds the point-count limit"
            );
        }
        pointCount += cycle.points.size();
    }

    if (!finite(input.frame.origin) || !finite(input.frame.xAxis) || !finite(input.frame.yAxis)
        || !finite(input.frame.normal)) {
        return ValidationResult::failure(
            DiagnosticCode::NonFiniteValue,
            "projection frame contains a non-finite component"
        );
    }
    for (const double value : input.sourcePlacement) {
        if (!finite(value)) {
            return ValidationResult::failure(
                DiagnosticCode::NonFiniteValue,
                "source placement contains a non-finite component"
            );
        }
    }

    std::vector<EncodedCycle> cycles;
    cycles.reserve(input.cycles.size());
    for (const PolylineCycle& cycle : input.cycles) {
        EncodedCycle encoded;
        const ValidationResult result = encodeCycle(cycle, input.coordinateQuantumMm, encoded);
        if (!result.valid) {
            return result;
        }
        cycles.push_back(std::move(encoded));
    }
    std::sort(cycles.begin(), cycles.end(), [](const EncodedCycle& lhs, const EncodedCycle& rhs) {
        if (lhs.hole != rhs.hole) {
            return !lhs.hole;
        }
        return lhs.bytes < rhs.bytes;
    });

    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), projectionMagic.begin(), projectionMagic.end());
    appendLittleEndian(bytes, projectionSchemaMajor);
    appendLittleEndian(bytes, projectionSchemaMinor);
    appendString(bytes, "mm");
    appendLittleEndian(bytes, input.frame.algorithmVersion);

    ValidationResult result
        = encodeQuantizedVector3(bytes, input.frame.origin, input.coordinateQuantumMm, "frame origin");
    if (!result.valid) {
        return result;
    }
    constexpr double directionQuantum = 1.0e-12;
    for (const auto& [description, vector] : std::array<std::pair<const char*, Vector3d>, 3> {
             std::pair<const char*, Vector3d> {"frame x-axis", input.frame.xAxis},
             std::pair<const char*, Vector3d> {"frame y-axis", input.frame.yAxis},
             std::pair<const char*, Vector3d> {"frame normal", input.frame.normal},
         }) {
        result = encodeQuantizedVector3(bytes, vector, directionQuantum, description);
        if (!result.valid) {
            return result;
        }
    }

    const auto planarity = quantize(input.planarityToleranceMm, input.coordinateQuantumMm);
    if (!planarity) {
        return ValidationResult::failure(
            DiagnosticCode::NonFiniteValue,
            "planarity tolerance is not representable"
        );
    }
    appendLittleEndian(bytes, *planarity);
    for (const double value : input.sourcePlacement) {
        const auto quantized = quantize(value, directionQuantum);
        if (!quantized) {
            return ValidationResult::failure(
                DiagnosticCode::NonFiniteValue,
                "source placement is not representable"
            );
        }
        appendLittleEndian(bytes, *quantized);
    }

    appendLittleEndian(bytes, static_cast<std::uint32_t>(cycles.size()));
    for (const EncodedCycle& cycle : cycles) {
        bytes.insert(bytes.end(), cycle.bytes.begin(), cycle.bytes.end());
    }

    output.snapshot = input;
    output.snapshot.cycles.clear();
    output.snapshot.cycles.reserve(cycles.size());
    for (const EncodedCycle& cycle : cycles) {
        PolylineCycle normalizedCycle;
        normalizedCycle.hole = cycle.hole;
        normalizedCycle.points.reserve(cycle.points.size());
        for (const auto& [x, y] : cycle.points) {
            normalizedCycle.points.push_back(
                {x * input.coordinateQuantumMm, y * input.coordinateQuantumMm}
            );
        }
        output.snapshot.cycles.push_back(std::move(normalizedCycle));
    }
    output.bytes = std::move(bytes);
    output.sha256 = sha256Hex(output.bytes);
    return ValidationResult::success();
}

std::string sha256Hex(const std::vector<std::uint8_t>& bytes)
{
    const QByteArray view(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<qsizetype>(bytes.size())
    );
    return QCryptographicHash::hash(view, QCryptographicHash::Sha256).toHex().toStdString();
}

const char* toString(const OperationStatus status)
{
    switch (status) {
        case OperationStatus::Complete:
            return "Complete";
        case OperationStatus::Inconclusive:
            return "Inconclusive";
        case OperationStatus::InvalidInput:
            return "InvalidInput";
        case OperationStatus::Unavailable:
            return "Unavailable";
        case OperationStatus::Cancelled:
            return "Cancelled";
        case OperationStatus::NumericalFailure:
            return "NumericalFailure";
        case OperationStatus::ResourceLimit:
            return "ResourceLimit";
    }
    return "InvalidInput";
}

const char* toString(const ConformanceDecision decision)
{
    switch (decision) {
        case ConformanceDecision::NotEvaluated:
            return "NotEvaluated";
        case ConformanceDecision::Pass:
            return "Pass";
        case ConformanceDecision::Fail:
            return "Fail";
        case ConformanceDecision::Inconclusive:
            return "Inconclusive";
    }
    return "NotEvaluated";
}

const char* toString(const DiagnosticCode code)
{
    switch (code) {
        case DiagnosticCode::None:
            return "None";
        case DiagnosticCode::InvalidSchema:
            return "InvalidSchema";
        case DiagnosticCode::InvalidGeometry:
            return "InvalidGeometry";
        case DiagnosticCode::UnsupportedGeometry:
            return "UnsupportedGeometry";
        case DiagnosticCode::TopologyFailure:
            return "TopologyFailure";
        case DiagnosticCode::NonFiniteValue:
            return "NonFiniteValue";
        case DiagnosticCode::ResourceLimit:
            return "ResourceLimit";
        case DiagnosticCode::OpenCVUnavailable:
            return "OpenCVUnavailable";
        case DiagnosticCode::IdentityMismatch:
            return "IdentityMismatch";
        case DiagnosticCode::MissingCalibration:
            return "MissingCalibration";
        case DiagnosticCode::LowImageQuality:
            return "LowImageQuality";
        case DiagnosticCode::NumericalFailure:
            return "NumericalFailure";
        case DiagnosticCode::Cancelled:
            return "Cancelled";
    }
    return "None";
}

}  // namespace Inspection::Photo
