// SPDX-License-Identifier: LGPL-2.1-or-later

#include "InterferenceScanPy.h"
#include "InterferenceScan.h"
#include "AssemblyObject.h"

#include <App/DocumentObjectPy.h>
#include <App/ScopedDocumentMutation.h>
#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <Base/PyWrapParseTupleAndKeywords.h>
#include <Mod/Part/App/TopoShape.h>
#include <Mod/Part/App/TopoShapePy.h>
#include <Standard_Failure.hxx>

#include <array>
#include <cmath>
#include <memory>

namespace Assembly
{
namespace
{
constexpr auto snapshotName = "Assembly.InterferenceScanResult";
constexpr auto preparedName = "Assembly.PreparedInterferenceScan";

// No document pointers: only the document thread prepares this snapshot.
struct PreparedScan
{
    std::vector<InterferenceLeaf> first;
    std::vector<InterferenceLeaf> second;
    InterferenceScanOptions options;
    std::vector<std::pair<std::string, std::string>> exclusions;
    std::string firstOccurrence;
    std::string secondOccurrence;
    Py::Object token;  // Own the cancellation flag through worker completion.
    std::atomic<bool> started {false};
};

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

Py::Object scanSelectedPairPy(const Py::Tuple& args, const Py::Dict& keywords, bool prepareOnly)
{
    PyObject* hostPy = nullptr;
    PyObject* firstPy = nullptr;
    PyObject* secondPy = nullptr;
    PyObject* token = Py_None;
    PyObject* clearance = Py_None;
    PyObject* geometry = Py_None;
    int measure = 0;
    Py_ssize_t cap = 10000;
    static const std::array<const char*, 9> names {
        "host", "first", "second", "measureDistances", "maxFacePairCandidates", "cancel", "clearance", "geometry", nullptr
    };
    if (!Base::Wrapped_ParseTupleAndKeywords(
            args.ptr(), keywords.ptr(), "O!OO|pnOOO", names,
            &App::DocumentObjectPy::Type, &hostPy, &firstPy, &secondPy, &measure, &cap, &token, &clearance, &geometry)) {
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
        options.clearance = clearance == Py_None ? getInterferenceClearance(host)
                                                : PyFloat_AsDouble(clearance);
        if (PyErr_Occurred()) {
            throw Py::Exception();
        }
        if (clearance != Py_None
            && (!std::isfinite(options.clearance) || options.clearance < 0)) {
            throw Py::ValueError("clearance must be finite and nonnegative");
        }
        options.measureSelectedPairDistances = measure != 0;
        options.maxFacePairCandidates = static_cast<std::size_t>(cap);
        options.cancelFlag = token == Py_None ? nullptr : cancellation(token);
        auto* cached = geometry == Py_None ? nullptr
            : static_cast<PreparedScan*>(PyCapsule_GetPointer(geometry, preparedName));
        if (geometry != Py_None && !cached) {
            throw Py::Exception();
        }
        if (cached && (cached->firstOccurrence != scope.first.occurrencePrefix
                       || cached->secondOccurrence != scope.second.occurrencePrefix)) {
            throw Py::ValueError("Geometry must belong to the same occurrence pair");
        }
        auto a = cached ? reposeInterferenceLeaves(host, cached->first)
                       : collectInterferenceLeavesUnderPrefix(host, scope.first.occurrencePrefix, true);
        auto b = cached ? reposeInterferenceLeaves(host, cached->second)
                       : collectInterferenceLeavesUnderPrefix(host, scope.second.occurrencePrefix, true);
        auto prepared = a;
        prepared.insert(prepared.end(), b.begin(), b.end());
        options.clearanceRules = snapshotInterferenceClearanceRules(host, &prepared);
        std::vector<std::pair<std::string, std::string>> exclusions;
        for (const auto& rule : getInterferenceExclusionRules(host)) {
            if (rule.valid) {
                exclusions.emplace_back(rule.firstIdentity, rule.secondIdentity);
            }
        }
        auto preparedScan = std::make_unique<PreparedScan>();
        preparedScan->first = std::move(a);
        preparedScan->second = std::move(b);
        preparedScan->options = std::move(options);
        preparedScan->exclusions = std::move(exclusions);
        preparedScan->firstOccurrence = scope.first.occurrencePrefix;
        preparedScan->secondOccurrence = scope.second.occurrencePrefix;
        preparedScan->token = Py::Object(token);
        auto capsule = Py::asObject(PyCapsule_New(preparedScan.get(), preparedName, [](PyObject* obj) {
            delete static_cast<PreparedScan*>(PyCapsule_GetPointer(obj, preparedName));
        }));
        preparedScan.release();
        if (prepareOnly) {
            return capsule;
        }
        Py::Tuple runArgs(1);
        runArgs[0] = capsule;
        return runPreparedInterferenceScanPy(runArgs);
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

Py::Object runPreparedInterferenceScanPy(const Py::Tuple& args)
{
    PyObject* capsule = nullptr;
    if (!PyArg_ParseTuple(args.ptr(), "O", &capsule)) {
        throw Py::Exception();
    }
    auto* scan = static_cast<PreparedScan*>(PyCapsule_GetPointer(capsule, preparedName));
    if (!scan) {
        throw Py::Exception();
    }
    if (scan->started.exchange(true)) {
        throw Py::ValueError("A prepared scan can only be run once");
    }
    try {
        auto result = std::make_unique<InterferenceScanResult>();
        {
            Base::PyGILStateRelease unlock;
            *result = runInterferenceScanBetweenLeafSets(
                scan->first, scan->second, scan->options, scan->exclusions);
        }
        if (scan->first.empty() || scan->second.empty()) {
            result->complete = false;
            result->counts.invalidInputs += 1;
            InterferenceComponentIssue issue;
            issue.diagnostic = "Selected component has no physical solid leaves";
            result->componentIssues.push_back(issue);
        }
        auto out = resultToPy(std::move(result));
        out["firstOccurrence"] = Py::String(scan->firstOccurrence);
        out["secondOccurrence"] = Py::String(scan->secondOccurrence);
        out["clearance"] = Py::Float(scan->options.clearance);
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

Py::Object motionSimulationFrameTimesPy(const Py::Tuple& args)
{
    PyObject* hostPy = nullptr;
    if (!PyArg_ParseTuple(args.ptr(), "O!", &App::DocumentObjectPy::Type, &hostPy)) {
        throw Py::Exception();
    }
    auto* host = dynamic_cast<AssemblyObject*>(
        static_cast<App::DocumentObjectPy*>(hostPy)->getDocumentObjectPtr());
    if (!host || !host->isAttachedToDocument()) {
        throw Py::ValueError("An attached assembly is required");
    }
    Py::List times;
    for (double time : host->motionSimulationFrameTimes()) {
        times.append(Py::Float(time));
    }
    return times;
}

Py::Object motionSimulationJointStatePy(const Py::Tuple& args)
{
    PyObject* hostPy = nullptr;
    PyObject* jointPy = nullptr;
    unsigned long frame;
    const char* formula;
    double time;
    if (!PyArg_ParseTuple(args.ptr(), "O!O!ksd", &App::DocumentObjectPy::Type, &hostPy,
                          &App::DocumentObjectPy::Type, &jointPy, &frame, &formula, &time)) {
        throw Py::Exception();
    }
    auto* host = dynamic_cast<AssemblyObject*>(
        static_cast<App::DocumentObjectPy*>(hostPy)->getDocumentObjectPtr());
    auto* joint = static_cast<App::DocumentObjectPy*>(jointPy)->getDocumentObjectPtr();
    if (!host || !host->isAttachedToDocument() || !joint || !joint->isAttachedToDocument()) {
        throw Py::ValueError("An attached assembly and joint are required");
    }
    try {
        Py::List values;
        for (double value : host->motionSimulationJointState(joint, frame, formula, time)) {
            values.append(Py::Float(value));
        }
        return values;
    }
    catch (const std::exception& error) {
        throw Py::RuntimeError(error.what());
    }
}

Py::Object withMotionSweepStatePy(const Py::Tuple& args, bool mutation)
{
    PyObject* hostPy = nullptr;
    PyObject* callback = nullptr;
    if (!PyArg_ParseTuple(args.ptr(), "O!O", &App::DocumentObjectPy::Type, &hostPy, &callback)) {
        throw Py::Exception();
    }
    auto* host = dynamic_cast<AssemblyObject*>(
        static_cast<App::DocumentObjectPy*>(hostPy)->getDocumentObjectPtr());
    if (!host || !host->isAttachedToDocument() || !PyCallable_Check(callback)) {
        throw Py::ValueError("An attached assembly and synchronous callable are required");
    }
    try {
        std::unique_ptr<App::ScopedDocumentMutation::Allow> controlled;
        if (mutation) {
            controlled = std::make_unique<App::ScopedDocumentMutation::Allow>(*host->getDocument());
        }
        auto* result = mutation ? PyObject_CallNoArgs(callback) : host->withMotionSweepState(callback);
        if (!result) {
            throw Py::Exception();
        }
        return Py::asObject(result);
    }
    catch (const Base::Exception& error) {
        throw Py::RuntimeError(error.what());
    }
}
}  // namespace Assembly
