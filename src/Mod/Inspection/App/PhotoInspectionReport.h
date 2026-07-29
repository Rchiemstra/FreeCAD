// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <string>

#include <Mod/Inspection/InspectionGlobal.h>

#include "PhotoInspectionEngine.h"
#include "PhotoInspectionSheet.h"

namespace Inspection::Photo
{

constexpr std::size_t maximumReportBytes = 8 * 1024 * 1024;

struct InspectionExport ReportSerialization
{
    bool valid {false};
    Diagnostic diagnostic;
    std::string content;
    std::string sha256;
};

InspectionExport ReportSerialization toCanonicalJson(const AnalysisResult& result);
InspectionExport ReportSerialization toCsvMeasurements(const AnalysisResult& result);
InspectionExport VectorScene buildResultScene(const AnalysisResult& result);

}  // namespace Inspection::Photo
