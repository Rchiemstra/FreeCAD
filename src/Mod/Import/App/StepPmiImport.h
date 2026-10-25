// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <string>
#include <utility>
#include <vector>

#include <TDF_Label.hxx>
#include <TDocStd_Document.hxx>
#include <TopoDS_Shape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <Mod/Import/ImportGlobal.h>

class StepData_StepModel;

namespace Base
{
class FileInfo;
}

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
    std::string deviationStatus;
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
    // override: -1 uses the default-off preference, 0/1 explicitly disables/enables.
    static bool enabled(const Base::FileInfo& file, int override = -1);
    static std::unique_ptr<StepPmiImport> create(const Base::FileInfo& file, int override) noexcept;
    template<class Converter>
    void attach(Converter& converter) noexcept
    {
        try {
            converter.setShapeObserver(observer());
        }
        catch (...) {
            failed = true;
        }
    }
    App::DocumentObject* finish(const Base::FileInfo& file, const Handle(TDocStd_Document)& source,
                App::Document* target, bool supportedAssociation) const noexcept;
    void captureReadWarnings(const Handle(StepData_StepModel)& model) noexcept;
    ShapeImportObserver observer();
    std::vector<StepPmiRecord> extract(
        const Handle(TDocStd_Document) & document,
        App::Document* target,
        bool supportedAssociation
    ) const;
    App::DocumentObject* materialize(App::Document* target, const std::vector<StepPmiRecord>& records) const;

private:
    struct Association
    {
        std::string label;
        TopoDS_Shape shape;
        App::Document* document;
        std::string objectName;
        bool occurrence;
        mutable std::map<int, TopTools_IndexedMapOfShape> originalMaps;
        mutable std::map<int, TopTools_IndexedMapOfShape> actualMaps;
    };
    std::vector<Association> associations;
    using AssociationKey = std::tuple<std::string, App::Document*, std::string>;
    std::map<AssociationKey, TopTools_IndexedMapOfShape> associationIndex;
    struct SignedBounds
    {
        double lower;
        double upper;
        bool conflicting = false;
    };
    using BoundsKey = std::tuple<std::string, double, double>;
    std::map<BoundsKey, SignedBounds> signedBounds;
    std::set<std::pair<std::string, double>> lostUpperBounds;
    void captureSignedBounds(const Handle(StepData_StepModel)& model);
    bool failed = false;
    static void warn(const Base::FileInfo& file) noexcept;
    std::vector<std::string> sourceWarnings;
    bool sourceWarningsFailed = false;
    static std::vector<std::string> readWarnings(const Handle(StepData_StepModel)& model);
    StepPmiReference resolve(
        const TDF_Label& label,
        const std::string& role,
        App::Document* target,
        bool supported
    ) const;
};
}  // namespace Import
