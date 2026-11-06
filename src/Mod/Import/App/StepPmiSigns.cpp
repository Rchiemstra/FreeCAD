// SPDX-License-Identifier: LGPL-2.1-or-later
#include "StepPmiImport.h"

#include <cmath>
#include <Precision.hxx>
#include <STEPConstruct_UnitContext.hxx>
#include <StepBasic_LengthMeasureWithUnit.hxx>
#include <StepBasic_MeasureValueMember.hxx>
#include <StepBasic_NamedUnit.hxx>
#include <StepBasic_PlaneAngleMeasureWithUnit.hxx>
#include <StepBasic_SiUnitAndLengthUnit.hxx>
#include <StepData_StepModel.hxx>
#if OCC_VERSION_HEX >= 0x080000
#include <StepData_Factors.hxx>
#else
#include <StepData_GlobalFactors.hxx>
#endif
#include <StepRepr_ReprItemAndMeasureWithUnit.hxx>
#include <StepShape_PlusMinusTolerance.hxx>
#include <StepShape_ToleranceValue.hxx>

namespace Import
{
namespace
{
#if OCC_VERSION_HEX >= 0x080000
bool signedMeasure(const Handle(Standard_Transient)& source, double& value, std::string& unit,
                   const StepData_Factors& localFactors)
#else
bool signedMeasure(const Handle(Standard_Transient)& source, double& value, std::string& unit)
#endif
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
#if OCC_VERSION_HEX >= 0x080000
    if (context.ComputeFactors(measure->UnitComponent().NamedUnit(), localFactors) != 0) {
#else
    if (context.ComputeFactors(measure->UnitComponent().NamedUnit()) != 0) {
#endif
        return false;
    }
    value = measure->ValueComponent();
    if (context.LengthDone() && !context.PlaneAngleDone()) {
        // UnitContext divides by the transfer unit; undo that to obtain mm.
#if OCC_VERSION_HEX >= 0x080000
        value *= context.LengthFactor() * localFactors.CascadeUnit();
#else
        value *= context.LengthFactor()
            * StepData_GlobalFactors::Intance().CascadeUnit(); // codespell:ignore intance
#endif
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

// Only replace generic length bounds, leaving shared source measures untouched.
// Both readers scale typed length measures, but 7.6 leaves generic ones unscaled.
Handle(Standard_Transient) lengthBound(const Handle(Standard_Transient)& source,
                                      double value, const std::string& unit)
{
    auto measure = Handle(StepBasic_MeasureWithUnit)::DownCast(source);
    const auto item = Handle(StepRepr_ReprItemAndMeasureWithUnit)::DownCast(source);
    if (!item.IsNull()) { measure = item->GetMeasureWithUnit(); }
    if (unit != "mm" || measure.IsNull()
        || measure->DynamicType() != STANDARD_TYPE(StepBasic_MeasureWithUnit)) {
        return source;
    }
    Handle(StepBasic_SiUnitAndLengthUnit) mm = new StepBasic_SiUnitAndLengthUnit;
    mm->Init(true, StepBasic_spMilli, StepBasic_sunMetre);
    StepBasic_Unit mmUnit;
    mmUnit.SetValue(mm);
    Handle(StepBasic_MeasureValueMember) member = new StepBasic_MeasureValueMember;
    member->SetName("LENGTH_MEASURE");
    member->SetReal(value);
    Handle(StepBasic_LengthMeasureWithUnit) length = new StepBasic_LengthMeasureWithUnit;
    length->Init(member, mmUnit);
    return length;
}
}  // namespace

void StepPmiImport::captureSignedBounds(const Handle(StepData_StepModel)& model)
{
    if (model.IsNull()) {
        return;
    }
#if OCC_VERSION_HEX >= 0x080000
    StepData_Factors localFactors;
    localFactors.SetCascadeUnit(model->LocalLengthUnit());
#endif
    // Capture every original signature before replacing any bounds. Shared
    // tolerance values must not make subsequent candidates see rewritten units.
    std::vector<std::tuple<Handle(StepShape_ToleranceValue),
                          Handle(Standard_Transient), Handle(Standard_Transient)>> normalised;
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
#if OCC_VERSION_HEX >= 0x080000
        const bool hasLower = signedMeasure(bounds->LowerBound(), lower, lowerUnit, localFactors);
        const bool hasUpper = signedMeasure(bounds->UpperBound(), upper, upperUnit, localFactors);
#else
        const bool hasLower = signedMeasure(bounds->LowerBound(), lower, lowerUnit);
        const bool hasUpper = signedMeasure(bounds->UpperBound(), upper, upperUnit);
#endif
        normalised.emplace_back(bounds,
            hasLower ? lengthBound(bounds->LowerBound(), lower, lowerUnit) : bounds->LowerBound(),
            hasUpper ? lengthBound(bounds->UpperBound(), upper, upperUnit) : bounds->UpperBound());
        if (!hasLower || !hasUpper || lowerUnit != upperUnit) {
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
    for (const auto& [bounds, lower, upper] : normalised) {
        bounds->SetLowerBound(lower);
        bounds->SetUpperBound(upper);
    }
}
}  // namespace Import
