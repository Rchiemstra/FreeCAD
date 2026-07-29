// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <string>
#include <vector>

#include <Mod/Inspection/InspectionGlobal.h>

#include "PhotoInspectionTypes.h"

namespace Inspection::Photo
{

struct InspectionExport UncertaintyComponent
{
    std::string name;
    double standardUncertaintyMm {0.0};
    std::string correlationGroup;
};

struct InspectionExport UncertaintyBudget
{
    std::vector<UncertaintyComponent> components;
    double combinedStandardUncertaintyMm {0.0};
    double coverageFactor {2.0};
    double expandedUncertaintyMm {0.0};
};

struct InspectionExport CircleMeasurement
{
    OperationStatus status {OperationStatus::InvalidInput};
    Diagnostic diagnostic;
    Vector2d center;
    double radiusMm {0.0};
    double diameterMm {0.0};
    double angularCoverageRadians {0.0};
    double rmsResidualMm {0.0};
};

struct InspectionExport ExtentMeasurement
{
    OperationStatus status {OperationStatus::InvalidInput};
    Diagnostic diagnostic;
    double minimumXmm {0.0};
    double maximumXmm {0.0};
    double minimumYmm {0.0};
    double maximumYmm {0.0};
    double widthMm {0.0};
    double heightMm {0.0};
};

InspectionExport ValidationResult combineUncertaintyBudget(UncertaintyBudget& budget);

InspectionExport CircleMeasurement
fitCircleFeature(const std::vector<Vector2d>& points, double minimumAngularCoverageRadians);

InspectionExport ExtentMeasurement measureExtents(const std::vector<PolylineCycle>& cycles);

}  // namespace Inspection::Photo
