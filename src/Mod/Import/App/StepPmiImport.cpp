// SPDX-License-Identifier: LGPL-2.1-or-later
#include "StepPmiImport.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>
#include <CXX/Objects.hxx>
#include <TDF_Tool.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
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
#include <Base/Console.h>
#include <Base/FileInfo.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Mod/Part/App/PartFeature.h>

namespace Import
{
namespace
{
std::string entry(const TDF_Label& label)
{
    TCollection_AsciiString text;
    if (!label.IsNull()) {
        TDF_Tool::Entry(label, text);
    }
    return text.ToCString();
}

std::string text(const Handle(TCollection_HAsciiString) & value)
{
    return value.IsNull() ? std::string() : std::string(value->ToCString());
}

using EnumName = std::pair<int, const char*>;

const std::vector<EnumName> dimensionTypeNames {
    {XCAFDimTolObjects_DimensionType_Location_None, "Location_None"},
    {XCAFDimTolObjects_DimensionType_Location_CurvedDistance, "Location_CurvedDistance"},
    {XCAFDimTolObjects_DimensionType_Location_LinearDistance, "Location_LinearDistance"},
    {XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromCenterToOuter,
     "Location_LinearDistance_FromCenterToOuter"},
    {XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromCenterToInner,
     "Location_LinearDistance_FromCenterToInner"},
    {XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromOuterToCenter,
     "Location_LinearDistance_FromOuterToCenter"},
    {XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromOuterToOuter,
     "Location_LinearDistance_FromOuterToOuter"},
    {XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromOuterToInner,
     "Location_LinearDistance_FromOuterToInner"},
    {XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromInnerToCenter,
     "Location_LinearDistance_FromInnerToCenter"},
    {XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromInnerToOuter,
     "Location_LinearDistance_FromInnerToOuter"},
    {XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromInnerToInner,
     "Location_LinearDistance_FromInnerToInner"},
    {XCAFDimTolObjects_DimensionType_Location_Angular, "Location_Angular"},
    {XCAFDimTolObjects_DimensionType_Location_Oriented, "Location_Oriented"},
    {XCAFDimTolObjects_DimensionType_Location_WithPath, "Location_WithPath"},
    {XCAFDimTolObjects_DimensionType_Size_CurveLength, "Size_CurveLength"},
    {XCAFDimTolObjects_DimensionType_Size_Diameter, "Size_Diameter"},
    {XCAFDimTolObjects_DimensionType_Size_SphericalDiameter, "Size_SphericalDiameter"},
    {XCAFDimTolObjects_DimensionType_Size_Radius, "Size_Radius"},
    {XCAFDimTolObjects_DimensionType_Size_SphericalRadius, "Size_SphericalRadius"},
    {XCAFDimTolObjects_DimensionType_Size_ToroidalMinorDiameter, "Size_ToroidalMinorDiameter"},
    {XCAFDimTolObjects_DimensionType_Size_ToroidalMajorDiameter, "Size_ToroidalMajorDiameter"},
    {XCAFDimTolObjects_DimensionType_Size_ToroidalMinorRadius, "Size_ToroidalMinorRadius"},
    {XCAFDimTolObjects_DimensionType_Size_ToroidalMajorRadius, "Size_ToroidalMajorRadius"},
    {XCAFDimTolObjects_DimensionType_Size_ToroidalHighMajorDiameter, "Size_ToroidalHighMajorDiameter"},
    {XCAFDimTolObjects_DimensionType_Size_ToroidalLowMajorDiameter, "Size_ToroidalLowMajorDiameter"},
    {XCAFDimTolObjects_DimensionType_Size_ToroidalHighMajorRadius, "Size_ToroidalHighMajorRadius"},
    {XCAFDimTolObjects_DimensionType_Size_ToroidalLowMajorRadius, "Size_ToroidalLowMajorRadius"},
    {XCAFDimTolObjects_DimensionType_Size_Thickness, "Size_Thickness"},
    {XCAFDimTolObjects_DimensionType_Size_Angular, "Size_Angular"},
    {XCAFDimTolObjects_DimensionType_Size_WithPath, "Size_WithPath"},
    {XCAFDimTolObjects_DimensionType_CommonLabel, "CommonLabel"},
    {XCAFDimTolObjects_DimensionType_DimensionPresentation, "DimensionPresentation"},
};

const std::vector<EnumName> geomToleranceTypeNames {
    {XCAFDimTolObjects_GeomToleranceType_None, "None"},
    {XCAFDimTolObjects_GeomToleranceType_Angularity, "Angularity"},
    {XCAFDimTolObjects_GeomToleranceType_CircularRunout, "CircularRunout"},
    {XCAFDimTolObjects_GeomToleranceType_CircularityOrRoundness, "CircularityOrRoundness"},
    {XCAFDimTolObjects_GeomToleranceType_Coaxiality, "Coaxiality"},
    {XCAFDimTolObjects_GeomToleranceType_Concentricity, "Concentricity"},
    {XCAFDimTolObjects_GeomToleranceType_Cylindricity, "Cylindricity"},
    {XCAFDimTolObjects_GeomToleranceType_Flatness, "Flatness"},
    {XCAFDimTolObjects_GeomToleranceType_Parallelism, "Parallelism"},
    {XCAFDimTolObjects_GeomToleranceType_Perpendicularity, "Perpendicularity"},
    {XCAFDimTolObjects_GeomToleranceType_Position, "Position"},
    {XCAFDimTolObjects_GeomToleranceType_ProfileOfLine, "ProfileOfLine"},
    {XCAFDimTolObjects_GeomToleranceType_ProfileOfSurface, "ProfileOfSurface"},
    {XCAFDimTolObjects_GeomToleranceType_Straightness, "Straightness"},
    {XCAFDimTolObjects_GeomToleranceType_Symmetry, "Symmetry"},
    {XCAFDimTolObjects_GeomToleranceType_TotalRunout, "TotalRunout"},
};

const std::vector<EnumName> dimensionModifNames {
    {XCAFDimTolObjects_DimensionModif_ControlledRadius, "ControlledRadius"},
    {XCAFDimTolObjects_DimensionModif_Square, "Square"},
    {XCAFDimTolObjects_DimensionModif_StatisticalTolerance, "StatisticalTolerance"},
    {XCAFDimTolObjects_DimensionModif_ContinuousFeature, "ContinuousFeature"},
    {XCAFDimTolObjects_DimensionModif_TwoPointSize, "TwoPointSize"},
    {XCAFDimTolObjects_DimensionModif_LocalSizeDefinedBySphere, "LocalSizeDefinedBySphere"},
    {XCAFDimTolObjects_DimensionModif_LeastSquaresAssociationCriterion,
     "LeastSquaresAssociationCriterion"},
    {XCAFDimTolObjects_DimensionModif_MaximumInscribedAssociation, "MaximumInscribedAssociation"},
    {XCAFDimTolObjects_DimensionModif_MinimumCircumscribedAssociation,
     "MinimumCircumscribedAssociation"},
    {XCAFDimTolObjects_DimensionModif_CircumferenceDiameter, "CircumferenceDiameter"},
    {XCAFDimTolObjects_DimensionModif_AreaDiameter, "AreaDiameter"},
    {XCAFDimTolObjects_DimensionModif_VolumeDiameter, "VolumeDiameter"},
    {XCAFDimTolObjects_DimensionModif_MaximumSize, "MaximumSize"},
    {XCAFDimTolObjects_DimensionModif_MinimumSize, "MinimumSize"},
    {XCAFDimTolObjects_DimensionModif_AverageSize, "AverageSize"},
    {XCAFDimTolObjects_DimensionModif_MedianSize, "MedianSize"},
    {XCAFDimTolObjects_DimensionModif_MidRangeSize, "MidRangeSize"},
    {XCAFDimTolObjects_DimensionModif_RangeOfSizes, "RangeOfSizes"},
    {XCAFDimTolObjects_DimensionModif_AnyRestrictedPortionOfFeature, "AnyRestrictedPortionOfFeature"},
    {XCAFDimTolObjects_DimensionModif_AnyCrossSection, "AnyCrossSection"},
    {XCAFDimTolObjects_DimensionModif_SpecificFixedCrossSection, "SpecificFixedCrossSection"},
    {XCAFDimTolObjects_DimensionModif_CommonTolerance, "CommonTolerance"},
    {XCAFDimTolObjects_DimensionModif_FreeStateCondition, "FreeStateCondition"},
    {XCAFDimTolObjects_DimensionModif_Between, "Between"},
};

const std::vector<EnumName> geomToleranceModifNames {
    {XCAFDimTolObjects_GeomToleranceModif_Any_Cross_Section, "Any_Cross_Section"},
    {XCAFDimTolObjects_GeomToleranceModif_Common_Zone, "Common_Zone"},
    {XCAFDimTolObjects_GeomToleranceModif_Each_Radial_Element, "Each_Radial_Element"},
    {XCAFDimTolObjects_GeomToleranceModif_Free_State, "Free_State"},
    {XCAFDimTolObjects_GeomToleranceModif_Least_Material_Requirement, "Least_Material_Requirement"},
    {XCAFDimTolObjects_GeomToleranceModif_Line_Element, "Line_Element"},
    {XCAFDimTolObjects_GeomToleranceModif_Major_Diameter, "Major_Diameter"},
    {XCAFDimTolObjects_GeomToleranceModif_Maximum_Material_Requirement,
     "Maximum_Material_Requirement"},
    {XCAFDimTolObjects_GeomToleranceModif_Minor_Diameter, "Minor_Diameter"},
    {XCAFDimTolObjects_GeomToleranceModif_Not_Convex, "Not_Convex"},
    {XCAFDimTolObjects_GeomToleranceModif_Pitch_Diameter, "Pitch_Diameter"},
    {XCAFDimTolObjects_GeomToleranceModif_Reciprocity_Requirement, "Reciprocity_Requirement"},
    {XCAFDimTolObjects_GeomToleranceModif_Separate_Requirement, "Separate_Requirement"},
    {XCAFDimTolObjects_GeomToleranceModif_Statistical_Tolerance, "Statistical_Tolerance"},
    {XCAFDimTolObjects_GeomToleranceModif_Tangent_Plane, "Tangent_Plane"},
    {XCAFDimTolObjects_GeomToleranceModif_All_Around, "All_Around"},
    {XCAFDimTolObjects_GeomToleranceModif_All_Over, "All_Over"},
};

const std::vector<EnumName> datumSingleModifNames {
    {XCAFDimTolObjects_DatumSingleModif_AnyCrossSection, "AnyCrossSection"},
    {XCAFDimTolObjects_DatumSingleModif_Any_LongitudinalSection, "Any_LongitudinalSection"},
    {XCAFDimTolObjects_DatumSingleModif_Basic, "Basic"},
    {XCAFDimTolObjects_DatumSingleModif_ContactingFeature, "ContactingFeature"},
    {XCAFDimTolObjects_DatumSingleModif_DegreeOfFreedomConstraintU, "DegreeOfFreedomConstraintU"},
    {XCAFDimTolObjects_DatumSingleModif_DegreeOfFreedomConstraintV, "DegreeOfFreedomConstraintV"},
    {XCAFDimTolObjects_DatumSingleModif_DegreeOfFreedomConstraintW, "DegreeOfFreedomConstraintW"},
    {XCAFDimTolObjects_DatumSingleModif_DegreeOfFreedomConstraintX, "DegreeOfFreedomConstraintX"},
    {XCAFDimTolObjects_DatumSingleModif_DegreeOfFreedomConstraintY, "DegreeOfFreedomConstraintY"},
    {XCAFDimTolObjects_DatumSingleModif_DegreeOfFreedomConstraintZ, "DegreeOfFreedomConstraintZ"},
    {XCAFDimTolObjects_DatumSingleModif_DistanceVariable, "DistanceVariable"},
    {XCAFDimTolObjects_DatumSingleModif_FreeState, "FreeState"},
    {XCAFDimTolObjects_DatumSingleModif_LeastMaterialRequirement, "LeastMaterialRequirement"},
    {XCAFDimTolObjects_DatumSingleModif_Line, "Line"},
    {XCAFDimTolObjects_DatumSingleModif_MajorDiameter, "MajorDiameter"},
    {XCAFDimTolObjects_DatumSingleModif_MaximumMaterialRequirement, "MaximumMaterialRequirement"},
    {XCAFDimTolObjects_DatumSingleModif_MinorDiameter, "MinorDiameter"},
    {XCAFDimTolObjects_DatumSingleModif_Orientation, "Orientation"},
    {XCAFDimTolObjects_DatumSingleModif_PitchDiameter, "PitchDiameter"},
    {XCAFDimTolObjects_DatumSingleModif_Plane, "Plane"},
    {XCAFDimTolObjects_DatumSingleModif_Point, "Point"},
    {XCAFDimTolObjects_DatumSingleModif_Translation, "Translation"},
};

const std::vector<EnumName> datumModifWithValueNames {
    {XCAFDimTolObjects_DatumModifWithValue_None, "None"},
    {XCAFDimTolObjects_DatumModifWithValue_CircularOrCylindrical, "CircularOrCylindrical"},
    {XCAFDimTolObjects_DatumModifWithValue_Distance, "Distance"},
    {XCAFDimTolObjects_DatumModifWithValue_Projected, "Projected"},
    {XCAFDimTolObjects_DatumModifWithValue_Spherical, "Spherical"},
};

const std::vector<EnumName> geomToleranceTypeValueNames {
    {XCAFDimTolObjects_GeomToleranceTypeValue_None, "None"},
    {XCAFDimTolObjects_GeomToleranceTypeValue_Diameter, "Diameter"},
    {XCAFDimTolObjects_GeomToleranceTypeValue_SphericalDiameter, "SphericalDiameter"},
};

const std::vector<EnumName> geomToleranceMatReqModifNames {
    {XCAFDimTolObjects_GeomToleranceMatReqModif_None, "None"},
    {XCAFDimTolObjects_GeomToleranceMatReqModif_M, "M"},
    {XCAFDimTolObjects_GeomToleranceMatReqModif_L, "L"},
};

const std::vector<EnumName> geomToleranceZoneModifNames {
    {XCAFDimTolObjects_GeomToleranceZoneModif_None, "None"},
    {XCAFDimTolObjects_GeomToleranceZoneModif_Projected, "Projected"},
    {XCAFDimTolObjects_GeomToleranceZoneModif_Runout, "Runout"},
    {XCAFDimTolObjects_GeomToleranceZoneModif_NonUniform, "NonUniform"},
};

const std::vector<EnumName> datumTargetTypeNames {
    {XCAFDimTolObjects_DatumTargetType_Point, "Point"},
    {XCAFDimTolObjects_DatumTargetType_Line, "Line"},
    {XCAFDimTolObjects_DatumTargetType_Rectangle, "Rectangle"},
    {XCAFDimTolObjects_DatumTargetType_Circle, "Circle"},
    {XCAFDimTolObjects_DatumTargetType_Area, "Area"},
};

std::string enumName(
    int value,
    const std::vector<EnumName>& names,
    const std::string& field,
    StepPmiRecord& record
)
{
    for (const auto& item : names) {
        if (item.first == value) {
            return item.second;
        }
    }
    const auto unknown = "Unknown " + field + " (" + std::to_string(value) + ")";
    record.warnings.push_back(unknown);
    return unknown;
}

template<class Sequence>
void modifiers(const Sequence& sequence, const std::vector<EnumName>& names, StepPmiRecord& record)
{
    for (int i = 1; i <= sequence.Length(); ++i) {
        const int value = sequence.Value(i);
        record.modifiers.push_back(enumName(value, names, "modifier", record));
        record.numbers["modifierCode" + std::to_string(i)] = value;
    }
}

Py::List strings(const std::vector<std::string>& values)
{
    Py::List result;
    for (const auto& value : values) {
        result.append(Py::String(value));
    }
    return result;
}

Py::Dict toPython(const StepPmiRecord& record)
{
    Py::Dict data;
    data["category"] = Py::String(record.category);
    data["sourceId"] = Py::String(record.sourceId);
    data["name"] = Py::String(record.name);
    data["kind"] = Py::String(record.kind);
    data["unit"] = Py::String(record.unit);
    data["type"] = Py::Long(record.type);
    data["value"] = Py::Float(record.value);
    data["hasValue"] = Py::Boolean(record.hasValue);
    data["hasBounds"] = Py::Boolean(record.hasBounds);
    data["hasDeviations"] = Py::Boolean(record.hasDeviations);
    data["lower"] = Py::Float(record.lower);
    data["upper"] = Py::Float(record.upper);
    data["datumPosition"] = Py::Long(record.datumPosition);
    data["datumName"] = Py::String(record.datumName);
    data["modifiers"] = strings(record.modifiers);
    data["datumIds"] = strings(record.datumIds);
    data["warnings"] = strings(record.warnings);
    Py::List values;
    for (double value : record.values) {
        values.append(Py::Float(value));
    }
    data["values"] = values;
    Py::Dict numbers;
    for (const auto& item : record.numbers) {
        numbers[item.first] = Py::Float(item.second);
    }
    data["numbers"] = numbers;
    Py::Dict extra;
    for (const auto& item : record.strings) {
        extra[item.first] = Py::String(item.second);
    }
    data["strings"] = extra;
    Py::List references;
    for (const auto& ref : record.references) {
        Py::Dict reference;
        reference["sourceId"] = Py::String(ref.sourceId);
        reference["role"] = Py::String(ref.role);
        reference["objectName"] = Py::String(ref.objectName);
        reference["subElement"] = Py::String(ref.subElement);
        reference["status"] = Py::String(ref.status);
        references.append(reference);
    }
    data["references"] = references;
    return data;
}
}  // namespace

bool StepPmiImport::enabled(const Base::FileInfo& file, int override)
{
    return file.hasExtension({"stp", "step"})
        && (override < 0
                ? App::GetApplication().GetParameterGroupByPath(
                      "User parameter:BaseApp/Preferences/Mod/Import")
                      ->GetBool("ImportSemanticPMI", false)
                : override != 0);
}

void StepPmiImport::finish(
    const Base::FileInfo& file, const Handle(TDocStd_Document)& source,
    App::Document* target, bool supportedAssociation) const
{
    // Geometry already exists. Roll back only objects added by materialisation,
    // including a helper that raises after creating part of the PMI group.
    std::set<std::string> existing;
    for (auto object : target->getObjects()) {
        existing.insert(object->getNameInDocument());
    }
    try {
        materialize(target, extract(source, target, supportedAssociation));
        return;
    }
    catch (...) {
        // PyCXX exceptions leave the Python error indicator set.
        PyErr_Clear();
    }
    std::vector<std::string> added;
    for (auto object : target->getObjects()) {
        if (!existing.count(object->getNameInDocument())) {
            added.emplace_back(object->getNameInDocument());
        }
    }
    for (auto it = added.rbegin(); it != added.rend(); ++it) {
        target->removeObject(it->c_str());
    }
    Base::Console().warning("STEP PMI: could not import semantic PMI from '{}'; geometry kept.\n",
                            file.filePath());
}

ShapeImportObserver StepPmiImport::observer()
{
    return [this](const TDF_Label& label, const TopoDS_Shape& shape, App::DocumentObject* object) {
        if (label.IsNull() || shape.IsNull() || !object) {
            return;
        }
        // The legacy traversal also visits auxiliary labels (views/clips). Exclude those.
        const auto shapeTable = XCAFDoc_DocumentTool::ShapesLabel(label);
        auto parent = label;
        while (!parent.IsNull() && parent != shapeTable) {
            parent = parent.Father();
        }
        if (parent.IsNull()) {
            return;
        }
        const auto id = entry(label);
        for (const auto& existing : associations) {
            if (existing.label == id && existing.document == object->getDocument()
                && existing.objectName == object->getNameInDocument()
                && existing.shape.IsSame(shape)) {
                return;
            }
        }
        associations.push_back(
            {id,
             shape,
             object->getDocument(),
             object->getNameInDocument(),
             XCAFDoc_ShapeTool::IsReference(label)}
        );
    };
}

StepPmiReference StepPmiImport::resolve(
    const TDF_Label& label,
    const std::string& role,
    App::Document* target,
    bool supported
) const
{
    StepPmiReference result;
    result.sourceId = entry(label);
    result.role = role;
    result.status = supported ? "unresolved reference" : "unsupported association";
    if (!supported) {
        return result;
    }
    const auto shape = XCAFDoc_ShapeTool::GetShape(label);
    if (shape.IsNull()) {
        return result;
    }
    // Match identity, never proximity or a guessed face number. Verify the created topology too.
    std::set<std::pair<std::string, std::string>> matches;
    const char* names[] = {"Compound", "CompSolid", "Solid", "Shell", "Face", "Wire", "Edge", "Vertex"};
    const auto type = shape.ShapeType();
    bool occurrence = false;
    for (const auto& candidate : associations) {
        if (candidate.document != target) {
            continue;
        }
        auto object = target->getObject(candidate.objectName.c_str());
        if (!object) {
            continue;
        }
        TopTools_IndexedMapOfShape originalMap;
        TopExp::MapShapes(candidate.shape, type, originalMap);
        if (candidate.occurrence) {
            // A definition-level label does not select a placed assembly occurrence.
            // Partner checks here only reject ambiguity; they never establish a reference.
            for (int i = 1; i <= originalMap.Extent(); ++i) {
                if (originalMap(i).IsPartner(shape)) {
                    occurrence = true;
                    break;
                }
            }
        }
        if (!candidate.shape.IsSame(shape) && !originalMap.Contains(shape)) {
            continue;
        }
        auto feature = dynamic_cast<Part::Feature*>(object);
        if (!feature) {
            // Links identify occurrences. Part A cannot choose an occurrence from a definition.
            occurrence = true;
            continue;
        }
        const auto& actual = feature->Shape.getValue();
        if (actual.IsNull()) {
            continue;
        }
        if (candidate.shape.IsSame(shape) && actual.IsPartner(shape)) {
            matches.emplace(candidate.objectName, "");
            continue;
        }
        TopTools_IndexedMapOfShape actualMap;
        const int index = originalMap.FindIndex(shape);
        if (!index || type >= TopAbs_SHAPE) {
            continue;
        }
        TopExp::MapShapes(actual, type, actualMap);
        const auto delta = actual.Location() * candidate.shape.Location().Inverted();
        if (index <= actualMap.Extent() && actualMap(index).IsSame(shape.Moved(delta))) {
            matches.emplace(candidate.objectName, std::string(names[type]) + std::to_string(index));
        }
    }
    if (occurrence) {
        result.status = "ambiguous association";
    }
    else if (matches.size() == 1) {
        result.objectName = matches.begin()->first;
        result.subElement = matches.begin()->second;
        result.status = "resolved";
    }
    else if (matches.size() > 1) {
        result.status = "ambiguous association";
    }
    return result;
}

std::vector<StepPmiRecord> StepPmiImport::extract(
    const Handle(TDocStd_Document) & document,
    App::Document* target,
    bool supportedAssociation
) const
{
    if (sourceWarningsFailed) {
        throw std::runtime_error("Could not extract STEP PMI reader diagnostics");
    }
    std::vector<StepPmiRecord> records;
    const auto tool = XCAFDoc_DocumentTool::DimTolTool(document->Main());
    auto references = [&](const TDF_Label& label, StepPmiRecord& record) {
        TDF_LabelSequence first, second;
        tool->GetRefShapeLabel(label, first, second);
        for (int i = 1; i <= first.Length(); ++i) {
            record.references.push_back(resolve(first.Value(i), "first", target, supportedAssociation)
            );
        }
        for (int i = 1; i <= second.Length(); ++i) {
            record.references.push_back(
                resolve(second.Value(i), "second", target, supportedAssociation)
            );
        }
        if (!supportedAssociation) {
            record.warnings.emplace_back("unsupported association: merge or multi-document import");
        }
    };
    TDF_LabelSequence labels;
    tool->GetDimensionLabels(labels);
    for (int i = 1; i <= labels.Length(); ++i) {
        const auto label = labels.Value(i);
        StepPmiRecord record;
        record.category = "Dimension";
        record.sourceId = entry(label);
        Handle(XCAFDoc_Dimension) attribute;
        if (label.FindAttribute(XCAFDoc_Dimension::GetID(), attribute)) {
            const auto object = attribute->GetObject();
            record.type = object->GetType();
            record.kind = enumName(record.type, dimensionTypeNames, "dimension type", record);
            record.name = text(object->GetSemanticName());
            // OCCT STEPCAF normalises length measures to mm and angular measures to degrees.
            // Geometric angularity tolerance below is still a linear tolerance zone, in mm.
            const bool angular = record.type == XCAFDimTolObjects_DimensionType_Location_Angular
                || record.type == XCAFDimTolObjects_DimensionType_Size_Angular;
            record.unit = angular ? "deg" : "mm";
            if (record.type <= XCAFDimTolObjects_DimensionType_Location_None
                || record.type >= XCAFDimTolObjects_DimensionType_CommonLabel) {
                record.unit.clear();
            }
            const auto values = object->GetValues();
            if (!values.IsNull()) {
                for (int j = values->Lower(); j <= values->Upper(); ++j) {
                    record.values.push_back(values->Value(j));
                }
                record.hasValue = !record.values.empty();
                if (record.hasValue) {
                    record.value = object->GetValue();
                }
                record.hasBounds = object->IsDimWithRange();
                record.hasDeviations = object->IsDimWithPlusMinusTolerance();
                if (record.hasBounds) {
                    record.lower = object->GetLowerBound();
                    record.upper = object->GetUpperBound();
                }
                else if (record.hasDeviations) {
                    // XCAF stores the lower tolerance as a magnitude.
                    record.lower = -object->GetLowerTolValue();
                    record.upper = object->GetUpperTolValue();
                }
            }
            modifiers(object->GetModifiers(), dimensionModifNames, record);
            record.numbers["qualifier"] = object->GetQualifier();
            Standard_Integer left = 0, right = 0;
            object->GetNbOfDecimalPlaces(left, right);
            record.numbers["decimalPlacesLeft"] = left;
            record.numbers["decimalPlacesRight"] = right;
            Standard_Boolean hole = false;
            XCAFDimTolObjects_DimensionFormVariance variance;
            XCAFDimTolObjects_DimensionGrade grade;
            if (object->GetClassOfTolerance(hole, variance, grade)) {
                record.numbers["hole"] = hole;
                record.numbers["formVariance"] = variance;
                record.numbers["grade"] = grade;
            }
            for (int j = 0; j < object->NbDescriptions(); ++j) {
                record.strings["descriptionName" + std::to_string(j)] = text(
                    object->GetDescriptionName(j)
                );
                record.strings["description" + std::to_string(j)] = text(object->GetDescription(j));
            }
            if (record.type == XCAFDimTolObjects_DimensionType_Location_None) {
                record.warnings.emplace_back("Unknown dimension type (None)");
                record.unit.clear();
            }
        }
        else {
            record.warnings.emplace_back("Missing dimension data");
        }
        references(label, record);
        records.push_back(std::move(record));
    }
    labels.Clear();
    tool->GetGeomToleranceLabels(labels);
    for (int i = 1; i <= labels.Length(); ++i) {
        const auto label = labels.Value(i);
        StepPmiRecord record;
        record.category = "Tolerance";
        record.sourceId = entry(label);
        record.unit = "mm";
        Handle(XCAFDoc_GeomTolerance) attribute;
        if (label.FindAttribute(XCAFDoc_GeomTolerance::GetID(), attribute)) {
            const auto object = attribute->GetObject();
            record.type = object->GetType();
            record.kind = enumName(record.type, geomToleranceTypeNames, "tolerance type", record);
            record.value = object->GetValue();
            record.values.push_back(record.value);
            record.hasValue = true;
            record.name = text(object->GetSemanticName());
            modifiers(object->GetModifiers(), geomToleranceModifNames, record);
            record.strings["valueType"] = enumName(
                object->GetTypeOfValue(),
                geomToleranceTypeValueNames,
                "tolerance value type",
                record
            );
            record.strings["materialRequirement"] = enumName(
                object->GetMaterialRequirementModifier(),
                geomToleranceMatReqModifNames,
                "material requirement modifier",
                record
            );
            record.strings["zoneModifier"] = enumName(
                object->GetZoneModifier(),
                geomToleranceZoneModifNames,
                "zone modifier",
                record
            );
            record.numbers["valueTypeCode"] = object->GetTypeOfValue();
            record.numbers["materialRequirementCode"] = object->GetMaterialRequirementModifier();
            record.numbers["zoneModifierCode"] = object->GetZoneModifier();
            record.numbers["zoneModifierValue"] = object->GetValueOfZoneModifier();
            record.numbers["maxValueModifier"] = object->GetMaxValueModifier();
            if (record.type == XCAFDimTolObjects_GeomToleranceType_None) {
                record.warnings.emplace_back("Unknown tolerance type (None)");
            }
        }
        else {
            record.warnings.emplace_back("Missing tolerance data");
        }
        TDF_LabelSequence datums;
        tool->GetDatumWithObjectOfTolerLabels(label, datums);
        for (int j = 1; j <= datums.Length(); ++j) {
            record.datumIds.push_back(entry(datums.Value(j)));
        }
        references(label, record);
        records.push_back(std::move(record));
    }
    labels.Clear();
    tool->GetDatumLabels(labels);
    for (int i = 1; i <= labels.Length(); ++i) {
        const auto label = labels.Value(i);
        StepPmiRecord record;
        record.category = "Datum";
        record.kind = "datum";
        record.sourceId = entry(label);
        Handle(XCAFDoc_Datum) attribute;
        if (label.FindAttribute(XCAFDoc_Datum::GetID(), attribute)) {
            const auto object = attribute->GetObject();
            record.name = text(object->GetSemanticName());
            record.datumName = text(object->GetName());
            record.datumPosition = object->GetPosition();
            modifiers(object->GetModifiers(), datumSingleModifNames, record);
            XCAFDimTolObjects_DatumModifWithValue modifier;
            Standard_Real value = 0;
            object->GetModifierWithValue(modifier, value);
            record.strings["modifierWithValue"]
                = enumName(modifier, datumModifWithValueNames, "datum modifier", record);
            record.numbers["modifierWithValueCode"] = modifier;
            record.numbers["modifierValue"] = value;
            record.numbers["isDatumTarget"] = object->IsDatumTarget();
            if (object->IsDatumTarget()) {
                record.strings["targetType"] = enumName(
                    object->GetDatumTargetType(),
                    datumTargetTypeNames,
                    "datum target type",
                    record
                );
                record.numbers["targetType"] = object->GetDatumTargetType();
                record.numbers["targetNumber"] = object->GetDatumTargetNumber();
                if (object->HasDatumTargetParams()) {
                    record.numbers["targetLength"] = object->GetDatumTargetLength();
                    record.numbers["targetWidth"] = object->GetDatumTargetWidth();
                }
            }
        }
        else {
            record.warnings.emplace_back("Missing datum data");
        }
        references(label, record);
        records.push_back(std::move(record));
    }
    for (auto& record : records) {
        record.warnings.insert(record.warnings.end(), sourceWarnings.begin(), sourceWarnings.end());
    }
    return records;
}

void StepPmiImport::materialize(App::Document* target, const std::vector<StepPmiRecord>& records)
{
    if (records.empty()) {
        return;
    }
    Py::List data;
    for (const auto& record : records) {
        data.append(toPython(record));
    }
    Py::Module module(PyImport_ImportModule("StepPmi"), true);
    Py::Tuple args(2);
    args.setItem(0, Py::asObject(target->getPyObject()));
    args.setItem(1, data);
    Py::Callable(module.getAttr("create")).apply(args);
}
}  // namespace Import
