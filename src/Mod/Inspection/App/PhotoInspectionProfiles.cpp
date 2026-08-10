// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionProfiles.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace Inspection::Photo
{
namespace
{

constexpr std::size_t maximumJsonDepth = 8;

bool finite(const double value)
{
    return std::isfinite(value);
}

bool safeToken(const std::string& value, const std::size_t maximumLength)
{
    if (value.empty() || value.size() > maximumLength) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isalnum(character) || character == '-' || character == '_' || character == '.'
            || character == ':';
    });
}

ProfileResult failure(const DiagnosticCode code, std::string message)
{
    return {
        false,
        false,
        {code, DiagnosticSeverity::Error, std::move(message)},
        {},
        {},
    };
}

std::string compact(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

std::string hashJson(const std::string& json)
{
    return sha256Hex({json.begin(), json.end()});
}

QJsonArray doubles(const double* begin, const double* end)
{
    QJsonArray result;
    for (const double* current = begin; current != end; ++current) {
        result.append(*current);
    }
    return result;
}

QJsonArray doubles(const std::vector<double>& values)
{
    QJsonArray result;
    for (const double value : values) {
        result.append(value);
    }
    return result;
}

QJsonObject cameraObject(const CameraProfile& profile)
{
    QJsonObject object;
    object.insert("accepted_frames", profile.acceptedFrames);
    object.insert("board_hash", QString::fromStdString(profile.boardHash));
    object.insert("camera_fingerprint", QString::fromStdString(profile.cameraFingerprint));
    object.insert(
        "camera_matrix",
        doubles(profile.cameraMatrix.data(), profile.cameraMatrix.data() + profile.cameraMatrix.size())
    );
    object.insert("distortion", doubles(profile.distortion));
    object.insert("image_height", profile.imageHeight);
    object.insert("image_width", profile.imageWidth);
    object.insert("policy_version", QString::fromStdString(profile.policyVersion));
    object.insert("rms_reprojection_error_px", profile.rmsReprojectionErrorPx);
    object.insert("schema_major", profile.schemaMajor);
    object.insert("schema_minor", profile.schemaMinor);
    object.insert("uuid", QString::fromStdString(profile.uuid));
    object.insert("validated", profile.validated);
    return object;
}

QJsonObject printerObject(const PrinterProfile& profile)
{
    const auto& values = profile.physicalFromCommand.coefficients();
    QJsonObject object;
    object.insert("calibration_points", profile.calibrationPoints);
    object.insert("fit_residual_mm", profile.fitResidualMm);
    object.insert("held_out_spans", profile.heldOutSpans);
    object.insert("media", QString::fromStdString(profile.media));
    object.insert("physical_from_command", doubles(values.data(), values.data() + values.size()));
    object.insert("policy_version", QString::fromStdString(profile.policyVersion));
    object.insert("printer_fingerprint", QString::fromStdString(profile.printerFingerprint));
    object.insert("repeatability_mm", profile.repeatabilityMm);
    object.insert("repeated_prints", profile.repeatedPrints);
    object.insert("schema_major", profile.schemaMajor);
    object.insert("schema_minor", profile.schemaMinor);
    object.insert("uuid", QString::fromStdString(profile.uuid));
    object.insert("validated", profile.validated);
    return object;
}

bool jsonHasDuplicateObjectKeyOrExcessDepth(const std::string& json, std::string& reason)
{
    std::vector<std::set<std::string>> objectKeys;
    std::size_t depth = 0;
    bool inString = false;
    bool escape = false;
    std::size_t stringStart = 0;

    for (std::size_t index = 0; index < json.size(); ++index) {
        const char character = json[index];
        if (inString) {
            if (escape) {
                escape = false;
                continue;
            }
            if (character == '\\') {
                escape = true;
                continue;
            }
            if (character != '"') {
                continue;
            }

            inString = false;
            std::size_t next = index + 1;
            while (next < json.size() && std::isspace(static_cast<unsigned char>(json[next]))) {
                ++next;
            }
            if (next < json.size() && json[next] == ':' && !objectKeys.empty()) {
                const std::string key = json.substr(stringStart, index - stringStart);
                if (!objectKeys.back().insert(key).second) {
                    reason = "profile JSON contains duplicate object key: " + key;
                    return true;
                }
            }
            continue;
        }

        if (character == '"') {
            inString = true;
            stringStart = index + 1;
        }
        else if (character == '{') {
            objectKeys.emplace_back();
            ++depth;
        }
        else if (character == '[') {
            ++depth;
        }
        else if (character == '}') {
            if (!objectKeys.empty()) {
                objectKeys.pop_back();
            }
            if (depth > 0) {
                --depth;
            }
        }
        else if (character == ']') {
            if (depth > 0) {
                --depth;
            }
        }

        if (depth > maximumJsonDepth) {
            reason = "profile JSON exceeds maximum nesting depth";
            return true;
        }
    }
    return false;
}

bool exactKeys(const QJsonObject& object, const std::set<QString>& expected, std::string& reason)
{
    std::set<QString> actual;
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
        actual.insert(iterator.key());
    }
    if (actual != expected) {
        reason = "profile JSON fields do not match schema";
        return false;
    }
    return true;
}

