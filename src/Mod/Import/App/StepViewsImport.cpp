// SPDX-License-Identifier: LGPL-2.1-or-later
#include "StepViewsImport.h"

#include <cmath>
#include <set>
#include <stdexcept>
#include <CXX/Objects.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPConstruct_UnitContext.hxx>
#include <StepData_GlobalFactors.hxx>
#include <StepData_StepModel.hxx>
#include <StepData_UndefinedEntity.hxx>
#include <Interface_UndefinedContent.hxx>
#include <StepGeom_Axis2Placement2d.hxx>
#include <StepGeom_Axis2Placement3d.hxx>
#include <StepGeom_CartesianPoint.hxx>
#include <StepGeom_Direction.hxx>
#include <StepGeom_GeometricRepresentationContextAndGlobalUnitAssignedContext.hxx>
#include <StepGeom_GeomRepContextAndGlobUnitAssCtxAndGlobUncertaintyAssCtx.hxx>
#include <StepGeom_Plane.hxx>
#include <StepRepr_GlobalUnitAssignedContext.hxx>
#include <StepBasic_NamedUnit.hxx>
#include <StepBasic_ConversionBasedUnit.hxx>
#include <StepBasic_MeasureWithUnit.hxx>
#include <Standard_Failure.hxx>
#include <StepBasic_SiUnit.hxx>
#include <StepVisual_CameraModelD3.hxx>
#include <StepVisual_CameraModelD3MultiClipping.hxx>
#include <StepVisual_CameraModelD3MultiClippingIntersection.hxx>
#include <StepVisual_CameraModelD3MultiClippingUnion.hxx>
#include <StepVisual_DraughtingModel.hxx>
#include <StepVisual_HArray1OfCameraModelD3MultiClippingInterectionSelect.hxx>
#include <StepVisual_HArray1OfCameraModelD3MultiClippingUnionSelect.hxx>
#include <StepVisual_PlanarBox.hxx>
#include <StepVisual_ViewVolume.hxx>
#include <TDF_Tool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_View.hxx>
#include <XCAFDoc_ViewTool.hxx>
#include <XCAFView_Object.hxx>
#include <gp_Ax2.hxx>

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Console.h>

