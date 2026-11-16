// SPDX-License-Identifier: LGPL-2.1-or-later
#include "StepPmiExport.h"
#include "WriterStep.h"
#include "StepAttributesExport.h"

#include <algorithm>
#include <numbers>
#include <map>
#include <set>
#include <stdexcept>
#include <Standard_Version.hxx>
#include <UnitsMethods.hxx>
#include <TDataStd_Name.hxx>
#include <TDF_LabelSequence.hxx>
#include <StepData_StepModel.hxx>
#include <StepBasic_LengthMeasureWithUnit.hxx>
#include <StepBasic_NamedUnit.hxx>
#include <StepBasic_PlaneAngleMeasureWithUnit.hxx>
#include <StepBasic_MeasureValueMember.hxx>
#include <StepBasic_MeasureWithUnit.hxx>
#include <StepShape_ToleranceValue.hxx>
#include <STEPConstruct_UnitContext.hxx>
#include <STEPCAFControl_GDTProperty.hxx>
#include <StepDimTol_GeometricTolerance.hxx>
#include <StepDimTol_ProjectedZoneDefinition.hxx>
#include <StepDimTol_ToleranceZone.hxx>
#include <StepDimTol_ToleranceZoneForm.hxx>
#include <StepDimTol_HArray1OfToleranceZoneTarget.hxx>
#include <StepRepr_ShapeAspect.hxx>
#include <StepRepr_ReprItemAndMeasureWithUnit.hxx>
#include <StepShape_DimensionalLocation.hxx>
#include <StepShape_DimensionalSize.hxx>
#include <XCAFDoc_Datum.hxx>
#include <XCAFDoc_Dimension.hxx>
#include <XCAFDoc_DimTolTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_GeomTolerance.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDimTolObjects_DatumObject.hxx>
#include <XCAFDimTolObjects_DimensionObject.hxx>
#include <XCAFDimTolObjects_GeomToleranceObject.hxx>
#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObjectPy.h>
#include <Base/Console.h>
#include <Base/FileInfo.h>
#include <Mod/Part/App/Interface.h>
#include <Mod/Part/App/PartFeature.h>

