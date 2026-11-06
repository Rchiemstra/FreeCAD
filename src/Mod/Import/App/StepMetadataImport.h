// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "StepViewsImport.h"
#include "StepAttributesImport.h"

namespace Import
{
// One observer slot, composed here so independent metadata options cannot replace PMI.
class ImportExport StepMetadataImport
{
public:
    StepMetadataImport(const Base::FileInfo& file, int semanticPmi, int savedViews, int userAttributes = -1);
    StepPmiImport* pmiReader() const
    {
        return pmi.get();
    }
    StepViewsImport* viewsReader() const
    {
        return views.get();
    }
    StepAttributesImport* attributesReader() const { return attributes.get(); }
    template<class Converter>
    void attach(Converter& converter) noexcept
    {
        if (!views && !attributes) {
            if (pmi) {
                pmi->attach(converter);
            }
            return;
        }
        converter.setModelShapesOnly(true);
        try {
            auto first = pmi ? pmi->observer() : ShapeImportObserver();
            auto second = views ? views->observer() : ShapeImportObserver();
            auto third = attributes ? attributes->observer() : ShapeImportObserver();
            converter.setShapeObserver([first, second, third](const TDF_Label& label,
                const TopoDS_Shape& shape, App::DocumentObject* object) {
                if (first) {
                    first(label, shape, object);
                }
                if (second) { second(label, shape, object); }
                if (third) { third(label, shape, object); }
            });
        }
        catch (...) {
            if (views) { views->discard(); }
            if (attributes) { attributes->discard(); }
            if (pmi) {
                pmi->attach(converter);
            }
        }
    }
    void finish(const Base::FileInfo& file, const Handle(TDocStd_Document)& source,
                App::Document* target, bool supported, bool attributesSupported) const noexcept;

private:
    std::unique_ptr<StepPmiImport> pmi;
    std::unique_ptr<StepViewsImport> views;
    std::unique_ptr<StepAttributesImport> attributes;
};
}  // namespace Import