namespace Import
{
namespace
{
std::string entry(const TDF_Label& label)
{
    TCollection_AsciiString text;
    if (!label.IsNull()) { TDF_Tool::Entry(label, text); }
    return text.ToCString();
}
std::string text(const Handle(TCollection_HAsciiString)& value)
{
    return value.IsNull() ? "" : value->ToCString();
}
std::vector<double> point(const Handle(StepGeom_CartesianPoint)& p, double factor)
{
    if (p.IsNull() || p->NbCoordinates() < 2 || p->NbCoordinates() > 3) {
        throw std::runtime_error("missing camera coordinates");
    }
    return {p->CoordinatesValue(1) * factor, p->CoordinatesValue(2) * factor,
            p->NbCoordinates() == 3 ? p->CoordinatesValue(3) * factor : 0.};
}
gp_Dir direction(const Handle(StepGeom_Direction)& d, const gp_Dir& fallback)
{
    if (d.IsNull()) { return fallback; }
    if (d->NbDirectionRatios() < 2 || d->NbDirectionRatios() > 3) {
        throw std::runtime_error("invalid direction dimensions");
    }
    for (int i = 1; i <= d->NbDirectionRatios(); ++i) {
        if (!std::isfinite(d->DirectionRatiosValue(i))) {
            throw std::runtime_error("nonfinite direction");
        }
    }
    return gp_Dir(d->DirectionRatiosValue(1), d->DirectionRatiosValue(2),
        d->NbDirectionRatios() == 3 ? d->DirectionRatiosValue(3) : 0.);
}
std::vector<double> ratios(const Handle(StepGeom_Direction)& d)
{
    std::vector<double> result;
    if (!d.IsNull()) {
        for (int i = 1; i <= d->NbDirectionRatios(); ++i) {
            result.push_back(d->DirectionRatiosValue(i));
        }
    }
    return result;
}
gp_Ax2 basis(const Handle(StepGeom_Axis2Placement3d)& axis)
{
    if (axis.IsNull()) { throw std::runtime_error("missing camera basis"); }
    return gp_Ax2(gp_Pnt(), direction(axis->Axis(), gp_Dir(0,0,1)),
                  direction(axis->RefDirection(), gp_Dir(1,0,0)));
}
std::vector<double> vector(const gp_Dir& d) { return {d.X(), d.Y(), d.Z()}; }
std::string id(const Handle(StepData_StepModel)& model, const Handle(Standard_Transient)& entity)
{
    return entity.IsNull() ? "missing" : "#" + std::to_string(model->IdentLabel(entity));
}

// 7.6 has no ContextDependentUnit class. Keep unsupported unit declarations as
// literal provenance; the typed context may have lost its reference to them.
// These declarations never establish a unit conversion or an owner association.
std::string unsupportedUnits(const Handle(StepData_StepModel)& model)
{
    std::string result;
    for (int i = 1; i <= model->NbEntities(); ++i) {
        auto entity = Handle(StepData_UndefinedEntity)::DownCast(model->Value(i));
        if (entity.IsNull()) {
            continue;
        }
        std::string declaration;
        bool unit = false;
        int count = 0;
        for (auto part = entity; !part.IsNull() && count++ < 32; part = part->Next()) {
            std::string type = part->StepType();
            unit = unit || type.ends_with("_UNIT");
            declaration += type + "(";
            auto content = part->UndefinedContent();
            if (!content.IsNull()) {
                for (int j = 1; j <= content->NbParams(); ++j) {
                    if (j > 1) {
                        declaration += ",";
                    }
                    declaration += content->IsParamEntity(j)
                        ? id(model, content->ParamEntity(j)) : text(content->ParamValue(j));
                }
            }
            declaration += ") ";
        }
        if (unit) {
            result += id(model, entity) + " " + declaration + "; ";
        }
    }
    return result;
}

double lengthFactor(const Handle(StepRepr_RepresentationContext)& context,
                    StepViewRecord& record, const Handle(StepData_StepModel)& model)
{
    Handle(StepRepr_GlobalUnitAssignedContext) units;
    auto simple = Handle(StepGeom_GeometricRepresentationContextAndGlobalUnitAssignedContext)
        ::DownCast(context);
    auto full = Handle(StepGeom_GeomRepContextAndGlobUnitAssCtxAndGlobUncertaintyAssCtx)
        ::DownCast(context);
    if (!simple.IsNull()) { units = simple->GlobalUnitAssignedContext(); }
    else if (!full.IsNull()) { units = full->GlobalUnitAssignedContext(); }
    record.strings["UnitContexts"] += id(model, context) + "; ";
    std::set<int> dimensions;
    bool valid = !units.IsNull();
    double length = 0;
    if (!units.IsNull()) {
        for (int i = 1; i <= units->NbUnits(); ++i) {
            auto unit = units->UnitsValue(i);
            if (unit.IsNull()) {
                record.strings["SourceUnits"] += "missing unit; ";
                valid = false;
                continue;
            }
            auto& definitions = record.strings["SourceUnits"];
            definitions += id(model, unit) + " " + unit->DynamicType()->Name();
            auto conversion = Handle(StepBasic_ConversionBasedUnit)::DownCast(unit);
            auto si = Handle(StepBasic_SiUnit)::DownCast(unit);
            if (!conversion.IsNull()) {
                definitions += " " + text(conversion->Name());
                auto measure = conversion->ConversionFactor();
                if (!measure.IsNull()) {
                    definitions += " value=" + std::to_string(measure->ValueComponent())
                        + " unit=" + id(model, measure->UnitComponent().Value());
                }
            }
            if (!si.IsNull()) {
                definitions += " name=" + std::to_string(si->Name());
                if (si->HasPrefix()) {
                    definitions += " prefix=" + std::to_string(si->Prefix());
                }
            }
            definitions += "; ";
            // ComputeFactors(context) in 7.6 overwrites earlier errors. Validate
            // every declaration independently, including duplicate dimensions.
            STEPConstruct_UnitContext single;
            if (single.ComputeFactors(unit) != 0) { valid = false; }
            int dimension = single.LengthDone() ? 1 : single.PlaneAngleDone() ? 2
                : single.SolidAngleDone() ? 3 : single.AreaDone() ? 4 : single.VolumeDone() ? 5 : 0;
            double value = dimension == 1 ? single.LengthFactor() : dimension == 2
                ? single.PlaneAngleFactor() : dimension == 3 ? single.SolidAngleFactor()
                : dimension == 4 ? single.AreaFactor() : single.VolumeFactor();
            if (!dimension || !dimensions.insert(dimension).second
                || !std::isfinite(value) || value <= 0) { valid = false; }
            if (dimension == 1) { length = value; }
        }
    }
    if (!valid || !dimensions.count(1)) { return 0; }
    // 7.6.3 ComputeFactors divides by CascadeUnit; undo that to obtain mm.
    double result = length
        * StepData_GlobalFactors::Intance().CascadeUnit(); // codespell:ignore intance
    return std::isfinite(result) && result > 0 ? result : 0;
}

std::vector<Handle(Standard_Transient)> clippingChildren(
    const Handle(Standard_Transient)& entity, char& operation)
{
    std::vector<Handle(Standard_Transient)> children;
    operation = '*';
    auto camera = Handle(StepVisual_CameraModelD3MultiClipping)::DownCast(entity);
    auto intersection = Handle(StepVisual_CameraModelD3MultiClippingIntersection)::DownCast(entity);
    auto unionNode = Handle(StepVisual_CameraModelD3MultiClippingUnion)::DownCast(entity);
    if (!camera.IsNull() || !intersection.IsNull()) {
        auto array = !camera.IsNull() ? camera->ShapeClipping() : intersection->ShapeClipping();
        if (!array.IsNull()) {
            for (int i = array->Lower(); i <= array->Upper(); ++i) {
                children.push_back(array->Value(i).Value());
            }
        }
    }
    else if (!unionNode.IsNull()) {
        operation = '+';
        auto array = unionNode->ShapeClipping();
        if (!array.IsNull()) {
            for (int i = array->Lower(); i <= array->Upper(); ++i) {
                children.push_back(array->Value(i).Value());
            }
        }
    }
    else { throw std::runtime_error("unsupported clipping operand"); }
    return children;
}

std::string sourceClipping(const Handle(Standard_Transient)& root,
                           const Handle(StepData_StepModel)& model)
{
    std::string result;
    std::vector<Handle(Standard_Transient)> pending {root};
    std::set<const Standard_Transient*> seen;
    while (!pending.empty()) {
        auto entity = pending.back();
        pending.pop_back();
        if (entity.IsNull() || !seen.insert(entity.get()).second) { continue; }
        result += id(model, entity) + " " + entity->DynamicType()->Name() + "(";
        char operation;
        try {
            auto children = clippingChildren(entity, operation);
            for (const auto& child : children) {
                result += id(model, child) + ",";
                pending.push_back(child);
            }
        }
        catch (const std::exception&) {} // Leaf or unsupported typed operand: ID/type retained.
        result += "); ";
    }
    return result;
}

// Preserve the Boolean tree using STEP IDs, independent of XCAF label allocation.
std::string clipping(const Handle(Standard_Transient)& entity,
    const Handle(StepData_StepModel)& model, double scale,
    std::vector<StepViewRecord>& planes, std::vector<std::string>& ids,
    std::string& reason, int depth = 0)
{
    if (entity.IsNull()) { throw std::runtime_error("missing clipping operand"); }
    if (depth > 64) { throw std::runtime_error("clipping depth exceeds 64"); }
    auto plane = Handle(StepGeom_Plane)::DownCast(entity);
    if (!plane.IsNull()) {
        const auto sourceId = id(model, plane);
        ids.push_back(sourceId);
        StepViewRecord record;
        record.strings = {{"RecordType", "ClippingPlane"}, {"SourceId", sourceId},
                          {"StepEntityId", sourceId}, {"Name", text(plane->Name())},
                          {"Units", scale ? "mm" : "source (unknown)"},
                          {"RetainedHalfspace", "positive"},
                          {"UnavailableReason", scale ? "" : "unknown length units"}};
        record.strings["SourceBasisId"] = id(model, plane->Position());
        try {
            if (plane->Position().IsNull()) { throw std::runtime_error("missing plane basis"); }
            record.vectors["SourceOrigin"] = point(plane->Position()->Location(), 1);
            record.vectors["SourceAxis"] = ratios(plane->Position()->Axis());
            record.vectors["SourceRefDirection"] = ratios(plane->Position()->RefDirection());
            auto normal = basis(plane->Position()).Direction();
            auto p = point(plane->Position()->Location(), scale ? scale : 1);
            record.vectors["Normal"] = vector(normal);
            record.vectors["Origin"] = p;
            record.numbers["Offset"] = -(normal.X()*p[0] + normal.Y()*p[1] + normal.Z()*p[2]);
            for (double value : p) {
                if (!std::isfinite(value)) { throw std::runtime_error("nonfinite plane origin"); }
            }
        }
        catch (const Standard_Failure& error) {
            record.strings["UnavailableReason"] += std::string("plane data: ") + error.GetMessageString();
        }
        catch (const std::exception& error) { record.strings["UnavailableReason"] += error.what(); }
        if (!record.strings["UnavailableReason"].empty()) {
            reason += "clipping plane " + sourceId + ": " + record.strings["UnavailableReason"] + "; ";
        }
        record.numbers["sourceLengthFactor"] = scale;
        record.flags["Capping"] = false; // STEP camera plane has no capping attribute.
        planes.push_back(std::move(record));
        return sourceId;
    }
    char operation;
    auto children = clippingChildren(entity, operation);
    if (children.empty()) { throw std::runtime_error("empty clipping expression"); }
    std::string result = "(";
    for (auto child : children) {
        if (result.size() > 1) { result += operation; }
        try {
            result += clipping(child, model, scale, planes, ids, reason, depth + 1);
        }
        catch (const Standard_Failure& error) {
            reason += "clipping operand " + id(model, child) + ": " + error.GetMessageString() + "; ";
            result += "unavailable:" + id(model, child);
        }
        catch (const std::exception& error) {
            reason += "clipping operand " + id(model, child) + ": " + error.what() + "; ";
            result += "unavailable:" + id(model, child);
        }
    }
    return result + ")";
}
void cameraData(const Handle(StepVisual_CameraModelD3)& camera,
                const Handle(StepData_StepModel)& model, double scale, StepViewRecord& record)
{
    double factor = scale ? scale : 1;
    auto vrs = camera->ViewReferenceSystem();
    auto volume = camera->PerspectiveOfVolume();
    record.strings["SourceBasisId"] = id(model, vrs);
    record.strings["SourceVolumeId"] = id(model, volume);
    if (!vrs.IsNull()) {
        record.vectors["SourceOrigin"] = point(vrs->Location(), 1);
        record.vectors["SourceAxis"] = ratios(vrs->Axis());
        record.vectors["SourceRefDirection"] = ratios(vrs->RefDirection());
    }
    if (volume.IsNull()) { throw std::runtime_error("missing view volume"); }
    record.numbers["SourceViewPlaneDistance"] = volume->ViewPlaneDistance();
    record.numbers["SourceFrontDistance"] = volume->FrontPlaneDistance();
    record.numbers["SourceBackDistance"] = volume->BackPlaneDistance();
    auto rawWindow = volume->ViewWindow();
    record.strings["SourceWindowId"] = id(model, rawWindow);
    if (!rawWindow.IsNull()) {
        record.vectors["SourceWindowSize"] = {rawWindow->SizeInX(), rawWindow->SizeInY()};
    }
    record.vectors["SourceProjectionPoint"] = point(volume->ProjectionPoint(), 1);
    auto& reason = record.strings["UnavailableReason"];
    auto axis = basis(camera->ViewReferenceSystem());
    auto origin = point(camera->ViewReferenceSystem()->Location(), factor);
    record.vectors["Origin"] = origin;
    record.vectors["ViewDirection"] = vector(axis.Direction());
    record.vectors["UpDirection"] = vector(axis.YDirection());
    record.vectors["RightDirection"] = vector(axis.XDirection());
    if (volume->ViewWindow().IsNull()) {
        throw std::runtime_error("missing view window");
    }
    record.strings["Projection"] = volume->ProjectionType() == StepVisual_copParallel
        ? "parallel" : volume->ProjectionType() == StepVisual_copCentral ? "perspective" : "unknown";
    if (record.strings["Projection"] == "unknown") {
        reason += "unsupported projection; ";
    }
    auto projection = point(volume->ProjectionPoint(), factor);
    record.vectors["ProjectionPoint"] = projection;
    const auto x = axis.XDirection(), y = axis.YDirection(), z = axis.Direction();
    record.vectors["CameraPosition"] = {
        origin[0]+x.X()*projection[0]+y.X()*projection[1]+z.X()*projection[2],
        origin[1]+x.Y()*projection[0]+y.Y()*projection[1]+z.Y()*projection[2],
        origin[2]+x.Z()*projection[0]+y.Z()*projection[1]+z.Z()*projection[2]};
    record.numbers["ViewPlaneDistance"] = volume->ViewPlaneDistance()*factor;
    record.numbers["FrontDistance"] = volume->FrontPlaneDistance()*factor;
    record.numbers["BackDistance"] = volume->BackPlaneDistance()*factor;
    record.flags["FrontClipping"] = volume->FrontPlaneClipping();
    record.flags["BackClipping"] = volume->BackPlaneClipping();
    record.flags["SidesClipping"] = volume->ViewVolumeSidesClipping();
    if (!std::isfinite(volume->ViewPlaneDistance()) || volume->ViewPlaneDistance() <= 0
        || (volume->ProjectionType() == StepVisual_copCentral
            && std::abs(volume->ProjectionPoint()->CoordinatesValue(3)
                        - volume->ViewPlaneDistance()) <= 1e-12)) {
        reason += "invalid view plane distance; ";
    }
    if (!std::isfinite(volume->FrontPlaneDistance()) || !std::isfinite(volume->BackPlaneDistance())
        || volume->FrontPlaneDistance() < 0 || volume->BackPlaneDistance() < 0
        || (volume->FrontPlaneClipping() && volume->BackPlaneClipping()
            && volume->BackPlaneDistance() <= volume->FrontPlaneDistance())) {
        reason += "invalid clipping distances; ";
    }
    auto window = volume->ViewWindow();
    if (!std::isfinite(window->SizeInX()) || !std::isfinite(window->SizeInY())
        || window->SizeInX() <= 0 || window->SizeInY() <= 0) {
        reason += "invalid window dimensions; ";
    }
    record.vectors["WindowSize"] = {window->SizeInX()*factor, window->SizeInY()*factor};
    auto placement = window->Placement();
    auto a2 = placement.Axis2Placement2d();
    auto a3 = placement.Axis2Placement3d();
    if (!a3.IsNull()) {
        record.vectors["SourceWindowOrigin"] = point(a3->Location(), 1);
        record.vectors["SourceWindowAxis"] = ratios(a3->Axis());
        record.vectors["SourceWindowRefDirection"] = ratios(a3->RefDirection());
        record.vectors["WindowOrigin"] = point(a3->Location(), factor);
        auto b = basis(a3);
        record.vectors["WindowRight"] = vector(b.XDirection());
        record.vectors["WindowUp"] = vector(b.YDirection());
    }
    else if (!a2.IsNull()) {
        record.vectors["SourceWindowOrigin"] = point(a2->Location(), 1);
        record.vectors["SourceWindowRefDirection"] = ratios(a2->RefDirection());
        record.vectors["WindowOrigin"] = point(a2->Location(), factor);
        auto right = direction(a2->RefDirection(), gp_Dir(1,0,0));
        record.vectors["WindowRight"] = vector(right);
        record.vectors["WindowUp"] = {-right.Y(), right.X(), 0};
    }
    else { reason += "unsupported window placement; "; }
    if (record.vectors.count("WindowOrigin")) {
        const auto& o = record.vectors.at("WindowOrigin");
        const auto& right = record.vectors.at("WindowRight");
        const auto& up = record.vectors.at("WindowUp");
        if (std::abs(right[0]-1) > 1e-7 || std::abs(right[1]) > 1e-7 || std::abs(right[2]) > 1e-7
            || std::abs(up[0]) > 1e-7 || std::abs(up[1]-1) > 1e-7 || std::abs(up[2]) > 1e-7) {
            reason += "rotated window; ";
        }
        bool centered = true;
        for (int i = 0; i < 3; ++i) {
            centered = centered && std::abs(o[i] + right[i]*window->SizeInX()*factor/2
                                          + up[i]*window->SizeInY()*factor/2) <= 1e-7;
        }
        if (!centered) {
            reason += "asymmetric window; ";
        }
    }
}

Py::List strings(const std::vector<std::string>& values)
{
    Py::List result;
    for (const auto& value : values) { result.append(Py::String(value)); }
    return result;
}
Py::Dict python(const StepViewRecord& record)
{
    Py::Dict result;
    for (auto& item : record.strings) { result[item.first] = Py::String(item.second); }
    for (auto& item : record.numbers) { result[item.first] = Py::Float(item.second); }
    for (auto& item : record.flags) { result[item.first] = Py::Boolean(item.second); }
    for (auto& item : record.vectors) {
        Py::List values;
        for (double value : item.second) { values.append(Py::Float(value)); }
        result[item.first] = values;
    }
    result["planes"] = strings(record.planes);
    Py::List refs;
    for (const auto& ref : record.references) {
        Py::Dict item;
        item["sourceId"] = Py::String(ref.sourceId);
        item["role"] = Py::String(ref.role);
        item["scope"] = Py::String(ref.scope);
        item["objectName"] = Py::String(ref.objectName);
        item["reason"] = Py::String(ref.reason);
        refs.append(item);
    }
    result["references"] = refs;
    return result;
}
}  // namespace

bool StepViewsImport::canReadXcaf(const STEPCAFControl_Reader& reader) noexcept
{
    try {
        const auto model = reader.Reader().StepModel();
        for (int i = 1; i <= model->NbEntities(); ++i) {
            auto camera = Handle(StepVisual_CameraModelD3)::DownCast(model->Value(i));
            if (camera.IsNull()) { continue; }
            StepViewRecord record;
            cameraData(camera, model, 1, record);
            auto multi = Handle(StepVisual_CameraModelD3MultiClipping)::DownCast(camera);
            if (!multi.IsNull()) {
                std::vector<StepViewRecord> planes;
                std::string reason;
                clipping(multi, model, 1, planes, record.planes, reason);
                if (!reason.empty()) { return false; }
            }
        }
        return true;
    }
    catch (...) { return false; }
}

void StepViewsImport::capture(const STEPCAFControl_Reader& reader,
    const Handle(TDocStd_Document)& source) noexcept
{
    try {
        const auto model = reader.Reader().StepModel();
        auto tool = XCAFDoc_DocumentTool::ViewTool(source->Main());
        TDF_LabelSequence labels;
        tool->GetViewLabels(labels);
        std::vector<Handle(StepVisual_CameraModelD3)> cameras;
        for (int i = 1; i <= model->NbEntities(); ++i) {
            auto camera = Handle(StepVisual_CameraModelD3)::DownCast(model->Value(i));
            if (!camera.IsNull()) { cameras.push_back(camera); }
        }
        // ReadViews in 7.6.3 adds one label per camera, in model order. If it returned
        // early, refuse all associations; names never establish identity.
        const bool complete = labels.Length() == static_cast<int>(cameras.size());
        const auto unsupportedUnitData = unsupportedUnits(model);
        std::vector<StepViewRecord> planeRecords;
        for (size_t index = 0; index < cameras.size(); ++index) {
            auto camera = cameras[index];
            StepViewRecord record;
            record.strings = {{"RecordType", "SavedView"}, {"Name", text(camera->Name())},
                {"StepEntityId", id(model, camera)}, {"SourceId", id(model, camera)},
                {"UnavailableReason", ""}};
            double scale = 0;
            int contexts = 0;
            for (int i = 1; i <= model->NbEntities(); ++i) {
                auto drawing = Handle(StepVisual_DraughtingModel)::DownCast(model->Value(i));
                if (drawing.IsNull()) { continue; }
                for (int j = 1; j <= drawing->NbItems(); ++j) {
                    if (drawing->ItemsValue(j) == camera) {
                        ++contexts;
                        record.strings["DraughtingModels"] += id(model, drawing) + "; ";
                        try { scale = lengthFactor(drawing->ContextOfItems(), record, model); }
                        catch (...) {
                            scale = 0;
                            record.strings["UnavailableReason"] += "invalid unit declaration; ";
                        }
                    }
                }
            }
            if (contexts != 1) { scale = 0; }
            record.numbers["sourceLengthFactor"] = scale;
            record.strings["Units"] = scale ? "mm" : "source (unknown)";
            auto& reason = record.strings["UnavailableReason"];
            if (!scale) {
                reason += "unknown or ambiguous length units; ";
                if (!unsupportedUnitData.empty()) {
                    record.strings["SourceUnits"] +=
                        "unassociated unsupported unit declarations: " + unsupportedUnitData;
                }
            }
            try { cameraData(camera, model, scale, record); }
            catch (const Standard_Failure& error) {
                reason += std::string("camera data: ") + error.GetMessageString() + "; ";
            }
            catch (const std::exception& error) { reason += std::string(error.what()) + "; "; }
            // A missing volume must not hide this camera's independent clipping data.
            try {
                auto multi = Handle(StepVisual_CameraModelD3MultiClipping)::DownCast(camera);
                record.strings["SourceClippingTree"] = multi.IsNull() ? "" : sourceClipping(multi, model);
                record.strings["ClippingExpression"] = multi.IsNull() ? ""
                    : clipping(multi, model, scale, planeRecords, record.planes, reason);
                if (record.strings["ClippingExpression"].find('+') != std::string::npos) {
                    reason += "union evaluation unavailable; ";
                }
            }
            catch (const Standard_Failure& error) {
                reason += std::string("clipping data: ") + error.GetMessageString() + "; ";
            }
            catch (const std::exception& error) { reason += std::string(error.what()) + "; "; }
            if (complete) {
                auto label = labels.Value(static_cast<int>(index) + 1);
                record.strings["SourceId"] = entry(label);
                Handle(XCAFDoc_View) view;
                if (!label.FindAttribute(XCAFDoc_View::GetID(), view)
                    || text(view->GetObject()->Name()) != text(camera->Name())) {
                    reason += "view table order unavailable; ";
                    record.references.push_back({"", "shape", "view table", "", "view table order unavailable"});
                    record.references.push_back({"", "pmi", "view table", "", "view table order unavailable"});
                    records.push_back(std::move(record));
                    continue;
                }
                record.strings["XcafClippingExpression"] = text(view->GetObject()->ClippingExpression());
                TDF_LabelSequence shapes, gdts;
                tool->GetRefShapeLabel(label, shapes);
                tool->GetRefGDTLabel(label, gdts);
                for (int j = 1; j <= shapes.Length(); ++j) {
                    auto owner = shapes.Value(j);
                    TDF_LabelSequence users;
                    XCAFDoc_ShapeTool::GetUsers(owner, users);
                    bool occurrence = XCAFDoc_ShapeTool::IsReference(owner) || !users.IsEmpty();
                    record.references.push_back({entry(owner), "shape",
                        occurrence ? "definition with occurrences" : XCAFDoc_ShapeTool::IsAssembly(owner)
                            ? "root assembly" : "root definition", "",
                        occurrence ? "occurrence path unavailable" : ""});
                }
                for (int j = 1; j <= gdts.Length(); ++j) {
                    record.references.push_back({entry(gdts.Value(j)), "pmi", "semantic PMI", "", ""});
                }
            }
            else {
                reason += "incomplete XCAF view table; associations unavailable; ";
                for (const auto role : {"shape", "pmi"}) {
                    record.references.push_back({"", role, "view table", "", "incomplete XCAF view table"});
                }
            }
            if (contexts != 1) {
                for (auto& ref : record.references) {
                    ref.reason = "ambiguous draughting models; " + ref.reason;
                }
            }
            records.push_back(std::move(record));
        }
        // Keep each view context's scale. Python deduplicates only identical source ID/scale
        // pairs, so views in different unit contexts cannot overwrite each other's planes.
        records.insert(records.end(), planeRecords.begin(), planeRecords.end());
    }
    catch (...) { failed = true; }
}

ShapeImportObserver StepViewsImport::observer()
{
    return [this](const TDF_Label& label, const TopoDS_Shape&, App::DocumentObject* object) {
        try {
            if (!object || label.IsNull()) { return; }
            owners[entry(label)].push_back({object->getDocument(), object->getNameInDocument(),
                XCAFDoc_ShapeTool::IsReference(label)});
        }
        catch (...) { failed = true; }
    };
}

void StepViewsImport::finish(App::Document* target, bool supported,
    const std::vector<std::string>& pmiObjects) const noexcept
{
    try {
        if (failed) { throw std::runtime_error("view extraction unavailable"); }
        if (records.empty()) { return; }
        auto resolved = records;
        for (auto& record : resolved) {
            for (auto& ref : record.references) {
                if (!supported) { ref.reason = "merged or multi-document association unavailable; " + ref.reason; }
                if (ref.role != "shape" || !ref.reason.empty()) { continue; }
                std::set<std::string> matches;
                const auto found = owners.find(ref.sourceId);
                if (found != owners.end()) {
                    for (auto& owner : found->second) {
                        if (owner.document == target && target->getObject(owner.name.c_str())
                            && !owner.occurrence) {
                            matches.insert(owner.name);
                        }
                    }
                }
                if (matches.size() == 1) { ref.objectName = *matches.begin(); }
                else { ref.reason = matches.empty() ? "omitted or unknown exact owner" : "ambiguous exact owner"; }
            }
        }
        Py::List data;
        for (auto& record : resolved) { data.append(python(record)); }
        Py::Module module(PyImport_ImportModule("StepViews"), true);
        Py::Tuple args(3);
        args.setItem(0, Py::asObject(target->getPyObject()));
        args.setItem(1, data);
        args.setItem(2, strings(pmiObjects));
        Py::Callable(module.getAttr("create")).apply(args);
    }
    catch (...) {
        PyErr_Clear();
        Base::Console().warning("STEP saved views: metadata unavailable; geometry/PMI kept.\n");
    }
}
}  // namespace Import
