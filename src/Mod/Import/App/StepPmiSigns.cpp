// SPDX-License-Identifier: LGPL-2.1-or-later
#include "StepPmiImport.h"

#include <cmath>
#include <Precision.hxx>
#include <STEPConstruct_UnitContext.hxx>
#include <StepBasic_LengthMeasureWithUnit.hxx>
#include <StepBasic_NamedUnit.hxx>
#include <StepBasic_PlaneAngleMeasureWithUnit.hxx>
#include <StepData_StepModel.hxx>
#include <StepRepr_ReprItemAndMeasureWithUnit.hxx>
#include <StepShape_PlusMinusTolerance.hxx>
#include <StepShape_ToleranceValue.hxx>

namespace Import
{
namespace
{
bool signedMeasure(const Handle(Standard_Transient)& source, double& value, std::string& unit)
{
    auto measure = Handle(StepBasic_MeasureWithUnit)::DownCast(source);
    const auto item = Handle(StepRepr_ReprItemAndMeasureWithUnit)::DownCast(source);
    if (!item.IsNull()) {
        measure = item->GetMeasureWithUnit();
    }
    if (measure.IsNull() || measure->UnitComponent().NamedUnit().IsNull()) {
        return false;
    }
    STEPConstruct_UnitContext context;
    if (context.ComputeFactors(measure->UnitComponent().NamedUnit()) != 0) {
        return false;
    }
    value = measure->ValueComponent();
    if (context.LengthDone() && !context.PlaneAngleDone()) {
        value *= context.LengthFactor();
        unit = "mm";
    }
    else if (context.PlaneAngleDone() && !context.LengthDone()) {
        // Match STEPCAFControl_Reader's conversion exactly; a mismatch fails closed.
        const double factor = context.PlaneAngleFactor() * 180 / M_PI;
        if (std::abs(1. - factor) > Precision::Confusion()) {
            value *= factor;
        }
        unit = "deg";
    }
    else {
        return false;
    }
    return std::isfinite(value);
}
}  // namespace

void StepPmiImport::captureSignedBounds(const Handle(StepData_StepModel)& model)
{
    if (model.IsNull()) {
        return;
    }
    for (int i = 1; i <= model->NbEntities(); ++i) {
        const auto tolerance = Handle(StepShape_PlusMinusTolerance)::DownCast(model->Value(i));
        if (tolerance.IsNull()) {
            continue;
        }
        const auto bounds = tolerance->Range().ToleranceValue();
        if (bounds.IsNull()) {
            continue;
        }
        double lower = 0, upper = 0;
        std::string lowerUnit, upperUnit;
        if (!signedMeasure(bounds->LowerBound(), lower, lowerUnit)
            || !signedMeasure(bounds->UpperBound(), upper, upperUnit) || lowerUnit != upperUnit) {
            continue;
        }
        // A magnitude match is exact, in normalised units. If more than one source
        // can correspond, ALL must agree on both signed values. Never choose one
        // by traversal order, label order, proximity, or a fuzzy numeric match.
        if (upper < 0) {
            // OCCT 7.6 drops a negative upper tolerance and misclassifies the
            // remaining [nominal, abs(lower)] array as an absolute range.
            // This index is ONLY a veto on interpreting such arrays as bounds.
            lostUpperBounds.emplace(lowerUnit, std::abs(lower));
        }
        const BoundsKey key {lowerUnit, std::abs(lower), std::abs(upper)};
        auto inserted = signedBounds.emplace(key, SignedBounds {lower, upper, false});
        if (!inserted.second) {
            auto& previous = inserted.first->second;
            previous.conflicting |= previous.lower != lower || previous.upper != upper;
        }
    }
}
}  // namespace Import
