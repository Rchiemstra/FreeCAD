// SPDX-License-Identifier: LGPL-2.1-or-later
#include "StepAttributesExport.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <stdexcept>
#include <Standard_Failure.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPConstruct.hxx>
#include <StepData_StepModel.hxx>
#include <StepBasic_ProductDefinition.hxx>
#include <StepBasic_MeasureValueMember.hxx>
#include <StepBasic_SiUnitAndLengthUnit.hxx>
#include <StepBasic_SiUnitAndPlaneAngleUnit.hxx>
#include <StepBasic_ConversionBasedUnitAndLengthUnit.hxx>
#include <StepBasic_LengthMeasureWithUnit.hxx>
#include <StepBasic_DimensionalExponents.hxx>
#include <StepRepr_DescriptiveRepresentationItem.hxx>
#include <StepRepr_HArray1OfRepresentationItem.hxx>
#include <StepRepr_ReprItemAndLengthMeasureWithUnit.hxx>
#include <StepRepr_ReprItemAndPlaneAngleMeasureWithUnit.hxx>
#include <StepBasic_PlaneAngleMeasureWithUnit.hxx>
#include <StepRepr_NextAssemblyUsageOccurrence.hxx>
#include <StepRepr_PropertyDefinition.hxx>
#include <StepRepr_PropertyDefinitionRepresentation.hxx>
#include <StepRepr_Representation.hxx>
#include <StepRepr_RepresentationContext.hxx>
#include <StepShape_ShapeDefinitionRepresentation.hxx>
#include <StepShape_ContextDependentShapeRepresentation.hxx>
#include <TransferBRep.hxx>
#include <TDF_LabelSequence.hxx>
#include <TransferBRep_ShapeMapper.hxx>
#include <Transfer_FinderProcess.hxx>
#include <XSControl_TransferWriter.hxx>
#include <XSControl_WorkSession.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObjectPy.h>
#include <App/PropertyStandard.h>
#include <Base/Console.h>
#include <Mod/Part/App/Interface.h>

