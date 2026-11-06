// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "StepPmiExport.h"

class STEPCAFControl_Writer;

namespace Import
{
// Local to one export. Native object identity -> observed XCAF label -> exact
// writer transfer binder. No source STEP identity or display name participates.
class ImportExport StepAttributesExport
{
public:
    explicit StepAttributesExport(int override);
    ShapeExportObserver observer();
    void prepare(const std::vector<App::DocumentObject*>& objects);
    bool hasCandidates() const;
    void complete(STEPCAFControl_Writer& writer, const Handle(TDocStd_Document)& document);
    void fail(const std::string& reason);
    Py::Object report(const Py::Object& pmi) const;
    // Private Python test callback, absent in normal exports; no saved preference.
    bool testStage(const char* stage, bool fallback, int entities) const;
private:
    struct Association { TDF_Label label; App::DocumentObject* owner; };
    bool enabled;
    bool captureFailed = false;
    std::vector<Association> associations;
    Py::List candidates;
    Py::List results;
};
}  // namespace Import
