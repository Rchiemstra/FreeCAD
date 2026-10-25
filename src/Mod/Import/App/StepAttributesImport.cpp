// SPDX-License-Identifier: LGPL-2.1-or-later
#include "StepAttributesImport.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <CXX/Objects.hxx>
#include <Interface_Check.hxx>
#include <Interface_EntityIterator.hxx>
#include <Interface_ReportEntity.hxx>
#include <Interface_UndefinedContent.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPConstruct_UnitContext.hxx>
#include <StepBasic_ConversionBasedUnit.hxx>
#include <StepBasic_MeasureValueMember.hxx>
#include <StepBasic_MeasureWithUnit.hxx>
#include <StepBasic_ProductDefinition.hxx>
#include <StepBasic_SiUnit.hxx>
#include <StepData_GlobalFactors.hxx>
#include <StepData_StepModel.hxx>
#include <StepData_UndefinedEntity.hxx>
#include <StepRepr_DescriptiveRepresentationItem.hxx>
#include <StepRepr_MeasureRepresentationItem.hxx>
#include <StepRepr_NextAssemblyUsageOccurrence.hxx>
#include <StepRepr_ProductDefinitionShape.hxx>
#include <StepRepr_PropertyDefinitionRepresentation.hxx>
#include <StepRepr_Representation.hxx>
#include <StepRepr_RepresentationContext.hxx>
#include <StepRepr_ReprItemAndMeasureWithUnit.hxx>
#include <StepShape_ShapeDefinitionRepresentation.hxx>
#include <TDF_Tool.hxx>
#include <Transfer_Binder.hxx>
#include <TransferBRep.hxx>
#include <Transfer_TransientProcess.hxx>
#include <XSControl_TransferReader.hxx>
#include <XSControl_WorkSession.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Console.h>
#include <Base/FileInfo.h>
#include <Base/Stream.h>

