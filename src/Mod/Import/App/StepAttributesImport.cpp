// SPDX-License-Identifier: LGPL-2.1-or-later
#include "StepAttributesImport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <locale>
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
#include <StepBasic_ConversionBasedUnitAndLengthUnit.hxx>
#include <StepBasic_ConversionBasedUnitAndPlaneAngleUnit.hxx>
#include <StepBasic_DimensionalExponents.hxx>
#include <StepBasic_MeasureValueMember.hxx>
#include <StepBasic_MeasureWithUnit.hxx>
#include <StepBasic_ProductDefinition.hxx>
#include <StepBasic_SiUnit.hxx>
#include <StepBasic_SiUnitAndLengthUnit.hxx>
#include <StepBasic_SiUnitAndPlaneAngleUnit.hxx>
#include <Standard_Version.hxx>
#if OCC_VERSION_HEX >= 0x080000
#include <StepData_Factors.hxx>
#else
#include <StepData_GlobalFactors.hxx>
#endif
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
TYPESYSTEM_SOURCE(Import::PropertyUnwrappedAngle, App::PropertyQuantity)

PropertyUnwrappedAngle::PropertyUnwrappedAngle()
{
    setUnit(Base::Unit::Angle);
}
App::Property* PropertyUnwrappedAngle::Copy() const
{
    auto copy = new PropertyUnwrappedAngle();
    copy->setValue(getValue());
    return copy;
}

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
    std::string raw, mask, numericMask;
    bool quoted = false, comment = false, oversized = false;
    std::set<std::string> seen;
    auto spaces = [](const std::string& text, size_t& pos) {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) { ++pos; }
    };
    auto digits = [](const std::string& text, size_t& pos) {
        const auto begin = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') { ++pos; }
        return pos != begin;
    };
    auto append = [&](char value, bool hidden, bool numericHidden = false) {
        if (raw.size() < 65536) {
            raw += value;
            mask += hidden ? ' ' : value;
            // Hide comments only. Quoted text must never become numeric whitespace.
            numericMask += numericHidden ? ' ' : value;
        }
        else { oversized = true; }
    };
    auto parse = [&](const std::string& token, double& value) {
        size_t pos = 0;
        spaces(token, pos);
        if (pos < token.size() && (token[pos] == '+' || token[pos] == '-')) { ++pos; }
        bool mantissa = digits(token, pos);
        if (pos < token.size() && token[pos] == '.') {
            ++pos;
            mantissa = digits(token, pos) || mantissa;
        }
        bool exponent = true;
        if (pos < token.size() && (token[pos] == 'E' || token[pos] == 'e')) {
            ++pos;
            if (pos < token.size() && (token[pos] == '+' || token[pos] == '-')) { ++pos; }
            exponent = digits(token, pos);
        }
        spaces(token, pos);
        if (!mantissa || !exponent || pos != token.size()) {
            return std::string("malformed source numeric token");
        }
        std::istringstream number(token);
        number.imbue(std::locale::classic());
        number >> value;
        return !number || !std::isfinite(value)
            ? std::string("nonfinite or out-of-range source numeric token") : std::string();
    };
    auto finish = [&](bool complete) {
        size_t pos = 0;
        spaces(mask, pos);
        if (pos == mask.size() || mask[pos++] != '#') { return; }
        const auto beginId = pos;
        if (!digits(mask, pos)) { return; }
        auto identity = mask.substr(beginId, pos - beginId);
        spaces(mask, pos);
        if (pos == mask.size() || mask[pos] != '=') { return; }
        const auto significant = identity.find_first_not_of('0');
        identity = significant == std::string::npos ? "0" : identity.substr(significant);
        const auto key = "#" + identity;
        if (!seen.insert(key).second) {
            result[key].reason = "duplicate source entity ID";
            return;
        }
        auto& scalar = result[key];
        if (!complete) { scalar.reason = "incomplete source entity"; }
        else if (oversized) { scalar.reason = "source entity exceeds scalar validation limit"; }
        else {
            // Iterative token scans keep long, legal comments below the size
            // limit independent of the regular-expression implementation's stack.
            constexpr const char* measure = "MEASURE_WITH_UNIT";
            size_t search = 0;
            int count = 0;
            while ((search = mask.find(measure, search)) != std::string::npos) {
                size_t cursor = search + std::char_traits<char>::length(measure);
                search = cursor;
                spaces(mask, cursor);
                if (cursor == mask.size() || mask[cursor++] != '(') { continue; }
                spaces(mask, cursor);
                const auto typeBegin = cursor;
                while (cursor < mask.size() && ((mask[cursor] >= 'A' && mask[cursor] <= 'Z')
                    || mask[cursor] == '_' || (cursor > typeBegin && mask[cursor] >= '0' && mask[cursor] <= '9'))) {
                    ++cursor;
                }
                const auto type = mask.substr(typeBegin, cursor - typeBegin);
                spaces(mask, cursor);
                if (type.empty() || cursor == mask.size() || mask[cursor++] != '(') { continue; }
                const auto close = mask.find(')', cursor);
                if (close == std::string::npos || mask.find('(', cursor) < close) { continue; }
                scalar.type = type;
                scalar.token = raw.substr(cursor, close - cursor);
                scalar.reason = parse(numericMask.substr(cursor, close - cursor), scalar.value);
                if (++count > 1) { scalar.reason = "ambiguous source scalar tokens"; }
            }
            constexpr const char* dimensions = "DIMENSIONAL_EXPONENTS";
            auto dim = mask.find(dimensions);
            if (dim != std::string::npos) {
                dim += std::char_traits<char>::length(dimensions);
                spaces(mask, dim);
                if (dim < mask.size() && mask[dim++] == '(') {
                    const auto close = mask.find(')', dim);
                    if (close != std::string::npos && mask.find('(', dim) > close) {
                        scalar.type = "DIMENSIONAL_EXPONENTS";
                        scalar.token = raw.substr(dim, close - dim);
                        std::vector<double> components;
                        while (dim <= close) {
                            const auto end = std::min(numericMask.find(',', dim), close);
                            double component = 0;
                            auto reason = parse(numericMask.substr(dim, end - dim), component);
                            if (!reason.empty()) { scalar.reason = reason; }
                            components.push_back(component);
                            dim = end + 1;
                        }
                        if (components.size() != 7) { scalar.reason = "invalid dimensional exponents"; }
                        else {
                            // Both supported dimensions require the other six
                            // source exponents to be exactly zero. Preserve the
                            // length exponent for comparison with the typed unit.
                            scalar.value = components.front();
                            if (std::any_of(components.begin() + 1, components.end(),
                                [](double exponent) { return exponent != 0; })) {
                                scalar.reason = "unsupported dimensional exponents";
                            }
                        }
                    }
                }
            }
        }
    };
    char c;
    while (input.get(c)) {
        if (comment) {
            append(c, true, true);
            if (c == '*' && input.peek() == '/') {
                input.get(c); append(c, true, true); comment = false;
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
            append(c, true, true); input.get(c); append(c, true, true); comment = true; continue;
        }
        if (c == '\'') { append(c, true); quoted = true; continue; }
        append(c, false);
        if (c != ';') { continue; }
        finish(true);
        raw.clear(); mask.clear(); numericMask.clear(); oversized = false;
    }
    if (!raw.empty()) { finish(false); }
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
    const Handle(StepData_StepModel)& model,
    const std::map<std::string, StepAttributeScalar>& scalars, int depth = 0)
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
#if OCC_VERSION_HEX >= 0x080000
        auto measure = Handle(StepBasic_MeasureWithUnit)::DownCast(conversion->ConversionFactor());
        auto combined = Handle(StepRepr_ReprItemAndMeasureWithUnit)
            ::DownCast(conversion->ConversionFactor());
        if (!combined.IsNull()) { measure = combined->GetMeasureWithUnit(); }
