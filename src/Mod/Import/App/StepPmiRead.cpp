// SPDX-License-Identifier: LGPL-2.1-or-later
#include "StepPmiImport.h"

#include <Interface_Check.hxx>
#include <Interface_ReportEntity.hxx>
#include <Interface_UndefinedContent.hxx>
#include <StepData_StepModel.hxx>
#include <StepData_UndefinedEntity.hxx>

namespace Import
{
namespace
{
void enumLiterals(const Handle(StepData_UndefinedEntity)& entity,
                  std::string& result, int depth = 0)
{
    if (entity.IsNull() || depth > 8) {
        return;
    }
    const auto content = entity->UndefinedContent();
    for (int i = 1; i <= content->NbParams(); ++i) {
        if (content->ParamType(i) == Interface_ParamSub && content->IsParamEntity(i)) {
            enumLiterals(Handle(StepData_UndefinedEntity)::DownCast(content->ParamEntity(i)),
                         result, depth + 1);
        }
        else if (content->ParamType(i) == Interface_ParamEnum && !content->IsParamEntity(i)) {
            if (!result.empty()) {
                result += ", ";
            }
            result += content->ParamValue(i)->ToCString();
        }
    }
}
}  // namespace

void StepPmiImport::captureReadWarnings(const Handle(StepData_StepModel)& model) noexcept
{
    // The reader's work session clears model diagnostics when destroyed. Copy
    // owned strings while it is alive; even its model handle cannot retain them.
    try {
        sourceWarnings = readWarnings(model);
    }
    catch (...) {
        // Defer failure until extraction, after geometry conversion has finished.
        sourceWarningsFailed = true;
    }
}

std::vector<std::string> StepPmiImport::readWarnings(
    const Handle(StepData_StepModel)& sourceModel)
{
    std::vector<std::string> warnings;
    if (sourceModel.IsNull()) {
        return warnings;
    }
    // XCAF cannot retain unknown STEP enumeration literals. Use OCCT's owned
    // error content, not a second STEP text parser, to keep those codes visible.
    for (int i = 1; i <= sourceModel->NbEntities(); ++i) {
        const auto report = sourceModel->ReportEntity(i);
        if (report.IsNull() || !report->Check()->HasFailed()) {
            continue;
        }
        auto entity = Handle(StepData_UndefinedEntity)::DownCast(report->Content());
        for (; !entity.IsNull(); entity = entity->Next()) {
            const std::string type = entity->StepType();
            if (type.find("MODIFIER") == std::string::npos) {
                continue;
            }
            std::string codes;
            enumLiterals(entity, codes);
            if (!codes.empty()) {
                warnings.push_back("File-level unmapped STEP modifier data in #"
                    + std::to_string(sourceModel->IdentLabel(sourceModel->Value(i))) + ": "
                    + codes + "; OCCT could not decode " + type
                    + ". Imported modifiers may be incomplete.");
            }
        }
    }
    return warnings;
}
}  // namespace Import