namespace Import
{
namespace
{
std::string text(const Handle(TCollection_HAsciiString)& value)
{
    return value.IsNull() ? "" : value->ToCString();
}
std::string id(const Handle(StepData_StepModel)& model, const Handle(Standard_Transient)& entity)
{
    return entity.IsNull() ? "missing" : "#" + std::to_string(model->IdentLabel(entity));
}
std::string entry(const TDF_Label& label)
{
    TCollection_AsciiString value;
    if (!label.IsNull()) { TDF_Tool::Entry(label, value); }
    return value.ToCString();
}
std::string number(double value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << value;
    return out.str();
}
// Typed reads can substitute zero or drop a select after malformed input. Keep
// the diagnostic's original parameters and refuse that record's numeric value.
std::string literal(const Handle(Standard_Transient)& entity,
                    const Handle(StepData_StepModel)& model, int depth = 0)
{
    if (depth > 16) { return "depth unavailable"; }
    auto unknown = Handle(StepData_UndefinedEntity)::DownCast(entity);
    if (unknown.IsNull()) { return id(model, entity); }
    std::string result;
    int count = 0;
    for (auto part = unknown; !part.IsNull() && count++ < 32; part = part->Next()) {
        result += std::string(part->StepType()) + "(";
        auto content = part->UndefinedContent();
        if (!content.IsNull()) {
            for (int i = 1; i <= content->NbParams(); ++i) {
                if (i > 1) { result += ","; }
                result += content->IsParamEntity(i)
                    ? literal(content->ParamEntity(i), model, depth + 1)
                    : text(content->ParamValue(i));
            }
        }
        result += ") ";
    }
    return result;
}
// OCCT 7.6 silently accepts '$' and quoted text as real zero. Its typed
// graph cannot distinguish those from a supplier's actual zero. This bounded
// lexical check validates only explicit scalar tokens, keyed by exact STEP ID;
// it never supplies an owner or a conversion. All values are owned before close.
std::map<std::string, StepAttributeScalar> sourceScalars(const Base::FileInfo& file)
{
    Base::ifstream input(file, std::ios::binary);
    std::map<std::string, StepAttributeScalar> result;
    std::string raw, mask;
    bool quoted = false, comment = false, oversized = false;
    const std::regex entity(R"(^\s*#([0-9]+)\s*=)");
    const std::regex measure(R"(MEASURE_WITH_UNIT\s*\(\s*([A-Z_][A-Z_0-9]*)\s*\(([^()]*)\))");
    const std::regex numeric(R"(^\s*[+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([Ee][+-]?[0-9]+)?\s*$)");
    auto append = [&](char value, bool hidden) {
        if (raw.size() < 65536) {
            raw += value;
            mask += hidden ? ' ' : value;
        }
        else { oversized = true; }
    };
    char c;
    while (input.get(c)) {
        if (comment) {
            append(c, true);
            if (c == '*' && input.peek() == '/') {
                input.get(c); append(c, true); comment = false;
            }
            continue;
        }
        if (quoted) {
            append(c, true);
            if (c == '\'') {
                if (input.peek() == '\'') { input.get(c); append(c, true); }
                else { quoted = false; }
            }
            continue;
        }
        if (c == '/' && input.peek() == '*') {
            append(c, true); input.get(c); append(c, true); comment = true; continue;
        }
        if (c == '\'') { append(c, true); quoted = true; continue; }
        append(c, false);
        if (c != ';') { continue; }
        std::smatch identity;
        if (std::regex_search(mask, identity, entity)) {
            const auto key = "#" + identity[1].str();
            if (oversized) { result[key].reason = "source entity exceeds scalar validation limit"; }
            else {
                std::sregex_iterator first(mask.begin(), mask.end(), measure), last;
                for (auto match = first; match != last; ++match) {
                    StepAttributeScalar scalar;
                    scalar.type = (*match)[1].str();
                    scalar.token = raw.substr((*match).position(2), (*match).length(2));
                    if (!std::regex_match(scalar.token, numeric)) {
                        scalar.reason = "malformed source numeric token";
                    }
                    else {
                        std::istringstream number(scalar.token);
                        number.imbue(std::locale::classic());
                        number >> scalar.value;
                        if (!number || !std::isfinite(scalar.value)) {
                            scalar.reason = "nonfinite or out-of-range source numeric token";
                        }
                    }
                    if (result.count(key)) { scalar.reason = "ambiguous source scalar tokens"; }
                    result[key] = std::move(scalar);
                }
            }
        }
        raw.clear(); mask.clear(); oversized = false;
    }
    return result;
}

// A typed representation may lose an unsupported item select. Its exact
// position and entity survive in OCCT's diagnostic content; never fill a hole
// by scanning names or by choosing a nearby model entity.
Handle(Standard_Transient) originalItem(const Handle(StepData_StepModel)& model,
    const Handle(StepRepr_Representation)& rep, int index)
{
    auto item = rep->ItemsValue(index);
    if (!item.IsNull()) { return item; }
    for (auto reports = model->Reports(); reports.More(); reports.Next()) {
        auto report = Handle(Interface_ReportEntity)::DownCast(reports.Value());
        if (report.IsNull() || report->Concerned() != rep) { continue; }
        auto raw = Handle(StepData_UndefinedEntity)::DownCast(report->Content());
        if (raw.IsNull() || raw->UndefinedContent().IsNull()) { continue; }
        auto content = raw->UndefinedContent();
        if (content->NbParams() < 2 || !content->IsParamEntity(2)) { continue; }
        auto list = Handle(StepData_UndefinedEntity)::DownCast(content->ParamEntity(2));
        if (!list.IsNull() && !list->UndefinedContent().IsNull()
            && index <= list->UndefinedContent()->NbParams()
            && list->UndefinedContent()->IsParamEntity(index)) {
            return list->UndefinedContent()->ParamEntity(index);
        }
    }
    return {};
}
std::string undefinedName(const Handle(StepData_UndefinedEntity)& item)
{
    int count = 0;
    for (auto part = item; !part.IsNull() && count++ < 32; part = part->Next()) {
        if (std::string(part->StepType()) != "REPRESENTATION_ITEM") { continue; }
        auto content = part->UndefinedContent();
        if (content.IsNull() || !content->NbParams() || content->IsParamEntity(1)) { return ""; }
        auto name = text(content->ParamValue(1));
        if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'') {
            name = name.substr(1, name.size() - 2);
            for (size_t p = 0; (p = name.find("''", p)) != std::string::npos; ++p) { name.erase(p, 1); }
        }
        return name;
    }
    return "";
}

std::string unitDescription(const Handle(StepBasic_NamedUnit)& unit,
    const Handle(StepData_StepModel)& model, int depth = 0)
{
    if (unit.IsNull()) { return "missing unit"; }
    if (depth > 16) { return "cyclic or excessive unit chain"; }
    std::string result = id(model, unit) + " " + unit->DynamicType()->Name();
    auto si = Handle(StepBasic_SiUnit)::DownCast(unit);
    auto conversion = Handle(StepBasic_ConversionBasedUnit)::DownCast(unit);
    if (!si.IsNull()) {
        result += " name=" + std::to_string(si->Name());
        if (si->HasPrefix()) { result += " prefix=" + std::to_string(si->Prefix()); }
    }
    if (!conversion.IsNull()) {
        result += " " + text(conversion->Name());
        auto measure = conversion->ConversionFactor();
        if (!measure.IsNull()) {
            result += " factor=" + number(measure->ValueComponent()) + " "
                + unitDescription(measure->UnitComponent().NamedUnit(), model, depth + 1);
        }
    }
    return result;
}
// Validate recursion before entering OCCT's recursive conversion routine.
bool validUnit(const Handle(StepBasic_NamedUnit)& unit, int depth = 0)
{
    if (unit.IsNull() || depth > 16) { return false; }
    auto conversion = Handle(StepBasic_ConversionBasedUnit)::DownCast(unit);
    if (conversion.IsNull()) { return !Handle(StepBasic_SiUnit)::DownCast(unit).IsNull(); }
    auto measure = conversion->ConversionFactor();
    return !measure.IsNull() && std::isfinite(measure->ValueComponent())
        && measure->ValueComponent() > 0 && validUnit(measure->UnitComponent().NamedUnit(), depth + 1);
}
void value(const Handle(Standard_Transient)& sourceItem,
    const Handle(StepData_StepModel)& model, StepAttributeRecord& record,
    const std::map<std::string, std::string>& bad, const std::string& unknownUnits,
    const std::map<std::string, StepAttributeScalar>& scalars)
{
    auto item = Handle(StepRepr_RepresentationItem)::DownCast(sourceItem);
    auto& fields = record.strings;
    fields["SourceType"] = item.IsNull() ? "missing representation item" : item->DynamicType()->Name();
    fields["SupplierName"] = item.IsNull() ? "" : text(item->Name());
    fields["SourceValue"] = "";
    fields["SourceUnit"] = "";
    auto diagnostic = bad.find(id(model, sourceItem));
    if (diagnostic != bad.end()) { fields["SourceFailure"] = diagnostic->second; }
    auto scalar = scalars.find(id(model, sourceItem));
    if (scalar != scalars.end()) { fields["SourceToken"] = scalar->second.token; }
    auto undefined = Handle(StepData_UndefinedEntity)::DownCast(sourceItem);
    if (!undefined.IsNull()) {
        fields["SupplierName"] = undefinedName(undefined);
        fields["SourceType"] = literal(undefined, model);
        fields["SourceValue"] = scalar == scalars.end() ? "" : scalar->second.token;
        fields["ValueReason"] = "unsupported source type";
        return;
    }
    auto descriptive = Handle(StepRepr_DescriptiveRepresentationItem)::DownCast(item);
    if (!descriptive.IsNull()) {
        if (descriptive->Description().IsNull()) {
            fields["ValueReason"] = "missing descriptive value";
        }
        else {
            record.kind = "String";
            fields["SourceValue"] = text(descriptive->Description());
        }
        return;
    }
    auto simple = Handle(StepRepr_MeasureRepresentationItem)::DownCast(item);
    auto combined = Handle(StepRepr_ReprItemAndMeasureWithUnit)::DownCast(item);
    Handle(StepBasic_MeasureWithUnit) measure;
    if (!simple.IsNull()) { measure = simple->Measure(); }
    if (!combined.IsNull()) { measure = combined->GetMeasureWithUnit(); }
    if (measure.IsNull() || measure->ValueComponentMember().IsNull()) {
        fields["ValueReason"] = "unsupported array, reference or source type";
        return;
    }
    std::string type = measure->ValueComponentMember()->Name();
    fields["SourceType"] += " " + type;
    fields["SourceValue"] = number(measure->ValueComponent());
    auto unit = measure->UnitComponent().NamedUnit();
    fields["SourceUnit"] = unitDescription(unit, model);
    if (unit.IsNull() && !unknownUnits.empty()) {
        fields["SourceUnit"] += "; unassociated unsupported declarations: " + unknownUnits;
    }
    if (scalar == scalars.end()) {
        fields["ValueReason"] = "source scalar unavailable for verification";
        return;
    }
    if (!scalar->second.reason.empty() || scalar->second.type != type
        || scalar->second.value != measure->ValueComponent()) {
        fields["SourceValue"] = scalar->second.token;
        fields["ValueReason"] = scalar->second.reason.empty()
            ? "source scalar differs from typed measure" : scalar->second.reason;
        return;
    }
    if (type != "LENGTH_MEASURE" && type != "POSITIVE_LENGTH_MEASURE"
        && type != "PLANE_ANGLE_MEASURE" && type != "POSITIVE_PLANE_ANGLE_MEASURE") {
        fields["ValueReason"] = "unsupported measure type " + type;
        return;
    }
    if (!validUnit(unit)) {
        fields["ValueReason"] = "unknown or invalid explicit unit";
        return;
    }
    if (diagnostic != bad.end()) {
        fields["ValueReason"] = "malformed source item";
        return;
    }
    STEPConstruct_UnitContext context;
    if (context.ComputeFactors(unit) != 0) {
        fields["ValueReason"] = "unsupported explicit unit";
        return;
    }
    const bool angle = type.find("PLANE_ANGLE") != std::string::npos;
    double factor = angle ? context.PlaneAngleFactor() * 180 / M_PI
        : context.LengthFactor() * StepData_GlobalFactors::Intance().CascadeUnit(); // codespell:ignore intance
    if ((angle ? !context.PlaneAngleDone() || context.LengthDone()
               : !context.LengthDone() || context.PlaneAngleDone())
        || !std::isfinite(factor) || factor <= 0) {
        fields["ValueReason"] = "conflicting measure type and unit";
        return;
    }
    record.value = measure->ValueComponent() * factor;
    if (!std::isfinite(record.value)) {
        fields["ValueReason"] = "nonfinite converted measure";
        record.value = 0;
        return;
    }
    record.kind = angle ? "Angle" : "Length";
}
}  // namespace