namespace Import
{
namespace
{
std::string text(const Py::Dict& item, const char* key)
{
    return Py::String(item.getItem(key)).as_std_string();
}
Handle(TCollection_HAsciiString) string(const Py::Dict& item, const char* key)
{
    return new TCollection_HAsciiString(text(item, key).c_str());
}
App::DocumentObject* object(const Py::Object& value)
{
    return static_cast<App::DocumentObjectPy*>(value.ptr())->getDocumentObjectPtr();
}
Py::Dict row(const Py::Object& obj, const std::string& reason)
{
    Py::Dict result;
    result["object"] = obj.getAttr("Name");
    result["kind"] = Py::String("userAttribute");
    result["status"] = Py::String(reason.empty() ? "written" : "skipped");
    result["reason"] = Py::String(reason);
    return result;
}
}
StepAttributesExport::StepAttributesExport(int override)
    : enabled(override < 0 ? App::GetApplication().GetParameterGroupByPath(
          "User parameter:BaseApp/Preferences/Mod/Import")->GetBool("ExportUserAttributes", false)
          : override != 0)
{}
ShapeExportObserver StepAttributesExport::observer()
{
    if (!enabled) { return {}; }
    return [this](const TDF_Label& label, App::DocumentObject* owner,
                  const Part::TopoShape&) noexcept {
        try {
            if (!testStage("observation", false, associations.size())) {
                throw std::runtime_error("converter observation unavailable");
            }
            associations.push_back({label, owner});
        }
        catch (...) { captureFailed = true; }
    };
}
void StepAttributesExport::prepare(const std::vector<App::DocumentObject*>& objects)
{
    if (!enabled) { return; }
    Py::List owners;
    for (auto obj : objects) { owners.append(Py::asObject(obj->getPyObject())); }
    try {
        PyObject* helper = PyImport_ImportModule("StepAttributesExport");
        if (!helper) { throw Py::Exception(); }
        Py::Module module(helper, true);
        Py::Tuple args(1);
        args[0] = owners;
        candidates = Py::List(Py::Callable(module.getAttr("prepare")).apply(args));
        if (captureFailed) { fail("converter observation unavailable; geometry/PMI kept"); }
    }
    catch (...) {
        PyErr_Clear();
        candidates = Py::List();
        // The helper itself may be missing. Enumerate only native attribute records.
        std::set<App::Document*> documents;
        for (auto owner : objects) {
            if (!documents.insert(owner->getDocument()).second) { continue; }
            for (auto obj : owner->getDocument()->getObjects()) {
                bool attribute = false;
                for (auto group : obj->getInList()) {
                    const auto kind = dynamic_cast<App::PropertyString*>(group->getPropertyByName("MetadataKind"));
                    const auto members = dynamic_cast<App::PropertyLinkList*>(group->getPropertyByName("Group"));
                    if (kind && std::string(kind->getValue()) == "UserAttributes" && members) {
                        const auto& values = members->getValues();
                        if (std::find(values.begin(), values.end(), obj) != values.end()) { attribute = true; }
                    }
                }
                if (attribute) {
                    Py::Dict item;
                    item["object"] = Py::asObject(obj->getPyObject());
                    std::string reason = "attribute preparation unavailable";
                    for (const char* field : {"SupplierName", "Owner", "ValueKind"}) {
                        if (!obj->getPropertyByName(field)) {
                            reason = std::string("missing native field: ") + field;
                            break;
                        }
                    }
                    item["reason"] = Py::String(reason);
                    candidates.append(item);
                }
            }
        }
        fail("attribute preparation unavailable; geometry/PMI kept");
    }
}
bool StepAttributesExport::hasCandidates() const
{
    return !captureFailed && std::any_of(candidates.begin(), candidates.end(), [](const Py::Object& value) {
        return text(Py::Dict(value), "reason").empty();
    });
}
bool StepAttributesExport::testStage(const char* stage, bool fallback, int entities) const
{
    if (!enabled) { return true; }
    // Only consult an already loaded helper. Production does not install this hook.
    auto module = PyDict_GetItemString(PyImport_GetModuleDict(), "StepAttributesExport");
    if (!module || module == Py_None) { return true; }
    auto callback = PyObject_GetAttrString(module, "_testExportStage");
    if (!callback) { PyErr_Clear(); return true; }
    Py::Callable hook(callback, true);
    Py::Tuple args(3);
    args[0] = Py::String(stage);
    args[1] = Py::Boolean(fallback);
    args[2] = Py::Long(entities);
    const auto fault = Py::String(hook.apply(args)).as_std_string();
    if (fault == "exception") { throw Standard_Failure("injected STEP writer exception"); }
    return fault != "false";
}
void StepAttributesExport::fail(const std::string& reason)
{
    results = Py::List();
    for (const auto& value : candidates) {
        Py::Dict item(value);
        const auto prior = text(item, "reason");
        results.append(row(item.getItem("object"), prior.empty() ? reason : prior));
    }
    PyErr_Clear();
}
Py::Object StepAttributesExport::report(const Py::Object& pmi) const
{
    if (!enabled) { return pmi; }
    Py::List combined;
    if (!pmi.isNone()) {
        for (const auto& item : Py::List(pmi)) { combined.append(item); }
    }
    for (const auto& item : results) {
        combined.append(item);
        Py::Dict result(item);
        Base::Console().message("STEP user attribute {}: {} {}\n", text(result, "object"),
                                text(result, "status"), text(result, "reason"));
    }
    return combined;
}
void StepAttributesExport::complete(STEPCAFControl_Writer& writer,
                                   const Handle(TDocStd_Document)& document)
{
    if (!enabled) { return; }
    if (captureFailed) {
        fail("converter observation unavailable; geometry/PMI kept");
        return;
    }
    results = Py::List();
    const auto shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const auto model = writer.ChangeWriter().Model();
    const auto process = writer.ChangeWriter().WS()->TransferWriter()->FinderProcess();
    auto definition = [&](const TDF_Label& label) {
        Handle(StepShape_ShapeDefinitionRepresentation) result;
        const auto mapper = TransferBRep::ShapeMapper(process, shapes->GetShape(label));
        process->FindTypedTransient(mapper, STANDARD_TYPE(StepShape_ShapeDefinitionRepresentation), result);
        return result;
    };
    struct Set {
        App::DocumentObject* owner;
        int index;
        Handle(Standard_Transient) target;
        std::string property, description, representation, identifier, type;
        std::vector<Handle(StepRepr_RepresentationItem)> items;
    };
    std::vector<Set> sets;
    for (const auto& value : candidates) {
        Py::Dict item(value);
        auto reason = text(item, "reason");
        Handle(Standard_Transient) target;
        Py::Object owner = Py::None();
        if (reason.empty()) {
            owner = item.getItem("owner");
            std::vector<TDF_Label> labels;
            const bool occurrence = text(item, "scope") == "occurrence";
            for (const auto& association : associations) {
                if (association.owner != object(owner)
                    || shapes->IsComponent(association.label) != occurrence) { continue; }
                if (std::find(labels.begin(), labels.end(), association.label) == labels.end()) {
                    labels.push_back(association.label);
                }
            }
            if (labels.size() != 1) { reason = "omitted or ambiguous exact exported owner"; }
            else {
                const auto label = labels.front();
                TDF_Label base = label;
                if (occurrence) { shapes->GetReferredShape(label, base); }
                // Refuse aliases where different native objects claimed a definition.
                std::set<App::DocumentObject*> owners;
                for (const auto& association : associations) {
                    if (association.label == base && association.owner) { owners.insert(association.owner); }
                }
                TDF_LabelSequence users;
                shapes->GetUsers(base, users);
                if (!occurrence && (users.Length() > 1 || owners.size() != 1)) {
                    reason = "ambiguous repeated or shared exported definition";
                }
                // An occurrence must have a free, non-repeated parent. No synthetic paths.
                TDF_LabelSequence parentUsers;
                if (occurrence) { shapes->GetUsers(label.Father(), parentUsers); }
                if (occurrence && !parentUsers.IsEmpty()) {
                    reason = "nested or repeated occurrence parent unavailable";
                }
                // A transfer binder must be injective over the observed labels.
                // Equal geometry or a shared binder cannot distinguish two native owners.
                const auto exactBinder = process->Find(TransferBRep::ShapeMapper(
                    process, shapes->GetShape(label)));
                for (const auto& association : associations) {
                    if (association.label == label
                        || shapes->IsComponent(association.label) != occurrence) { continue; }
                    if (occurrence && association.label.Father() != label.Father()) { continue; }
                    const auto other = process->Find(TransferBRep::ShapeMapper(
                        process, shapes->GetShape(association.label)));
                    if (!exactBinder.IsNull() && other == exactBinder) {
                        reason = "ambiguous shared writer transfer binder";
                        break;
                    }
                }
                auto sdr = definition(base);
                if (reason.empty() && sdr.IsNull()) { reason = "exact writer definition binder unavailable"; }
                if (reason.empty() && occurrence) {
                    auto parent = definition(label.Father());
                    Handle(StepShape_ContextDependentShapeRepresentation) cdsr;
                    const auto binder = process->Find(TransferBRep::ShapeMapper(process, shapes->GetShape(label)));
                    if (parent.IsNull() || binder.IsNull() || !STEPConstruct::FindCDSR(binder, parent, cdsr)
                        || cdsr.IsNull() || cdsr->RepresentedProductRelation().IsNull()) {
                        reason = "exact writer occurrence binder unavailable";
                    }
                    else { target = cdsr->RepresentedProductRelation()->Definition().Value(); }
                    if (reason.empty() && Handle(StepRepr_NextAssemblyUsageOccurrence)::DownCast(target).IsNull()) {
                        reason = "writer occurrence target unavailable";
                    }
                }
                else if (reason.empty()) {
                    auto property = sdr->Definition().PropertyDefinition();
                    if (!property.IsNull()) { target = property->Definition().Value(); }
                    if (Handle(StepBasic_ProductDefinition)::DownCast(target).IsNull()) {
                        reason = "writer product definition target unavailable";
                    }
                    // Definitions under a nested/repeated assembly have no unique scope.
                    for (int i = 1; reason.empty() && i <= users.Length(); ++i) {
                        TDF_LabelSequence ancestors;
                        shapes->GetUsers(users.Value(i).Father(), ancestors);
                        if (!ancestors.IsEmpty()) { reason = "nested definition scope unavailable"; }
                    }
                }
            }
        }
        if (!reason.empty()) {
            results.append(row(item.getItem("object"), reason));
            continue;
        }
        const auto kind = text(item, "kind");
        Handle(StepRepr_RepresentationItem) emitted;
        if (kind == "String") {
            Handle(StepRepr_DescriptiveRepresentationItem) descriptive = new StepRepr_DescriptiveRepresentationItem;
            descriptive->Init(string(item, "name"), string(item, "value"));
            emitted = descriptive;
        }
        else {
            const bool angle = kind == "Angle";
            const double native = Py::Float(item.getItem("value"));
            double factor = angle ? std::numbers::pi / 180.0 : 1.0;
            StepBasic_Unit unit;
            if (angle) {
                Handle(StepBasic_SiUnitAndPlaneAngleUnit) radians = new StepBasic_SiUnitAndPlaneAngleUnit;
                radians->Init(false, StepBasic_spMilli, StepBasic_sunRadian);
                unit.SetValue(radians);
            }
            else {
                const std::string output = Part::Interface::writeStepUnit();
                Handle(StepBasic_SiUnitAndLengthUnit) si = new StepBasic_SiUnitAndLengthUnit;
                si->Init(output != "M", StepBasic_spMilli, StepBasic_sunMetre);
                if (output == "INCH") {
                    Handle(StepBasic_MeasureValueMember) member = new StepBasic_MeasureValueMember;
                    member->SetName("LENGTH_MEASURE"); member->SetReal(25.4);
                    StepBasic_Unit mm; mm.SetValue(si);
                    Handle(StepBasic_LengthMeasureWithUnit) conversion = new StepBasic_LengthMeasureWithUnit;
                    conversion->Init(member, mm);
                    Handle(StepBasic_DimensionalExponents) dimensions = new StepBasic_DimensionalExponents;
                    dimensions->Init(1, 0, 0, 0, 0, 0, 0);
                    Handle(StepBasic_ConversionBasedUnitAndLengthUnit) inches = new StepBasic_ConversionBasedUnitAndLengthUnit;
                    inches->Init(dimensions, new TCollection_HAsciiString("INCH"), conversion);
                    unit.SetValue(inches); factor = 1.0 / 25.4;
                }
                else if (output == "MM" || output == "M") {
                    unit.SetValue(si); factor = output == "M" ? 0.001 : 1;
                }
                else { throw std::runtime_error("unsupported attribute output length unit"); }
            }
            // OCCT 7.6 does not Share() the complex item's explicit unit.
            model->AddWithRefs(unit.Value());
            const double converted = native * factor;
            if (!std::isfinite(native) || !std::isfinite(converted)) {
                throw std::runtime_error("nonfinite attribute value");
            }
            Handle(StepBasic_MeasureValueMember) member = new StepBasic_MeasureValueMember;
            member->SetName(angle ? "PLANE_ANGLE_MEASURE" : "LENGTH_MEASURE");
            member->SetReal(converted);
            Handle(StepRepr_RepresentationItem) named = new StepRepr_RepresentationItem;
            named->Init(string(item, "name"));
            if (angle) {
                Handle(StepBasic_PlaneAngleMeasureWithUnit) value = new StepBasic_PlaneAngleMeasureWithUnit;
                value->Init(member, unit);
                Handle(StepRepr_ReprItemAndPlaneAngleMeasureWithUnit) measure
                    = new StepRepr_ReprItemAndPlaneAngleMeasureWithUnit;
                measure->Init(value, named);
                measure->SetPlaneAngleMeasureWithUnit(value);
                emitted = measure;
            }
            else {
                Handle(StepBasic_LengthMeasureWithUnit) value = new StepBasic_LengthMeasureWithUnit;
                value->Init(member, unit);
                Handle(StepRepr_ReprItemAndLengthMeasureWithUnit) measure
                    = new StepRepr_ReprItemAndLengthMeasureWithUnit;
                measure->Init(value, named);
                measure->SetLengthMeasureWithUnit(value);
                emitted = measure;
            }
        }
        // Add now, so the injected later-record failure exercises partial model discard.
        model->AddWithRefs(emitted);
        const Set key {object(owner), static_cast<int>(Py::Long(item.getItem("setIndex"))), target, text(item, "propertySet"), text(item, "description"),
                       text(item, "representation"), text(item, "contextIdentifier"), text(item, "contextType"), {}};
        auto found = std::find_if(sets.begin(), sets.end(), [&](const Set& set) {
            return set.owner == key.owner && set.index == key.index && set.target == key.target && set.property == key.property
                && set.description == key.description && set.representation == key.representation
                && set.identifier == key.identifier && set.type == key.type;
        });
        if (found == sets.end()) { sets.push_back(key); found = std::prev(sets.end()); }
        found->items.push_back(emitted);
        results.append(row(item.getItem("object"), ""));
    }
    for (const auto& set : sets) {
        Handle(StepRepr_RepresentationContext) context = new StepRepr_RepresentationContext;
        context->Init(new TCollection_HAsciiString(set.identifier.c_str()), new TCollection_HAsciiString(set.type.c_str()));
        Handle(StepRepr_HArray1OfRepresentationItem) items = new StepRepr_HArray1OfRepresentationItem(1, set.items.size());
        for (size_t i = 0; i < set.items.size(); ++i) { items->SetValue(i + 1, set.items[i]); }
        Handle(StepRepr_Representation) representation = new StepRepr_Representation;
        representation->Init(new TCollection_HAsciiString(set.representation.c_str()), items, context);
        StepRepr_CharacterizedDefinition scope; scope.SetValue(set.target);
        Handle(StepRepr_PropertyDefinition) property = new StepRepr_PropertyDefinition;
        property->Init(new TCollection_HAsciiString(set.property.c_str()), true,
                       new TCollection_HAsciiString(set.description.c_str()), scope);
        StepRepr_RepresentedDefinition represented; represented.SetValue(property);
        Handle(StepRepr_PropertyDefinitionRepresentation) chain = new StepRepr_PropertyDefinitionRepresentation;
        chain->Init(represented, representation);
        model->AddWithRefs(chain);
    }
}
}  // namespace Import