bool integer(const QJsonValue& value, int& output)
{
    if (!value.isDouble() || !finite(value.toDouble())) {
        return false;
    }
    const double number = value.toDouble();
    if (std::floor(number) != number || number < std::numeric_limits<int>::min()
        || number > std::numeric_limits<int>::max()) {
        return false;
    }
    output = static_cast<int>(number);
    return true;
}

bool numberArray(
    const QJsonValue& value,
    const int minimumSize,
    const int maximumSize,
    std::vector<double>& output
)
{
    if (!value.isArray()) {
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() < minimumSize || array.size() > maximumSize) {
        return false;
    }
    output.clear();
    output.reserve(array.size());
    for (const QJsonValue element : array) {
        if (!element.isDouble() || !finite(element.toDouble())) {
            return false;
        }
        output.push_back(element.toDouble());
    }
    return true;
}

std::optional<QJsonObject> parseObject(const std::string& json, std::string& reason)
{
    if (json.empty() || json.size() > maximumProfileBytes) {
        reason = "profile JSON is empty or exceeds the byte limit";
        return std::nullopt;
    }
    if (jsonHasDuplicateObjectKeyOrExcessDepth(json, reason)) {
        return std::nullopt;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        reason = "profile JSON is malformed or is not an object";
        return std::nullopt;
    }
    return document.object();
}

}  // namespace

ProfileResult validateCameraProfile(const CameraProfile& profile)
{
    if (profile.schemaMajor != 1 || profile.schemaMinor < 0 || !safeToken(profile.uuid, 64)
        || !safeToken(profile.boardHash, 128) || !safeToken(profile.cameraFingerprint, 128)
        || !safeToken(profile.policyVersion, 64)) {
        return failure(DiagnosticCode::InvalidSchema, "camera profile identity/schema is invalid");
    }
    if (profile.imageWidth < 64 || profile.imageWidth > 65535 || profile.imageHeight < 64
        || profile.imageHeight > 65535 || profile.acceptedFrames < 3
        || profile.acceptedFrames > 100000 || profile.distortion.size() > 14) {
        return failure(DiagnosticCode::InvalidSchema, "camera profile dimensions/counts are invalid");
    }
    for (const double value : profile.cameraMatrix) {
        if (!finite(value)) {
            return failure(DiagnosticCode::NonFiniteValue, "camera matrix contains a non-finite value");
        }
    }
    for (const double value : profile.distortion) {
        if (!finite(value)) {
            return failure(
                DiagnosticCode::NonFiniteValue,
                "distortion vector contains a non-finite value"
            );
        }
    }
    if (profile.cameraMatrix[0] <= 0.0 || profile.cameraMatrix[4] <= 0.0
        || profile.cameraMatrix[2] < 0.0 || profile.cameraMatrix[2] > profile.imageWidth
        || profile.cameraMatrix[5] < 0.0 || profile.cameraMatrix[5] > profile.imageHeight
        || !finite(profile.rmsReprojectionErrorPx) || profile.rmsReprojectionErrorPx < 0.0) {
        return failure(DiagnosticCode::InvalidSchema, "camera calibration matrix/residual is invalid");
    }

    const std::string json = compact(cameraObject(profile));
    return {
        true,
        profile.validated,
        {},
        json,
        hashJson(json),
    };
}

ProfileResult validatePrinterProfile(const PrinterProfile& profile)
{
    if (profile.schemaMajor != 1 || profile.schemaMinor < 0 || !safeToken(profile.uuid, 64)
        || !safeToken(profile.printerFingerprint, 128) || !safeToken(profile.media, 16)
        || !safeToken(profile.policyVersion, 64)) {
        return failure(DiagnosticCode::InvalidSchema, "printer profile identity/schema is invalid");
    }
    if (!profile.physicalFromCommand.isFinite()
        || !printerCommandFromPhysical(profile.physicalFromCommand).has_value()
        || !finite(profile.fitResidualMm) || profile.fitResidualMm < 0.0
        || !finite(profile.repeatabilityMm) || profile.repeatabilityMm < 0.0
        || profile.calibrationPoints < 3 || profile.heldOutSpans < 0 || profile.repeatedPrints < 1) {
        return failure(DiagnosticCode::InvalidSchema, "printer transform/evidence is invalid");
    }

    const bool decisionCapable = profile.validated && profile.calibrationPoints >= 9
        && profile.heldOutSpans >= 2 && profile.repeatedPrints >= 5;
    const std::string json = compact(printerObject(profile));
    return {
        true,
        decisionCapable,
        {},
        json,
        hashJson(json),
    };
}