void StepAttributesImport::captureSourceScalars(const Base::FileInfo& file) noexcept
{
    try { scalars = sourceScalars(file); }
    catch (...) { scalars.clear(); }
}

void StepAttributesImport::capture(const STEPCAFControl_Reader& reader,
    const Handle(TDocStd_Document)& source) noexcept
{
    try {
        const auto model = reader.Reader().StepModel();
        const auto process = reader.Reader().WS()->TransferReader()->TransientProcess();
        const auto& labelMap = reader.GetShapeLabelMap();
        std::map<std::string, std::string> bad;
        std::string unknownUnits;
        std::map<const Standard_Transient*, std::vector<Handle(StepRepr_NextAssemblyUsageOccurrence)>> parents;
        std::set<const Standard_Transient*> assemblies;
        std::map<const Standard_Transient*, std::string> labels;
        std::map<std::string, std::set<const Standard_Transient*>> definitions, occurrences;
        for (auto reports = model->Reports(); reports.More(); reports.Next()) {
            auto report = Handle(Interface_ReportEntity)::DownCast(reports.Value());
            if (!report.IsNull() && report->Check()->HasFailed()) {
                bad[id(model, report->Concerned())] = literal(report->Content(), model);
            }
        }
        for (int i = 1; i <= model->NbEntities(); ++i) {
            auto entity = model->Value(i);
            auto unknown = Handle(StepData_UndefinedEntity)::DownCast(entity);
            if (!unknown.IsNull()) {
                auto declaration = literal(unknown, model);
                if (declaration.find("_UNIT") != std::string::npos) {
                    unknownUnits += id(model, entity) + " " + declaration + "; ";
                }
            }
            auto occurrence = Handle(StepRepr_NextAssemblyUsageOccurrence)::DownCast(entity);
            if (!occurrence.IsNull()) {
                parents[occurrence->RelatedProductDefinition().get()].push_back(occurrence);
                assemblies.insert(occurrence->RelatingProductDefinition().get());
            }
            // This is the exact transfer binder's map key, never ShapeTool::Search,
            // name matching or geometric equality. Refuse non-injective PD maps below.
            if (!Handle(StepBasic_ProductDefinition)::DownCast(entity).IsNull()
                || !occurrence.IsNull()) {
                auto binder = process->Find(entity);
                if (!binder.IsNull() && binder->HasResult()) {
                    auto shape = TransferBRep::ShapeResult(process, binder);
                    if (!shape.IsNull() && labelMap.IsBound(shape)) {
                        auto label = labelMap.Find(shape);
                        labels[entity.get()] = entry(label);
                        if (occurrence.IsNull()) { definitions[entry(label)].insert(entity.get()); }
                        else { occurrences[entry(label)].insert(entity.get()); }
                    }
                }
            }
        }
        for (int i = 1; i <= model->NbEntities(); ++i) {
            auto chain = Handle(StepRepr_PropertyDefinitionRepresentation)::DownCast(model->Value(i));
            if (chain.IsNull() || !Handle(StepShape_ShapeDefinitionRepresentation)::DownCast(chain).IsNull()) {
                continue;
            }
            auto property = chain->Definition().PropertyDefinition();
            try {
                if (property.IsNull()) { throw std::runtime_error("missing property definition"); }
                auto rep = chain->UsedRepresentation();
                if (rep.IsNull()) { throw std::runtime_error("missing property representation"); }
                auto scope = property->Definition();
                // Exclude standard material/validation chains and all semantic PMI
                // owners/representations. A descriptive item alone does not imply material.
                const auto name = text(property->Name());
                const auto repName = text(rep->Name());
                if (name == "material property" || name == "geometric validation property"
                    || repName == "material name" || repName == "density"
                    || !scope.ShapeAspect().IsNull() || !scope.ShapeAspectRelationship().IsNull()
                    || rep->DynamicType() != STANDARD_TYPE(StepRepr_Representation)) {
                    continue;
                }
                auto owner = scope.Value();
                auto pds = scope.ProductDefinitionShape();
                if (!pds.IsNull()) { owner = pds->Definition().Value(); }
                auto pd = Handle(StepBasic_ProductDefinition)::DownCast(owner);
                auto occurrence = Handle(StepRepr_NextAssemblyUsageOccurrence)::DownCast(owner);
                std::string ownerScope = "unsupported owner", ownerReason, path;
                if (!pd.IsNull()) {
                    ownerScope = assemblies.count(pd.get()) && parents[pd.get()].empty()
                        ? "root assembly" : "definition";
                    if (!parents[pd.get()].empty()) {
                        ownerReason = "definition used by occurrences; exact definition object unavailable";
                    }
                }
                else if (!occurrence.IsNull()) {
                    ownerScope = "occurrence";
                    auto parent = occurrence->RelatingProductDefinition();
                    if (parent.IsNull() || !parents[parent.get()].empty()) {
                        ownerReason = "complete occurrence path unavailable (nested or repeated parent)";
                    }
                    else { path = id(model, parent) + "/" + id(model, occurrence); }
                }
                else { ownerReason = "unsupported or external owner"; }
                std::string label = labels[owner.get()];
                if (!reader.ExternFiles().IsEmpty()) {
                    ownerReason = "external owner correspondence unavailable";
                }
                if (!pd.IsNull() && definitions[label].size() > 1) {
                    ownerReason = "ambiguous product definitions share transfer label";
                }
                if (!occurrence.IsNull() && occurrences[label].size() > 1) {
                    ownerReason = "ambiguous occurrence transfer label";
                }
                if (label.empty() && ownerReason.empty()) { ownerReason = "omitted or unknown exact owner"; }
                for (int j = 1; j <= rep->NbItems(); ++j) {
                    auto item = originalItem(model, rep, j);
                    StepAttributeRecord record;
                    record.order = j;
                    record.ownerLabel = label;
                    record.strings = {{"SourceId", id(model, chain) + "/" + id(model, item) + "/" + std::to_string(j)},
                        {"PropertySet", name}, {"PropertyDescription", text(property->Description())},
                        {"RepresentationName", repName}, {"PropertyDefinitionId", id(model, property)},
                        {"RepresentationId", id(model, rep)}, {"ContextId", id(model, rep->ContextOfItems())},
                        {"ContextIdentifier", rep->ContextOfItems().IsNull() ? "" : text(rep->ContextOfItems()->ContextIdentifier())},
                        {"ContextType", rep->ContextOfItems().IsNull() ? "" : text(rep->ContextOfItems()->ContextType())},
                        {"OwnerSourceId", id(model, owner)}, {"OwnerScope", ownerScope},
                        {"OccurrencePath", path}, {"OwnerReason", ownerReason}, {"ValueReason", ""}};
                    try { value(item, model, record, bad, unknownUnits, scalars); }
                    catch (...) { record.strings["ValueReason"] = "malformed property value"; record.kind.clear(); }
                    records.push_back(std::move(record));
                }
            }
            catch (...) {
                StepAttributeRecord record;
                record.strings = {{"SourceId", id(model, chain)}, {"SupplierName", ""},
                    {"SourceType", chain->DynamicType()->Name()}, {"SourceValue", ""}, {"SourceUnit", ""},
                    {"ValueReason", "malformed property chain"}, {"OwnerReason", "malformed property owner"}};
                records.push_back(std::move(record));
            }
        }
        (void)source; // Source lifetime ends before materialization; no handles retained.
    }
    catch (...) { failed = true; }
}

