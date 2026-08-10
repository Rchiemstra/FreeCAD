// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionReport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

namespace Inspection::Photo
{
namespace
{

bool finite(const double value)
{
    return std::isfinite(value);
}

ReportSerialization failure(std::string message)
{
    return {
        false,
        {DiagnosticCode::NonFiniteValue, DiagnosticSeverity::Error, std::move(message)},
        {},
        {},
    };
}

std::string hash(const std::string& content)
{
    return sha256Hex({content.begin(), content.end()});
}

std::string csvCell(std::string value)
{
    if (!value.empty()
        && (value.front() == '=' || value.front() == '+' || value.front() == '-'
            || value.front() == '@')) {
        value.insert(value.begin(), '\'');
    }
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char character : value) {
        if (character == '"') {
            escaped += "\"\"";
        }
        else if (character == '\r' || character == '\n') {
            escaped.push_back(' ');
        }
        else {
            escaped.push_back(character);
        }
    }
    escaped.push_back('"');
    return escaped;
}

std::string number(const double value)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(15) << value;
    return stream.str();
}

}  // namespace

ReportSerialization toCanonicalJson(const AnalysisResult& result)
{
    QJsonObject root;
    root.insert("decision", toString(result.decision));

    QJsonArray diagnostics;
    for (const Diagnostic& item : result.diagnostics) {
        QJsonObject object;
        object.insert("code", toString(item.code));
        object.insert("message", QString::fromStdString(item.message));
        object.insert("severity", static_cast<int>(item.severity));
        diagnostics.append(object);
    }
    root.insert("diagnostics", diagnostics);
    root.insert("generation", static_cast<qint64>(result.generation));

    QJsonArray measurements;
    for (const Measurement& measurement : result.measurements) {
        if (!finite(measurement.nominalMm) || !finite(measurement.actualMm)
            || !finite(measurement.lowerToleranceMm) || !finite(measurement.upperToleranceMm)
            || !finite(measurement.expandedUncertaintyMm)) {
            return failure("report contains a non-finite measurement");
        }
        QJsonObject object;
        object.insert("actual_mm", measurement.actualMm);
        object.insert("decision", toString(measurement.decision));
        object.insert("expanded_uncertainty_mm", measurement.expandedUncertaintyMm);
        object.insert("id", QString::fromStdString(measurement.id));
        object.insert("lower_tolerance_mm", measurement.lowerToleranceMm);
        object.insert("nominal_mm", measurement.nominalMm);
        object.insert("upper_tolerance_mm", measurement.upperToleranceMm);
        measurements.append(object);
    }
    root.insert("measurements", measurements);
    root.insert("photo_inspection_schema_version", QJsonArray {1, 0});
    root.insert("projection_geometry_sha256", QString::fromStdString(result.projectionGeometrySha256));
    root.insert("status", toString(result.status));

    const std::string content = QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
    if (content.size() > maximumReportBytes) {
        return {
            false,
            {
                DiagnosticCode::ResourceLimit,
                DiagnosticSeverity::Error,
                "canonical report exceeds the byte limit",
            },
            {},
            {},
        };
    }
    return {true, {}, content, hash(content)};
}

ReportSerialization toCsvMeasurements(const AnalysisResult& result)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "id,nominal_mm,actual_mm,lower_tolerance_mm,upper_tolerance_mm,expanded_uncertainty_"
              "mm,decision\r\n";
    for (const Measurement& measurement : result.measurements) {
        if (!finite(measurement.nominalMm) || !finite(measurement.actualMm)
            || !finite(measurement.lowerToleranceMm) || !finite(measurement.upperToleranceMm)
            || !finite(measurement.expandedUncertaintyMm)) {
            return failure("CSV report contains a non-finite measurement");
        }
        stream << csvCell(measurement.id) << ',' << number(measurement.nominalMm) << ','
               << number(measurement.actualMm) << ',' << number(measurement.lowerToleranceMm) << ','
               << number(measurement.upperToleranceMm) << ','
               << number(measurement.expandedUncertaintyMm) << ',' << toString(measurement.decision)
               << "\r\n";
        if (static_cast<std::size_t>(stream.tellp()) > maximumReportBytes) {
            return {
                false,
                {
                    DiagnosticCode::ResourceLimit,
                    DiagnosticSeverity::Error,
                    "CSV report exceeds the byte limit",
                },
                {},
                {},
            };
        }
    }

    const std::string content = stream.str();
    return {true, {}, content, hash(content)};
}

VectorScene buildResultScene(const AnalysisResult& result)
{
    VectorScene scene;
    scene.widthMm = 210.0;
    scene.heightMm = 297.0;

    for (const DeviationSample& sample : result.deviations) {
        ScenePrimitive vector;
        vector.id = "deviation-" + std::to_string(sample.cycleIndex) + '-'
            + std::to_string(sample.sampleIndex);
        vector.layer = "deviations";
        vector.kind = ScenePrimitiveKind::Polyline;
        vector.points = {sample.nearestNominalPoint, sample.measuredPoint};
        vector.strokeWidthMm = 0.15;
        scene.primitives.push_back(std::move(vector));
    }

    ScenePrimitive decision;
    decision.id = "result-decision";
    decision.layer = "decision";
    decision.kind = ScenePrimitiveKind::Text;
    decision.points = {{5.0, 10.0}};
    decision.text = std::string("Status: ") + toString(result.status)
        + " Decision: " + toString(result.decision);
    scene.primitives.push_back(std::move(decision));
    return scene;
}

}  // namespace Inspection::Photo
