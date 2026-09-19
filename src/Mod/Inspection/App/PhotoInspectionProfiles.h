// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <string>
#include <vector>

#include <Mod/Inspection/InspectionGlobal.h>

#include "PhotoInspectionTransform.h"
#include "PhotoInspectionTypes.h"

namespace Inspection::Photo
{

constexpr std::size_t maximumProfileBytes = 64 * 1024;

struct InspectionExport CameraProfile
{
    int schemaMajor {1};
    int schemaMinor {0};
    std::string uuid;
    int imageWidth {0};
    int imageHeight {0};
    std::array<double, 9> cameraMatrix {};
    std::vector<double> distortion;
    double rmsReprojectionErrorPx {0.0};
    int acceptedFrames {0};
    std::string boardHash;
    std::string cameraFingerprint;
    std::string policyVersion;
    bool validated {false};
};

struct InspectionExport PrinterProfile
{
    int schemaMajor {1};
    int schemaMinor {0};
    std::string uuid;
    std::string printerFingerprint;
    std::string media;
    AffineTransform2d physicalFromCommand;
    double fitResidualMm {0.0};
    double repeatabilityMm {0.0};
    int calibrationPoints {0};
    int heldOutSpans {0};
    int repeatedPrints {0};
    std::string policyVersion;
    bool validated {false};
};

struct InspectionExport ProfileResult
{
    bool valid {false};
    bool decisionCapable {false};
    Diagnostic diagnostic;
    std::string canonicalJson;
    std::string sha256;
};

InspectionExport ProfileResult validateCameraProfile(const CameraProfile& profile);
InspectionExport ProfileResult validatePrinterProfile(const PrinterProfile& profile);

InspectionExport ProfileResult parseCameraProfile(const std::string& json, CameraProfile& profile);
InspectionExport ProfileResult parsePrinterProfile(const std::string& json, PrinterProfile& profile);

}  // namespace Inspection::Photo
