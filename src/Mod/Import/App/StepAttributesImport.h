// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "StepPmiImport.h"

class STEPCAFControl_Reader;

namespace Import
{
// Owned values only: no source model, label, binder or shape survives capture.
struct StepAttributeRecord
{
    std::map<std::string, std::string> strings;
    std::string ownerLabel;
    std::string kind;
    double value = 0;
    int order = 0;
};

struct StepAttributeScalar
{
    std::string type;
    std::string token;
    std::string reason;
    double value = 0;
};

class ImportExport StepAttributesImport
{
public:
    void captureSourceScalars(const Base::FileInfo& file) noexcept;
    void capture(const STEPCAFControl_Reader& reader,
                 const Handle(TDocStd_Document)& source) noexcept;
    ShapeImportObserver observer();
    void discard() noexcept { failed = true; }
    void finish(App::Document* target, bool supported) const noexcept;

private:
    struct Owner
    {
        App::Document* document;
        std::string name;
        bool occurrence;
    };
    std::map<std::string, std::vector<Owner>> owners;
    std::vector<StepAttributeRecord> records;
    std::map<std::string, StepAttributeScalar> scalars;
    bool failed = false;
};
}  // namespace Import