ProfileResult parseCameraProfile(const std::string& json, CameraProfile& profile)
{
    std::string reason;
    const auto object = parseObject(json, reason);
    if (!object) {
        return failure(DiagnosticCode::InvalidSchema, reason);
    }
    static const std::set<QString> fields {
        "accepted_frames",
        "board_hash",
        "camera_fingerprint",
        "camera_matrix",
        "distortion",
        "image_height",
        "image_width",
        "policy_version",
        "rms_reprojection_error_px",
        "schema_major",
        "schema_minor",
        "uuid",
        "validated",
    };
    if (!exactKeys(*object, fields, reason)) {
        return failure(DiagnosticCode::InvalidSchema, reason);
    }

    CameraProfile parsed;
    std::vector<double> matrix;
    if (!integer((*object)["schema_major"], parsed.schemaMajor)
        || !integer((*object)["schema_minor"], parsed.schemaMinor)
        || !integer((*object)["image_width"], parsed.imageWidth)
        || !integer((*object)["image_height"], parsed.imageHeight)
        || !integer((*object)["accepted_frames"], parsed.acceptedFrames)
        || !numberArray((*object)["camera_matrix"], 9, 9, matrix)
        || !numberArray((*object)["distortion"], 0, 14, parsed.distortion)
        || !(*object)["rms_reprojection_error_px"].isDouble() || !(*object)["validated"].isBool()
        || !(*object)["uuid"].isString() || !(*object)["board_hash"].isString()
        || !(*object)["camera_fingerprint"].isString() || !(*object)["policy_version"].isString()) {
        return failure(DiagnosticCode::InvalidSchema, "camera profile field type is invalid");
    }
    std::copy(matrix.begin(), matrix.end(), parsed.cameraMatrix.begin());
    parsed.rmsReprojectionErrorPx = (*object)["rms_reprojection_error_px"].toDouble();
    parsed.validated = (*object)["validated"].toBool();
    parsed.uuid = (*object)["uuid"].toString().toStdString();
    parsed.boardHash = (*object)["board_hash"].toString().toStdString();
    parsed.cameraFingerprint = (*object)["camera_fingerprint"].toString().toStdString();
    parsed.policyVersion = (*object)["policy_version"].toString().toStdString();

    ProfileResult result = validateCameraProfile(parsed);
    if (result.valid) {
        profile = std::move(parsed);
    }
    return result;
}

ProfileResult parsePrinterProfile(const std::string& json, PrinterProfile& profile)
{
    std::string reason;
    const auto object = parseObject(json, reason);
    if (!object) {
        return failure(DiagnosticCode::InvalidSchema, reason);
    }
    static const std::set<QString> fields {
        "calibration_points",
        "fit_residual_mm",
        "held_out_spans",
        "media",
        "physical_from_command",
        "policy_version",
        "printer_fingerprint",
        "repeatability_mm",
        "repeated_prints",
        "schema_major",
        "schema_minor",
        "uuid",
        "validated",
    };
    if (!exactKeys(*object, fields, reason)) {
        return failure(DiagnosticCode::InvalidSchema, reason);
    }

    PrinterProfile parsed;
    std::vector<double> transform;
    if (!integer((*object)["schema_major"], parsed.schemaMajor)
        || !integer((*object)["schema_minor"], parsed.schemaMinor)
        || !integer((*object)["calibration_points"], parsed.calibrationPoints)
        || !integer((*object)["held_out_spans"], parsed.heldOutSpans)
        || !integer((*object)["repeated_prints"], parsed.repeatedPrints)
        || !numberArray((*object)["physical_from_command"], 6, 6, transform)
        || !(*object)["fit_residual_mm"].isDouble() || !(*object)["repeatability_mm"].isDouble()
        || !(*object)["validated"].isBool() || !(*object)["uuid"].isString()
        || !(*object)["printer_fingerprint"].isString() || !(*object)["media"].isString()
        || !(*object)["policy_version"].isString()) {
        return failure(DiagnosticCode::InvalidSchema, "printer profile field type is invalid");
    }
    parsed.physicalFromCommand = AffineTransform2d(
        {transform[0], transform[1], transform[2], transform[3], transform[4], transform[5]}
    );
    parsed.fitResidualMm = (*object)["fit_residual_mm"].toDouble();
    parsed.repeatabilityMm = (*object)["repeatability_mm"].toDouble();
    parsed.validated = (*object)["validated"].toBool();
    parsed.uuid = (*object)["uuid"].toString().toStdString();
    parsed.printerFingerprint = (*object)["printer_fingerprint"].toString().toStdString();
    parsed.media = (*object)["media"].toString().toStdString();
    parsed.policyVersion = (*object)["policy_version"].toString().toStdString();

    ProfileResult result = validatePrinterProfile(parsed);
    if (result.valid) {
        profile = std::move(parsed);
    }
    return result;
}

}  // namespace Inspection::Photo