#else
        auto measure = conversion->ConversionFactor();
#endif
        if (!measure.IsNull()) {
            result += " factor=" + id(model, measure) + " " + number(measure->ValueComponent());
            auto source = scalars.find(id(model, measure));
            if (source != scalars.end()) { result += " token=" + source->second.token; }
            result += " " + unitDescription(measure->UnitComponent().NamedUnit(), model, scalars, depth + 1);
        }
    }
    return result;
}
// Validate every link against owned source tokens and reader diagnostics before
// conversion. OCCT's conversion routine only handles a single conversion hop;
// multiply verified hops here and ask it only for the terminal SI factor.
std::string unitFactor(Handle(StepBasic_NamedUnit) unit, bool angle,
    const Handle(StepData_StepModel)& model,
    const std::map<std::string, std::string>& bad,
    const std::map<std::string, StepAttributeScalar>& scalars, double& factor)
{
    factor = 1;
    std::set<const Standard_Transient*> visited;
    for (int depth = 0; depth <= 16; ++depth) {
        if (unit.IsNull()) { return "missing explicit unit in conversion chain"; }
        const auto key = id(model, unit);
        auto fail = [&](const std::string& reason) { return key + ": " + reason; };
        if (!visited.insert(unit.get()).second) { return fail("cyclic unit chain"); }
        if (bad.count(key)) { return fail("reader diagnostic on unit"); }
        auto source = scalars.find(key);
        if (source == scalars.end()) { return fail("source unit unavailable for verification"); }
        if (!source->second.reason.empty()) { return fail(source->second.reason); }
        auto conversion = Handle(StepBasic_ConversionBasedUnit)::DownCast(unit);
        auto si = Handle(StepBasic_SiUnit)::DownCast(unit);
        if (!si.IsNull()) {
            const bool correct = angle
                ? !Handle(StepBasic_SiUnitAndPlaneAngleUnit)::DownCast(unit).IsNull()
                : !Handle(StepBasic_SiUnitAndLengthUnit)::DownCast(unit).IsNull();
            STEPConstruct_UnitContext context;
#if OCC_VERSION_HEX >= 0x080000
            StepData_Factors localFactors;
            localFactors.SetCascadeUnit(model->LocalLengthUnit());
            if (!correct || context.ComputeFactors(si, localFactors) != 0
#else
            if (!correct || context.ComputeFactors(si) != 0
#endif
                || (angle ? !context.PlaneAngleDone() || context.LengthDone()
                          : !context.LengthDone() || context.PlaneAngleDone())) {
                return fail("conflicting measure type and SI unit");
            }
#if OCC_VERSION_HEX >= 0x080000
            // Lengths are stored in mm and angles in degrees, independent of the model's unit.
            factor *= angle ? context.PlaneAngleFactor() * 180 / M_PI
                : context.LengthFactor() * localFactors.CascadeUnit();
#else
            factor *= angle ? context.PlaneAngleFactor() * 180 / M_PI
                : context.LengthFactor() * StepData_GlobalFactors::Intance().CascadeUnit(); // codespell:ignore intance
#endif
            return std::isfinite(factor) && factor > 0 ? "" : fail("invalid converted unit factor");
        }
        if (conversion.IsNull()) { return fail("unsupported explicit unit"); }
        const bool correct = angle
            ? !Handle(StepBasic_ConversionBasedUnitAndPlaneAngleUnit)::DownCast(unit).IsNull()
            : !Handle(StepBasic_ConversionBasedUnitAndLengthUnit)::DownCast(unit).IsNull();
        if (!correct) { return fail("conflicting conversion unit dimension"); }
        auto dims = unit->Dimensions();
        if (dims.IsNull()) { return fail("missing conversion dimensions"); }
        const auto dimKey = id(model, dims);
        auto dimSource = scalars.find(dimKey);
        if (bad.count(dimKey) || dimSource == scalars.end()
            || !dimSource->second.reason.empty() || dimSource->second.type != "DIMENSIONAL_EXPONENTS") {
            return fail(dimKey + ": unverifiable conversion dimensions");
        }
        const double typed[] = {dims->LengthExponent(), dims->MassExponent(), dims->TimeExponent(),
            dims->ElectricCurrentExponent(), dims->ThermodynamicTemperatureExponent(),
            dims->AmountOfSubstanceExponent(), dims->LuminousIntensityExponent()};
        for (int i = 0; i < 7; ++i) {
            const double expected = !angle && i == 0 ? 1 : 0;
            const double source = i == 0 ? dimSource->second.value : 0;
            if (source != typed[i] || typed[i] != expected) {
                return fail(dimKey + ": conflicting conversion dimensions");
            }
        }
#if OCC_VERSION_HEX >= 0x080000
        auto measure = Handle(StepBasic_MeasureWithUnit)::DownCast(conversion->ConversionFactor());
        auto combined = Handle(StepRepr_ReprItemAndMeasureWithUnit)
            ::DownCast(conversion->ConversionFactor());
        if (!combined.IsNull()) { measure = combined->GetMeasureWithUnit(); }
#else
        auto measure = conversion->ConversionFactor();
#endif
        if (measure.IsNull()) { return fail("missing conversion factor"); }
        const auto measureKey = id(model, measure);
        auto scalar = scalars.find(measureKey);
        if (bad.count(measureKey) || scalar == scalars.end()
            || !scalar->second.reason.empty() || measure->ValueComponentMember().IsNull()) {
            return fail(measureKey + ": unverifiable conversion factor"
                + (scalar == scalars.end() ? "" : ": " + scalar->second.reason));
        }
        const std::string type = measure->ValueComponentMember()->Name();
        const std::string expected = angle ? "PLANE_ANGLE_MEASURE" : "LENGTH_MEASURE";
        if ((type != expected && type != "POSITIVE_" + expected)
            || scalar->second.type != type || scalar->second.value != measure->ValueComponent()
            || !std::isfinite(measure->ValueComponent()) || measure->ValueComponent() <= 0) {
            return fail(measureKey + ": invalid conversion factor type or value");
        }
        factor *= measure->ValueComponent();
        if (!std::isfinite(factor) || factor <= 0) { return fail(measureKey + ": invalid conversion product"); }
        unit = measure->UnitComponent().NamedUnit();
    }
    return "excessive unit chain (limit 16)";
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
    if (scalar != scalars.end() && scalar->second.reason == "duplicate source entity ID") {
        fields["ValueReason"] = scalar->second.reason;
        fields["SourceValue"] = scalar->second.token;
        return;
    }
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
    fields["SourceUnit"] = unitDescription(unit, model, scalars);
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
    if (diagnostic != bad.end()) {
        fields["ValueReason"] = "malformed source item";
        return;
    }
    const bool angle = type.find("PLANE_ANGLE") != std::string::npos;
    double factor = 0;
    auto reason = unitFactor(unit, angle, model, bad, scalars, factor);
    if (!reason.empty()) {
        fields["ValueReason"] = reason;
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
                // Duplicate/partial source identities cannot establish an exact
                // association, even if OCCT transferred one of the declarations.
                std::vector<Handle(Standard_Transient)> ownerChain {owner, pds, property, chain, rep};
                if (!occurrence.IsNull()) { ownerChain.push_back(occurrence->RelatingProductDefinition()); }
                for (const auto& entity : ownerChain) {
                    if (entity.IsNull()) { continue; }
                    const auto key = id(model, entity);
                    auto sourceScalar = scalars.find(key);
                    if (sourceScalar != scalars.end() && !sourceScalar->second.reason.empty()) {
                        ownerReason = key + ": " + sourceScalar->second.reason;
                        break;
                    }
                    if (entity != rep && bad.count(key)) { ownerReason = key + ": malformed source owner chain"; }
                }
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
