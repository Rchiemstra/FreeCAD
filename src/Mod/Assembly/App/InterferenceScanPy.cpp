// SPDX-License-Identifier: LGPL-2.1-or-later

#include "InterferenceScanPy.h"
#include "InterferenceScan.h"

#include <App/DocumentObjectPy.h>
#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <Base/PyWrapParseTupleAndKeywords.h>
#include <Mod/Part/App/TopoShape.h>
#include <Mod/Part/App/TopoShapePy.h>
#include <Standard_Failure.hxx>

#include <array>
#include <memory>

namespace Assembly
{
namespace
{
constexpr auto snapshotName = "Assembly.InterferenceScanResult";
constexpr auto cancellationName = "Assembly.InterferenceScanCancellation";

const char* kindName(Part::InterferenceKind kind)
{
    switch (kind) {
        case Part::InterferenceKind::Clear: return "Clear";
        case Part::InterferenceKind::ClearanceViolation: return "ClearanceViolation";
        case Part::InterferenceKind::Contact: return "Contact";
        case Part::InterferenceKind::Penetration: return "Penetration";
        case Part::InterferenceKind::InvalidInput: return "InvalidInput";
        case Part::InterferenceKind::Inconclusive: return "Inconclusive";
        case Part::InterferenceKind::Cancelled: return "Cancelled";
    }
    return "Inconclusive";
}

const char* ruleName(InterferenceClearanceRuleKind kind)
{
    switch (kind) {
        case InterferenceClearanceRuleKind::ExactPair: return "ExactPair";
        case InterferenceClearanceRuleKind::MaxIndividual: return "MaxIndividual";
        case InterferenceClearanceRuleKind::DefaultStar: return "DefaultStar";
        case InterferenceClearanceRuleKind::AssemblyGlobal: return "AssemblyGlobal";
        case InterferenceClearanceRuleKind::Unresolved: return "Unresolved";
    }
    return "Unresolved";
}

Py::Object point(const Base::Vector3d& value)
{
    return Py::asObject(Py_BuildValue("(ddd)", value.x, value.y, value.z));
}

Py::Object shape(const TopoDS_Shape& value)
{
    return Py::asObject(new Part::TopoShapePy(new Part::TopoShape(value)));
}

Py::Dict detection(const Part::InterferenceResult& value)
{
    Py::Dict out;
    out["kind"] = Py::String(kindName(value.kind));
    out["minimumDistance"] = Py::Float(value.minimumDistance);
    out["pointOnFirst"] = point(value.pointOnFirst);
    out["pointOnSecond"] = point(value.pointOnSecond);
    out["commonShape"] = shape(value.commonShape);
    out["overlapVolume"] = Py::Float(value.overlapVolume);
    out["diagnostic"] = Py::String(value.diagnostic);
    return out;
}

InterferenceSelectionHandle handle(PyObject* value)
{
    PyObject* object = value;
    const char* subName = "";
    if (PyTuple_Check(value)) {
        if (!PyArg_ParseTuple(value, "O!s", &App::DocumentObjectPy::Type, &object, &subName)) {
            throw Py::Exception();
        }
    }
    else if (!PyObject_TypeCheck(object, &App::DocumentObjectPy::Type)) {
        throw Py::TypeError("Pair endpoints must be DocumentObjects or (DocumentObject, subname)");
    }
    auto* docObject = static_cast<App::DocumentObjectPy*>(object)->getDocumentObjectPtr();
    if (!docObject || !docObject->isAttachedToDocument()) {
        throw Py::ValueError("Pair endpoint is no longer attached to a document");
    }
    return {docObject, subName};
}

std::atomic<bool>* cancellation(PyObject* value)
{
    auto* flag = static_cast<std::atomic<bool>*>(PyCapsule_GetPointer(value, cancellationName));
    if (!flag) {
        throw Py::Exception();
    }
    return flag;
}

Py::Dict resultToPy(std::unique_ptr<InterferenceScanResult> result)
{
    Py::Dict out;
    out["complete"] = Py::Boolean(result->complete);
    out["cancelled"] = Py::Boolean(result->cancelled);
    Py::Dict counts;
#define SCAN_COUNT(name) counts[#name] = Py::Long(result->counts.name)
    SCAN_COUNT(penetrations);
    SCAN_COUNT(contacts);
    SCAN_COUNT(clearanceViolations);
    SCAN_COUNT(excludedViolations);
    SCAN_COUNT(invalidRules);
    SCAN_COUNT(invalidInputs);
    SCAN_COUNT(inconclusivePairs);
    SCAN_COUNT(clearPairs);
    SCAN_COUNT(clearFaceHits);
#undef SCAN_COUNT
    out["counts"] = counts;

    Py::List leaves;
    for (const auto& leaf : result->leaves) {
        Py::Dict item;
        item["occurrenceSubName"] = Py::String(leaf.occurrenceSubName);
        item["displayPath"] = Py::String(leaf.displayPath);
        item["sourceId"] = Py::String(leaf.sourceId);
        item["worldShape"] = shape(leaf.worldShape);
        item["visible"] = Py::Boolean(leaf.visible);
        item["shapeValid"] = Py::Boolean(leaf.shapeValid);
        item["diagnostic"] = Py::String(leaf.diagnostic);
        leaves.append(item);
    }
    out["leaves"] = leaves;

    Py::List pairs;
    for (const auto& pair : result->pairs) {
        Py::Dict item;
        item["leafIndexA"] = Py::Long(static_cast<long>(pair.leafIndexA));
        item["leafIndexB"] = Py::Long(static_cast<long>(pair.leafIndexB));
        item["detection"] = detection(pair.detection);
        item["solidDetection"] = detection(pair.solidDetection);
        item["distanceMeasured"] = Py::Boolean(pair.distanceMeasured);
        item["excluded"] = Py::Boolean(pair.excluded);
        item["governingFaceHitIndex"] = pair.governingFaceHitIndex == static_cast<std::size_t>(-1)
            ? Py::Long(-1) : Py::Long(static_cast<long>(pair.governingFaceHitIndex));
        item["faceEnumerationDiagnostic"] = Py::String(pair.faceEnumerationDiagnostic);
        Py::List hits;
        for (const auto& hit : pair.faceHits) {
            Py::Dict face;
            face["facePathA"] = Py::String(hit.facePathA);
            face["facePathB"] = Py::String(hit.facePathB);
            face["distance"] = Py::Float(hit.distance);
            face["appliedClearance"] = Py::Float(hit.appliedClearance);
            face["kind"] = Py::String(kindName(hit.classification));
            face["ruleKind"] = Py::String(ruleName(hit.ruleKind));
            Py::List rows;
            for (int row : hit.sourceRows) {
                rows.append(Py::Long(row));
            }
            Py::List comments;
            for (const auto& comment : hit.sourceComments) {
                comments.append(Py::String(comment));
            }
            face["sourceRows"] = rows;
            face["sourceComments"] = comments;
            face["diagnostic"] = Py::String(hit.diagnostic);
            face["closestPointsValid"] = Py::Boolean(hit.closestPointsValid);
            face["pointOnFirst"] = point(hit.pointOnFirst);
            face["pointOnSecond"] = point(hit.pointOnSecond);
            face["commonShape"] = shape(hit.commonShape);
            face["suppressedByExclusion"] = Py::Boolean(hit.suppressedByExclusion);
            hits.append(face);
        }
        item["faceHits"] = hits;
        pairs.append(item);
    }
    out["pairs"] = pairs;
    Py::List issues;
    for (const auto& issue : result->componentIssues) {
        Py::Dict item;
        item["kind"] = Py::Long(static_cast<int>(issue.kind));
        item["leafIndex"] = Py::Long(static_cast<long>(issue.leafIndex));
        item["leafIndexB"] = issue.leafIndexB == static_cast<std::size_t>(-1)
            ? Py::Long(-1) : Py::Long(static_cast<long>(issue.leafIndexB));
        item["diagnostic"] = Py::String(issue.diagnostic);
        issues.append(item);
    }
    out["componentIssues"] = issues;
    auto capsule = Py::asObject(PyCapsule_New(result.get(), snapshotName, [](PyObject* obj) {
        delete static_cast<InterferenceScanResult*>(PyCapsule_GetPointer(obj, snapshotName));
    }));
    result.release();
    out["_snapshot"] = capsule;
    return out;
}
}  // namespace

const InterferenceScanResult& interferenceScanSnapshotFromPy(PyObject* capsule)
{
    auto* result = static_cast<InterferenceScanResult*>(PyCapsule_GetPointer(capsule, snapshotName));
    if (!result) {
        throw Py::Exception();
    }
    return *result;
}

Py::Object newInterferenceScanCancellationPy(const Py::Tuple& args)
{
    if (!PyArg_ParseTuple(args.ptr(), "")) {
        throw Py::Exception();
    }
    auto flag = std::make_unique<std::atomic<bool>>(false);
    auto out = Py::asObject(PyCapsule_New(flag.get(), cancellationName, [](PyObject* obj) {
        delete static_cast<std::atomic<bool>*>(PyCapsule_GetPointer(obj, cancellationName));
    }));
    flag.release();
    return out;
}

Py::Object cancelInterferenceScanPy(const Py::Tuple& args)
{
    PyObject* token = nullptr;
    if (!PyArg_ParseTuple(args.ptr(), "O", &token)) {
        throw Py::Exception();
    }
    cancellation(token)->store(true, std::memory_order_relaxed);
    return Py::None();
}

Py::Object scanSelectedPairPy(const Py::Tuple& args, const Py::Dict& keywords)
{
    PyObject* hostPy = nullptr;
    PyObject* firstPy = nullptr;
    PyObject* secondPy = nullptr;
    PyObject* token = Py_None;
    int measure = 0;
    Py_ssize_t cap = 10000;
    static const std::array<const char*, 7> names {
        "host", "first", "second", "measureDistances", "maxFacePairCandidates", "cancel", nullptr
    };
    if (!Base::Wrapped_ParseTupleAndKeywords(
            args.ptr(), keywords.ptr(), "O!OO|pnO", names,
            &App::DocumentObjectPy::Type, &hostPy, &firstPy, &secondPy, &measure, &cap, &token)) {
        throw Py::Exception();
    }
    if (cap < 0) {
        throw Py::ValueError("maxFacePairCandidates must be nonnegative (zero means unlimited)");
    }
    try {
        auto* host = static_cast<App::DocumentObjectPy*>(hostPy)->getDocumentObjectPtr();
        if (!host || !host->isAttachedToDocument() || !isInterferenceRoot(host)) {
            throw Py::ValueError("host must be an attached interference root");
        }
        const auto scope = resolveInterferenceSelectionScope(host, {handle(firstPy), handle(secondPy)});
        if (scope.mode != InterferenceScanScopeMode::SelectedPair) {
            throw Py::ValueError("Select exactly two distinct component occurrences under host");
        }
        InterferenceScanOptions options;
        options.clearance = getInterferenceClearance(host);
        options.measureSelectedPairDistances = measure != 0;
        options.maxFacePairCandidates = static_cast<std::size_t>(cap);
        options.cancelFlag = token == Py_None ? nullptr : cancellation(token);
        auto a = collectInterferenceLeavesUnderPrefix(host, scope.first.occurrencePrefix, true);
        auto b = collectInterferenceLeavesUnderPrefix(host, scope.second.occurrencePrefix, true);
        auto prepared = a;
        prepared.insert(prepared.end(), b.begin(), b.end());
        options.clearanceRules = snapshotInterferenceClearanceRules(host, &prepared);
        std::vector<std::pair<std::string, std::string>> exclusions;
        for (const auto& rule : getInterferenceExclusionRules(host)) {
            if (rule.valid) {
                exclusions.emplace_back(rule.firstIdentity, rule.secondIdentity);
            }
        }
        auto result = std::make_unique<InterferenceScanResult>();
        {
            // Only immutable geometry/rules cross this boundary. Document preparation
            // and Python conversion remain on the caller's document thread.
            Base::PyGILStateRelease unlock;
            *result = runInterferenceScanBetweenLeafSets(a, b, options, exclusions);
        }
        if (a.empty() || b.empty()) {
            result->complete = false;
            result->counts.invalidInputs += 1;
            InterferenceComponentIssue issue;
            issue.diagnostic = "Selected component has no physical solid leaves";
            result->componentIssues.push_back(issue);
        }
        auto out = resultToPy(std::move(result));
        out["firstOccurrence"] = Py::String(scope.first.occurrencePrefix);
        out["secondOccurrence"] = Py::String(scope.second.occurrencePrefix);
        out["clearance"] = Py::Float(options.clearance);
        return out;
    }
    catch (const Base::Exception& error) {
        throw Py::RuntimeError(error.what());
    }
    catch (const Standard_Failure& error) {
        throw Py::RuntimeError(error.GetMessageString());
    }
    catch (const std::exception& error) {
        throw Py::RuntimeError(error.what());
    }
}
}  // namespace Assembly
