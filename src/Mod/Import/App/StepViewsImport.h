// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "StepPmiImport.h"

class STEPCAFControl_Reader;

namespace Import
{
struct StepViewReference
{
    std::string sourceId;
    std::string role;
    std::string scope;
    std::string objectName;
    std::string reason;
};

// Only owned values escape capture(). No source labels, shapes or handles are retained.
struct StepViewRecord
{
    std::map<std::string, std::string> strings;
    std::map<std::string, double> numbers;
    std::map<std::string, bool> flags;
    std::map<std::string, std::vector<double>> vectors;
    std::vector<std::string> planes;
    std::vector<StepViewReference> references;
};

class ImportExport StepViewsImport
{
public:
    void capture(const STEPCAFControl_Reader& reader,
                 const Handle(TDocStd_Document)& source) noexcept;
    ShapeImportObserver observer();
    void discard() noexcept { failed = true; }
    void finish(App::Document* target, bool supported,
                const std::vector<std::string>& pmiObjects) const noexcept;

private:
    struct Owner
    {
        App::Document* document;
        std::string name;
        bool occurrence;
    };
    std::map<std::string, std::vector<Owner>> owners;
    std::vector<StepViewRecord> records;
    bool failed = false;
};
}  // namespace Import
