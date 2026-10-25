// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "StepViewsImport.h"

namespace Import
{
// One observer slot, composed here so independent metadata options cannot replace PMI.
class ImportExport StepMetadataImport
{
public:
    StepMetadataImport(const Base::FileInfo& file, int semanticPmi, int savedViews);
    StepPmiImport* pmiReader() const
    {
        return pmi.get();
    }
    StepViewsImport* viewsReader() const
    {
        return views.get();
    }
    template<class Converter>
    void attach(Converter& converter) noexcept
    {
        if (!views) {
            if (pmi) {
                pmi->attach(converter);
            }
            return;
        }
        converter.setModelShapesOnly(true);
        try {
            auto first = pmi ? pmi->observer() : ShapeImportObserver();
            auto second = views->observer();
            converter.setShapeObserver([first, second](const TDF_Label& label,
                const TopoDS_Shape& shape, App::DocumentObject* object) {
                if (first) {
                    first(label, shape, object);
                }
                second(label, shape, object);
            });
        }
        catch (...) {
            views->discard();
            if (pmi) {
                pmi->attach(converter);
            }
        }
    }
    void finish(const Base::FileInfo& file, const Handle(TDocStd_Document)& source,
                App::Document* target, bool supported) const noexcept;

private:
    std::unique_ptr<StepPmiImport> pmi;
    std::unique_ptr<StepViewsImport> views;
};
}  // namespace Import