ShapeImportObserver StepAttributesImport::observer()
{
    return [this](const TDF_Label& label, const TopoDS_Shape&, App::DocumentObject* object) {
        try {
            if (object && !label.IsNull()) {
                owners[entry(label)].push_back({object->getDocument(), object->getNameInDocument(),
                    XCAFDoc_ShapeTool::IsReference(label)});
            }
        }
        catch (...) { failed = true; }
    };
}

void StepAttributesImport::finish(App::Document* target, bool supported) const noexcept
{
    try {
        if (failed || !target || records.empty()) { return; }
        Py::List data;
        for (auto record : records) {
            auto& reason = record.strings["OwnerReason"];
            if (!supported) { reason = "merged, reduced or multi-document owner unavailable; " + reason; }
            std::set<std::string> matches;
            if (reason.empty()) {
                auto found = owners.find(record.ownerLabel);
                if (found != owners.end()) {
                    for (auto& owner : found->second) {
                        if (owner.document == target && target->getObject(owner.name.c_str())
                            && owner.occurrence == (record.strings["OwnerScope"] == "occurrence")) {
                            matches.insert(owner.name);
                        }
                    }
                }
                if (matches.size() != 1) {
                    reason = matches.empty() ? "omitted, ambiguous or reduced exact owner" : "ambiguous exact owner";
                }
            }
            record.strings["ObjectName"] = reason.empty() ? *matches.begin() : "";
            Py::Dict item;
            for (auto& field : record.strings) { item[field.first] = Py::String(field.second); }
            item["ValueKind"] = Py::String(record.kind);
            item["value"] = Py::Float(record.value);
            item["SourceOrder"] = Py::Long(record.order);
            data.append(item);
        }
        Py::Module module(PyImport_ImportModule("StepAttributes"), true);
        Py::Tuple args(2);
        args.setItem(0, Py::asObject(target->getPyObject()));
        args.setItem(1, data);
        Py::Callable(module.getAttr("create")).apply(args);
    }
    catch (...) {
        PyErr_Clear();
        Base::Console().warning("STEP attributes: metadata unavailable; geometry/PMI/views kept.\n");
    }
}
}  // namespace Import
