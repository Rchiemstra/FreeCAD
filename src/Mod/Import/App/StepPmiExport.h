// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <CXX/Objects.hxx>
#include <functional>
#include <map>
#include <vector>
#include <TDF_Label.hxx>
#include <TDocStd_Document.hxx>
#include <Mod/Import/ImportGlobal.h>
#include <Mod/Part/App/TopoShape.h>

class StepData_StepModel;

namespace App
{
class DocumentObject;
}
namespace Base
{
class FileInfo;
}
namespace Import
{
using ShapeExportObserver = std::function<void(
    const TDF_Label&, App::DocumentObject*, const Part::TopoShape&)>;

// Per-export state; no source XCAF document or global report is retained.
class ImportExport StepPmiExport
{
public:
    explicit StepPmiExport(int override);
    ShapeExportObserver observer();
    void complete(const Handle(StepData_StepModel)& model) const;
    Py::Object write(const Base::FileInfo& file, const Handle(TDocStd_Document)& document,
                     const std::vector<App::DocumentObject*>& objects);
private:
    struct Association
    {
        TDF_Label label;
        App::DocumentObject* owner;
        Part::TopoShape shape;
    };
    bool enabled;
    bool failed = false;
    std::vector<Association> associations;
    std::map<std::string, std::string> semanticNames;
    std::map<std::string, std::pair<double, int>> projectedZones;
    void populate(const Handle(TDocStd_Document)& document, const Py::List& candidates,
                  Py::List& report);
};
}  // namespace Import
