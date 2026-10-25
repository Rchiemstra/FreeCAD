// SPDX-License-Identifier: LGPL-2.1-or-later
#include "StepMetadataImport.h"

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentObjectGroup.h>
#include <Base/Console.h>
#include <Base/FileInfo.h>

using namespace Import;

StepMetadataImport::StepMetadataImport(const Base::FileInfo& file, int semanticPmi, int savedViews)
    : pmi(StepPmiImport::create(file, semanticPmi))
{
    try {
        if (file.hasExtension({"step", "stp"}) && (savedViews < 0
                ? App::GetApplication().GetParameterGroupByPath(
                    "User parameter:BaseApp/Preferences/Mod/Import")
                    ->GetBool("ImportSavedViews", false) : savedViews != 0)) {
            views = std::make_unique<StepViewsImport>();
        }
    }
    catch (...) {
        Base::Console().warning("STEP saved views: initialization unavailable; geometry kept.\n");
    }
}

void StepMetadataImport::finish(const Base::FileInfo& file,
    const Handle(TDocStd_Document)& source, App::Document* target, bool supported) const noexcept
{
    auto pmiGroup = pmi ? dynamic_cast<App::DocumentObjectGroup*>(
        pmi->finish(file, source, target, supported)) : nullptr;
    if (views) {
        try {
            // Only members of the exact group returned by this PMI import may bind.
            std::vector<std::string> createdPmi;
            if (pmiGroup) {
                for (auto object : pmiGroup->getObjects()) {
                    createdPmi.emplace_back(object->getNameInDocument());
                }
            }
            views->finish(target, supported, createdPmi);
        }
        catch (...) {
            PyErr_Clear();
            Base::Console().warning("STEP saved views: associations unavailable; geometry/PMI kept.\n");
        }
    }
}
