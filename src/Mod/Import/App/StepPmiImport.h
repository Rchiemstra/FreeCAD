// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <TDF_Label.hxx>
#include <TDocStd_Document.hxx>
#include <TopoDS_Shape.hxx>
#include <Mod/Import/ImportGlobal.h>

namespace App
{
class Document;
class DocumentObject;
}  // namespace App

namespace Import
{
// Called only for shapes actually created by a converter. No converter state escapes.
using ShapeImportObserver
    = std::function<void(const TDF_Label&, const TopoDS_Shape&, App::DocumentObject*)>;

struct StepPmiReference
{
    std::string sourceId;
    std::string role;
    std::string objectName;
    std::string subElement;
    std::string status;
};

// Owned, plain data: no XCAF handles or labels survive extraction.
struct StepPmiRecord
{
    std::string category;
    std::string sourceId;
    std::string name;
    std::string kind;
    std::string unit;
    int type = 0;
    double value = 0;
    bool hasValue = false;
    bool hasBounds = false;
    bool hasDeviations = false;
    double lower = 0;
    double upper = 0;
    int datumPosition = 0;
    std::string datumName;
    std::vector<double> values;
    std::vector<std::string> modifiers;
    std::vector<std::string> datumIds;
    std::vector<StepPmiReference> references;
    std::vector<std::string> warnings;
    std::map<std::string, double> numbers;
    std::map<std::string, std::string> strings;
};

class ImportExport StepPmiImport
{
public:
    ShapeImportObserver observer();
    std::vector<StepPmiRecord> extract(
        const Handle(TDocStd_Document) & document,
        App::Document* target,
        bool supportedAssociation
    ) const;
    static void materialize(App::Document* target, const std::vector<StepPmiRecord>& records);

private:
    struct Association
    {
        std::string label;
        TopoDS_Shape shape;
        App::Document* document;
        std::string objectName;
        bool occurrence;
    };
    std::vector<Association> associations;
    StepPmiReference resolve(
        const TDF_Label& label,
        const std::string& role,
        App::Document* target,
        bool supported
    ) const;
};
}  // namespace Import
