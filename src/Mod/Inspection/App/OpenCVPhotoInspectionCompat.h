// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <Mod/Inspection/InspectionGlobal.h>

namespace Inspection::Photo
{

struct InspectionExport OpenCVCapability
{
    bool requested {false};
    bool available {false};
    int versionMajor {0};
    int versionMinor {0};
    int versionRevision {0};
    std::string version;
    std::string components;
    std::string compatibilityBranch;
    std::string reason;
    std::string buildInformation;
};

struct InspectionExport BinaryGrid
{
    int rows {0};
    int columns {0};
    std::vector<std::uint8_t> cells;

    bool valid() const;
    bool black(int row, int column) const;
};

class InspectionExport OpenCVPhotoInspectionCompat
{
public:
    static OpenCVCapability capability();

    // Performs an allocation-bounded PNG encode/decode and QR construction smoke
    // test. It never throws across the Inspection boundary.
    static bool runtimeSmokeTest(std::string& reason);

    static bool markerGrid(int dictionaryId, int markerId, BinaryGrid& output, std::string& reason);
    static bool qrGrid(const std::string& payload, BinaryGrid& output, std::string& reason);
};

}  // namespace Inspection::Photo