namespace Import
{
namespace
{
std::string str(const Py::Dict& data, const char* key)
{
    return Py::String(data.getItem(key)).as_std_string();
}
double num(const Py::Dict& data, const char* key)
{
    return Py::Float(data.getItem(key));
}
bool flag(const Py::Dict& data, const char* key)
{
    return Py::Boolean(data.getItem(key));
}
App::DocumentObject* object(const Py::Object& value)
{
    return static_cast<App::DocumentObjectPy*>(value.ptr())->getDocumentObjectPtr();
}
Handle(TCollection_HAsciiString) name(const Py::Dict& data, const char* key)
{
    return new TCollection_HAsciiString(str(data, key).c_str());
}
Py::Dict result(const Py::Dict& item, const std::string& reason)
{
    Py::Dict row;
    row["object"] = item.getItem("object").getAttr("Name");
    row["status"] = Py::String(reason.empty() ? "written" : "skipped");
    row["reason"] = Py::String(reason);
    return row;
}
}  // namespace

StepPmiExport::StepPmiExport(int override)
    : enabled(override < 0 ? App::GetApplication().GetParameterGroupByPath(
          "User parameter:BaseApp/Preferences/Mod/Import")->GetBool("ExportSemanticPMI", false)
          : override != 0)
{}

ShapeExportObserver StepPmiExport::observer()
{
    if (!enabled) {
        return {};
    }
    return [this](const TDF_Label& label, App::DocumentObject* owner,
                  const Part::TopoShape& shape) noexcept {
        try {
            associations.push_back({label, owner, shape});
        }
        catch (...) {
            failed = true;
        }
    };
}

void StepPmiExport::populate(const Handle(TDocStd_Document)& document,
                             const Py::List& candidates, Py::List& report)
{
    const auto shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const auto tool = XCAFDoc_DocumentTool::DimTolTool(document->Main());
    const std::string unit = Part::Interface::writeStepUnit();
    const auto lengthUnit = UnitsMethods::LengthUnitFromString(
        unit == "INCH" ? "in" : unit.c_str(), false);
    if (lengthUnit == UnitsMethods_LengthUnit_Undefined) {
        throw std::runtime_error("unsupported semantic PMI output unit");
    }
    const double lengthFactor = UnitsMethods::GetLengthUnitScale(
        UnitsMethods_LengthUnit_Millimeter, lengthUnit);
    std::map<App::DocumentObject*, TDF_Label> datums;
    std::map<std::string, App::DocumentObject*> datumIdentifiers;
    std::map<App::DocumentObject*, std::string> reasons;
    // Datums must exist before tolerances; source identifiers are scoped to each import.
    for (const bool datumPass : {true, false}) {
        for (const auto& value : candidates) {
            Py::Dict item(value);
            Py::Dict record(item.getItem("record"));
            auto owner = object(item.getItem("object"));
            std::string reason = str(item, "reason");
            if (!reason.empty()) {
                reasons[owner] = reason;
                continue;
            }
            const auto category = str(record, "category");
            if ((category == "Datum") != datumPass) {
                continue;
            }
            TDF_LabelSequence first, second;
            Py::List references(item.getItem("references"));
            for (const auto& reference : references) {
                Py::Tuple ref(reference);
                auto refOwner = object(ref[0]);
                const std::string sub = Py::String(ref[1]).as_std_string();
                const std::string role = Py::String(ref[2]).as_std_string();
                TDF_Label target;
                TopoDS_Shape subShape;
                for (const auto& association : associations) {
                    if (association.owner != refOwner || shapes->IsComponent(association.label)) {
                        continue;
                    }
                    const auto candidate = association.shape.getSubShape(sub.c_str());
                    if (candidate.IsNull()) {
                        continue;
                    }
                    if (!target.IsNull() && target != association.label) {
                        reason = "ambiguous exported shape instances";
                        break;
                    }
                    target = association.label;
                    subShape = candidate;
                }
                if (!reason.empty()) {
                    break;
                }
                if (target.IsNull()) {
                    reason = "referenced owner/face/edge not exported";
                    break;
                }
                std::vector<TDF_Label> ancestors;
                const auto occurrences = [&](const auto& self, const TDF_Label& label) -> int {
                    if (std::find(ancestors.begin(), ancestors.end(), label) != ancestors.end()) {
                        return 2;
                    }
                    TDF_LabelSequence users;
                    shapes->GetUsers(label, users);
                    if (users.IsEmpty()) {
                        return 1;
                    }
                    ancestors.push_back(label);
                    int count = 0;
                    for (int i = 1; i <= users.Length() && count < 2; ++i) {
                        count += self(self, users.Value(i).Father());
                    }
                    ancestors.pop_back();
                    return count;
                };
                if (occurrences(occurrences, target) > 1) {
                    reason = "ambiguous exported shape instances";
                    break;
                }
                // These are the converter's own subshapes, never a geometric search.
                auto label = shapes->AddSubShape(target, subShape);
                if (label.IsNull()) {
                    reason = "exact exported subshape label unavailable";
                    break;
                }
                if (role == "second") {
                    second.Append(label);
                }
                else {
                    first.Append(label);
                }
            }
            Py::List datumObjects(item.getItem("object").getAttr("Datums"));
            for (const auto& datum : datumObjects) {
                if (reason.empty() && !datums.count(object(datum))) {
                    reason = "referenced datum was skipped";
                }
            }
            if (first.IsEmpty() && reason.empty()) {
                reason = "no exported target reference";
            }
            if (!reason.empty()) {
                reasons[owner] = reason;
                continue;
            }
            Py::Dict numbers(record.getItem("numbers"));
            const int type = static_cast<int>(num(record, "type"));
            const int modifiers = static_cast<int>(Py::List(record.getItem("modifiers")).size());
            if (category == "Datum") {
                const auto [identifier, unique] = datumIdentifiers.emplace(
                    str(record, "datumName"), owner);
                if (!unique) {
                    reason = "duplicate datum identifier; writer cannot distinguish targets";
                    reasons[owner] = reason;
                    reasons[identifier->second] = reason;
                    const auto previous = datums.find(identifier->second);
                    if (previous != datums.end()) {
                        previous->second.ForgetAllAttributes(true);
                        datums.erase(previous);
                    }
                    continue;
                }
                Handle(XCAFDimTolObjects_DatumObject) data = new XCAFDimTolObjects_DatumObject;
                data->SetName(name(record, "datumName"));
                data->SetSemanticName(name(record, "name"));
                data->SetPosition(static_cast<int>(num(record, "datumPosition")));
                for (int i = 1; i <= modifiers; ++i) {
                    data->AddModifier(static_cast<XCAFDimTolObjects_DatumSingleModif>(
                        num(numbers, ("modifierCode" + std::to_string(i)).c_str())));
                }
                data->SetModifierWithValue(static_cast<XCAFDimTolObjects_DatumModifWithValue>(
                    num(numbers, "modifierWithValueCode")),
                    num(numbers, "modifierValue") * lengthFactor);
                const auto label = tool->AddDatum();
                XCAFDoc_Datum::Set(label)->SetObject(data);
                tool->SetDatum(first, label);
                datums[owner] = label;
            }
            else if (category == "Dimension") {
                Handle(XCAFDimTolObjects_DimensionObject) data
                    = new XCAFDimTolObjects_DimensionObject;
                data->SetType(static_cast<XCAFDimTolObjects_DimensionType>(type));
                data->SetSemanticName(name(record, "name"));
                // STEPCAF reads angles as degrees, but its writer expects radians.
                const double factor = str(record, "unit") == "deg"
                    ? std::numbers::pi / 180.0 : lengthFactor;
                data->SetValue(num(record, "value") * factor);
                if (flag(record, "hasBounds")) {
                    data->SetLowerBound(num(record, "lower") * factor);
                    data->SetUpperBound(num(record, "upper") * factor);
                }
                if (flag(record, "hasDeviations")) {
                    data->SetLowerTolValue(-num(record, "lower") * factor);
                    data->SetUpperTolValue(num(record, "upper") * factor);
                }
                data->SetQualifier(static_cast<XCAFDimTolObjects_DimensionQualifier>(
                    num(numbers, "qualifier")));
                data->SetNbOfDecimalPlaces(static_cast<int>(num(numbers, "decimalPlacesLeft")),
                                           static_cast<int>(num(numbers, "decimalPlacesRight")));
                if (numbers.hasKey("grade")) {
                    data->SetClassOfTolerance(num(numbers, "hole") != 0,
                        static_cast<XCAFDimTolObjects_DimensionFormVariance>(
                            num(numbers, "formVariance")),
                        static_cast<XCAFDimTolObjects_DimensionGrade>(num(numbers, "grade")));
                }
                for (int i = 1; i <= modifiers; ++i) {
                    data->AddModifier(static_cast<XCAFDimTolObjects_DimensionModif>(
                        num(numbers, ("modifierCode" + std::to_string(i)).c_str())));
                }
                const Py::Dict extra(record.getItem("strings"));
                for (int i = 0; extra.hasKey("description" + std::to_string(i)); ++i) {
                    data->AddDescription(name(extra, ("description" + std::to_string(i)).c_str()),
                        name(extra, ("descriptionName" + std::to_string(i)).c_str()));
                }
                const auto label = tool->AddDimension();
                XCAFDoc_Dimension::Set(label)->SetObject(data);
                const std::string marker = "FreeCAD_PMI_" + std::to_string(semanticNames.size());
                semanticNames.emplace(marker, str(record, "name"));
                // OCCT carries the suffix of this label name into each target aspect.
                TDataStd_Name::Set(label, TCollection_ExtendedString(("PMI " + marker).c_str()));
                tool->SetDimension(first, second, label);
            }
            else if (category == "Tolerance") {
                Handle(XCAFDimTolObjects_GeomToleranceObject) data
                    = new XCAFDimTolObjects_GeomToleranceObject;
                data->SetType(static_cast<XCAFDimTolObjects_GeomToleranceType>(type));
                data->SetSemanticName(name(record, "name"));
                data->SetValue(num(record, "value") * lengthFactor);
                data->SetTypeOfValue(static_cast<XCAFDimTolObjects_GeomToleranceTypeValue>(
                    num(numbers, "valueTypeCode")));
                data->SetMaterialRequirementModifier(
                    static_cast<XCAFDimTolObjects_GeomToleranceMatReqModif>(
                        num(numbers, "materialRequirementCode")));
                const auto zone = static_cast<XCAFDimTolObjects_GeomToleranceZoneModif>(
                    num(numbers, "zoneModifierCode"));
                const double zoneFactor = zone == XCAFDimTolObjects_GeomToleranceZoneModif_Projected
                    ? lengthFactor : zone == XCAFDimTolObjects_GeomToleranceZoneModif_Runout
                    ? std::numbers::pi / 180.0 : 1.0;
                data->SetZoneModifier(zone);
                data->SetValueOfZoneModifier(num(numbers, "zoneModifierValue") * zoneFactor);
                data->SetMaxValueModifier(num(numbers, "maxValueModifier") * lengthFactor);
                for (int i = 1; i <= modifiers; ++i) {
                    data->AddModifier(static_cast<XCAFDimTolObjects_GeomToleranceModif>(
                        num(numbers, ("modifierCode" + std::to_string(i)).c_str())));
                }
                const auto label = tool->AddGeomTolerance();
                XCAFDoc_GeomTolerance::Set(label)->SetObject(data);
                const std::string marker = "FreeCAD_PMI_" + std::to_string(semanticNames.size());
                semanticNames.emplace(marker, str(record, "name"));
                if (zone == XCAFDimTolObjects_GeomToleranceZoneModif_Projected) {
                    projectedZones.emplace(marker, std::make_pair(
                        data->GetValueOfZoneModifier(), data->GetTypeOfValue()));
                    // Emit the projected definition after transfer: older OCCT
                    // writers otherwise omit it or produce a runout zone instead.
                    data->SetTypeOfValue(XCAFDimTolObjects_GeomToleranceTypeValue_None);
                    data->SetZoneModifier(XCAFDimTolObjects_GeomToleranceZoneModif_None);
                    XCAFDoc_GeomTolerance::Set(label)->SetObject(data);
                }
                // OCCT carries the suffix of this label name into each target aspect.
                TDataStd_Name::Set(label, TCollection_ExtendedString(("PMI " + marker).c_str()));
                tool->SetGeomTolerance(first, label);
                for (const auto& datum : datumObjects) {
                    tool->SetDatumToGeomTol(datums.at(object(datum)), label);
                }
            }
        }
    }
    for (const auto& value : candidates) {
        Py::Dict item(value);
        report.append(result(item, reasons[object(item.getItem("object"))]));
    }
}

void StepPmiExport::complete(const Handle(StepData_StepModel)& model) const
{
    // OCCT writes generic MEASURE_WITH_UNIT bounds, which its own reader does
    // not scale. Use the standard typed measures so non-mm/angle units survive.
    const auto typedMeasure = [&](const Handle(Standard_Transient)& value) {
        const auto measure = Handle(StepBasic_MeasureWithUnit)::DownCast(value);
        if (measure.IsNull()) {
            throw std::runtime_error("writer emitted an unsupported tolerance measure");
        }
        Handle(StepBasic_MeasureWithUnit) typed;
        Handle(StepBasic_MeasureValueMember) member = new StepBasic_MeasureValueMember;
        STEPConstruct_UnitContext context;
        if (measure->UnitComponent().NamedUnit().IsNull()
            || context.ComputeFactors(measure->UnitComponent().NamedUnit()) != 0) {
            throw std::runtime_error("writer emitted an unknown tolerance unit");
        }
        if (context.LengthDone() && !context.PlaneAngleDone()) {
            typed = new StepBasic_LengthMeasureWithUnit;
            member->SetName("LENGTH_MEASURE");
        }
        else if (context.PlaneAngleDone() && !context.LengthDone()) {
            typed = new StepBasic_PlaneAngleMeasureWithUnit;
            member->SetName("PLANE_ANGLE_MEASURE");
        }
        else {
            throw std::runtime_error("writer emitted an unknown tolerance unit type");
        }
        member->SetReal(measure->ValueComponent());
        typed->Init(member, measure->UnitComponent());
        model->ReplaceEntity(model->Number(measure), typed);
        return typed;
    };
    for (int i = 1; i <= model->NbEntities(); ++i) {
        const auto bounds = Handle(StepShape_ToleranceValue)::DownCast(model->Value(i));
        if (!bounds.IsNull()) {
            bounds->SetLowerBound(typedMeasure(bounds->LowerBound()));
            bounds->SetUpperBound(typedMeasure(bounds->UpperBound()));
        }
    }
    std::set<std::string> emitted;
    const auto semanticName = [&](const Handle(StepRepr_ShapeAspect)& aspect) {
        if (aspect.IsNull() || aspect->Name().IsNull()) {
            throw std::runtime_error("writer omitted a semantic PMI target");
        }
        const std::string marker = aspect->Name()->ToCString();
        const auto found = semanticNames.find(marker);
        if (found == semanticNames.end()) {
            throw std::runtime_error("writer lost a semantic PMI association");
        }
        emitted.insert(marker);
        return new TCollection_HAsciiString(found->second.c_str());
    };
    // Resolve names through exact marker-bearing target aspects, never entity order
    // or geometric similarity. OCCT otherwise discards the semantic object's name.
    for (int i = 1; i <= model->NbEntities(); ++i) {
        const auto entity = model->Value(i);
        if (const auto location = Handle(StepShape_DimensionalLocation)::DownCast(entity)) {
            location->SetName(semanticName(location->RelatingShapeAspect()));
        }
        else if (const auto size = Handle(StepShape_DimensionalSize)::DownCast(entity)) {
            size->SetName(semanticName(size->AppliesTo()));
        }
        else if (const auto tolerance = Handle(StepDimTol_GeometricTolerance)::DownCast(entity)) {
            const auto aspect = tolerance->TolerancedShapeAspect().ShapeAspect();
            const auto projection = projectedZones.find(aspect->Name()->ToCString());
            if (projection != projectedZones.end()) {
                Handle(StepDimTol_ToleranceZoneForm) form = new StepDimTol_ToleranceZoneForm;
                const auto type = static_cast<XCAFDimTolObjects_GeomToleranceTypeValue>(
                    projection->second.second);
                form->Init(STEPCAFControl_GDTProperty::GetTolValueType(type));
                Handle(StepDimTol_HArray1OfToleranceZoneTarget) targets
                    = new StepDimTol_HArray1OfToleranceZoneTarget(1, 1);
                StepDimTol_ToleranceZoneTarget target;
                target.SetValue(tolerance);
                targets->SetValue(1, target);
                Handle(StepDimTol_ToleranceZone) zone = new StepDimTol_ToleranceZone;
                zone->Init(new TCollection_HAsciiString(), new TCollection_HAsciiString(),
                           aspect->OfShape(), StepData_LFalse, targets, form);
                Handle(StepBasic_MeasureValueMember) value = new StepBasic_MeasureValueMember;
                value->SetName("LENGTH_MEASURE");
                value->SetReal(projection->second.first);
                Handle(StepBasic_LengthMeasureWithUnit) length
                    = new StepBasic_LengthMeasureWithUnit;
#if OCC_VERSION_HEX >= 0x080000
                auto magnitude = Handle(StepBasic_MeasureWithUnit)::DownCast(tolerance->Magnitude());
                auto combined = Handle(StepRepr_ReprItemAndMeasureWithUnit)
                    ::DownCast(tolerance->Magnitude());
                if (!combined.IsNull()) { magnitude = combined->GetMeasureWithUnit(); }
                if (magnitude.IsNull()) {
                    throw std::runtime_error("writer emitted an unsupported tolerance magnitude");
                }
                length->Init(value, magnitude->UnitComponent());
#else
                length->Init(value, tolerance->Magnitude()->UnitComponent());
#endif
                Handle(StepDimTol_ProjectedZoneDefinition) definition
                    = new StepDimTol_ProjectedZoneDefinition;
                definition->Init(zone, nullptr, aspect, length);
                // OCCT 7.6 Share() omits the form and projection measure.
                model->AddWithRefs(form);
                model->AddWithRefs(length);
                model->AddWithRefs(definition);
            }
            tolerance->SetName(semanticName(tolerance->TolerancedShapeAspect().ShapeAspect()));
        }
    }
    if (emitted.size() != semanticNames.size()) {
        throw std::runtime_error("writer cannot express all semantic PMI records");
    }
    for (int i = 1; i <= model->NbEntities(); ++i) {
        const auto aspect = Handle(StepRepr_ShapeAspect)::DownCast(model->Value(i));
        if (!aspect.IsNull() && !aspect->Name().IsNull()
            && semanticNames.count(aspect->Name()->ToCString())) {
            aspect->SetName(new TCollection_HAsciiString());
        }
    }
}

Py::Object StepPmiExport::write(const Base::FileInfo& file,
                              const Handle(TDocStd_Document)& document,
                              const std::vector<App::DocumentObject*>& objects,
                              StepAttributesExport* attributes)
{
    if (attributes) { attributes->prepare(objects); }
    if (!enabled) {
        WriterStep(file).write(document, nullptr, attributes);
        return attributes ? attributes->report(Py::None()) : Py::None();
    }
    Py::List owners;
    for (auto owner : objects) {
        owners.append(Py::asObject(owner->getPyObject()));
    }
    Py::Object module = Py::None();
    Py::List report;
    Py::List candidates;
    Py::Tuple args(1);
    args[0] = owners;
    const std::string originalScheme = Part::Interface::writeStepScheme();
    document->SetUndoLimit(1);
    document->NewCommand();
    try {
        if (failed) {
            throw std::runtime_error("export association capture failed");
        }
        PyObject* helper = PyImport_ImportModule("StepPmiExport");
        if (!helper) {
            throw Py::Exception();
        }
        module = Py::Module(helper, true);
        candidates = Py::List(Py::Callable(module.getAttr("prepare")).apply(args));
        populate(document, candidates, report);
        const bool hasPmi = std::any_of(report.begin(), report.end(), [](const Py::Object& row) {
                return str(Py::Dict(row), "status") == "written";
            });
        if (hasPmi) {
            Part::Interface::writeStepScheme("AP242DIS");
        }
        else {
            document->AbortCommand();
        }
        WriterStep(file).write(document, hasPmi ? this : nullptr, attributes);
        if (hasPmi) {
            document->CommitCommand();
        }
    }
    catch (const WriterStep::RecoveryError&) {
        document->AbortCommand();
        Part::Interface::writeStepScheme(originalScheme.c_str());
        throw;
    }
    catch (...) {
        document->AbortCommand();
        Part::Interface::writeStepScheme(originalScheme.c_str());
        std::string reason = "PMI writing failed; geometry kept";
        try {
            throw;
        }
        catch (const Standard_Failure& error) {
            reason += ": " + std::string(error.GetMessageString());
        }
        catch (const std::exception& error) {
            reason += ": " + std::string(error.what());
        }
        catch (...) {
            // Python's active error is cleared below before the fallback writer.
        }
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        report = Py::List();
        // Even an unavailable Python helper must leave geometry export usable.
        std::set<App::Document*> documents;
        for (auto owner : objects) {
            if (documents.insert(owner->getDocument()).second) {
                for (auto candidate : owner->getDocument()->getObjects()) {
                    if (candidate->getPropertyByName("Category")
                        && candidate->getPropertyByName("RawMetadata")) {
                        Py::Dict item;
                        item["object"] = Py::asObject(candidate->getPyObject());
                        report.append(result(item, reason));
                    }
                }
            }
        }
        WriterStep(file).write(document, nullptr, attributes);
    }
    Part::Interface::writeStepScheme(originalScheme.c_str());
    for (const auto& value : report) {
        const Py::Dict row(value);
        if (str(row, "status") == "skipped") {
            Base::Console().warning("STEP PMI export {}: {}\n", str(row, "object"),
                                    str(row, "reason"));
        }
    }
    return attributes ? attributes->report(report) : report;
}
}  // namespace Import
