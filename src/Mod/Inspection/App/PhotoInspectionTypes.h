// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <Mod/Inspection/InspectionGlobal.h>

namespace Inspection::Photo
{

constexpr std::uint16_t projectionSchemaMajor = 1;
constexpr std::uint16_t projectionSchemaMinor = 0;
constexpr std::size_t maximumCycleCount = 4096;
constexpr std::size_t maximumPointCount = 1'000'000;
constexpr double coordinateLimitMm = 1.0e9;
constexpr double defaultCoordinateQuantumMm = 1.0e-6;

enum class OperationStatus
{
    Complete,
    Inconclusive,
    InvalidInput,
    Unavailable,
    Cancelled,
    NumericalFailure,
    ResourceLimit
};

enum class ConformanceDecision
{
    NotEvaluated,
    Pass,
    Fail,
    Inconclusive
};

enum class DiagnosticSeverity
{
    Information,
    Warning,
    Error
};

enum class DiagnosticCode
{
    None,
    InvalidSchema,
    InvalidGeometry,
    UnsupportedGeometry,
    TopologyFailure,
    NonFiniteValue,
    ResourceLimit,
    OpenCVUnavailable,
    IdentityMismatch,
    MissingCalibration,
    LowImageQuality,
    NumericalFailure,
    Cancelled
};

struct InspectionExport Diagnostic
{
    DiagnosticCode code {DiagnosticCode::None};
    DiagnosticSeverity severity {DiagnosticSeverity::Information};
    std::string message;
};

struct InspectionExport Vector2d
{
    double x {0.0};
    double y {0.0};
};

struct InspectionExport Vector3d
{
    double x {0.0};
    double y {0.0};
    double z {0.0};
};

struct InspectionExport CanonicalFrame
{
    Vector3d origin;
    Vector3d xAxis;
    Vector3d yAxis;
    Vector3d normal;
    std::uint16_t algorithmVersion {1};
};

struct InspectionExport PolylineCycle
{
    bool hole {false};
    std::vector<Vector2d> points;
};

struct InspectionExport ProjectionSnapshot
{
    CanonicalFrame frame;
    std::vector<PolylineCycle> cycles;
    std::array<double, 16> sourcePlacement {
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };
    double planarityToleranceMm {1.0e-6};
    double coordinateQuantumMm {defaultCoordinateQuantumMm};
};

struct InspectionExport CanonicalProjection
{
    ProjectionSnapshot snapshot;
    std::vector<std::uint8_t> bytes;
    std::string sha256;
};

struct InspectionExport ValidationResult
{
    bool valid {false};
    Diagnostic diagnostic;

    static ValidationResult success();
    static ValidationResult failure(DiagnosticCode code, std::string message);
};

InspectionExport std::optional<CanonicalFrame> makeCanonicalFrame(
    const Vector3d& pointOnPlane,
    const Vector3d& planeNormal
);

InspectionExport Vector2d projectToFrame(const CanonicalFrame& frame, const Vector3d& worldPoint);

InspectionExport ValidationResult
canonicalizeProjection(const ProjectionSnapshot& input, CanonicalProjection& output);

InspectionExport std::string sha256Hex(const std::vector<std::uint8_t>& bytes);

InspectionExport const char* toString(OperationStatus status);
InspectionExport const char* toString(ConformanceDecision decision);
InspectionExport const char* toString(DiagnosticCode code);

}  // namespace Inspection::Photo
