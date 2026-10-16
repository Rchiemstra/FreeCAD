// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentCommandHandle.h"
#include "DocumentPythonCommand.h"

#include "DocumentCommandHandlePy.h"
#include "DocumentCommandHandlePy.cpp"

#include <Base/Interpreter.h>

#include <sstream>

using namespace App;

namespace
{

const char* documentCommandRecomputeStateName(
    const DocumentCommandRecomputeState state) noexcept
{
    switch (state) {
        case DocumentCommandRecomputeState::Running:
            return "running";
        case DocumentCommandRecomputeState::Cancelling:
            return "cancelling";
        case DocumentCommandRecomputeState::Completed:
            return "completed";
        case DocumentCommandRecomputeState::PartialFailure:
            return "partial_failure";
        case DocumentCommandRecomputeState::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

const char* documentCommandRecomputeFeatureStateName(
    const DocumentCommandRecomputeFeatureState state) noexcept
{
    switch (state) {
        case DocumentCommandRecomputeFeatureState::Waiting:
            return "waiting";
        case DocumentCommandRecomputeFeatureState::Preparing:
            return "preparing";
        case DocumentCommandRecomputeFeatureState::Committing:
            return "committing";
        case DocumentCommandRecomputeFeatureState::Committed:
            return "committed";
        case DocumentCommandRecomputeFeatureState::Stale:
            return "stale";
        case DocumentCommandRecomputeFeatureState::Failed:
            return "failed";
        case DocumentCommandRecomputeFeatureState::Blocked:
            return "blocked";
        case DocumentCommandRecomputeFeatureState::Cancelling:
            return "cancelling";
        case DocumentCommandRecomputeFeatureState::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

PyObject* commandSnapshotToPython(const DocumentCommandSnapshot& snapshot)
{
    Py::Dict result;
    result.setItem("id", Py::Long(static_cast<unsigned long long>(snapshot.id)));
    result.setItem("kind", Py::String(documentCommandKindName(snapshot.kind)));
    result.setItem("state", Py::String(documentCommandStateName(snapshot.state)));
    result.setItem("progress", Py::Float(snapshot.progress));
    result.setItem("diagnostic", Py::String(snapshot.diagnostic));
    result.setItem("terminal", Py::Boolean(snapshot.terminal()));

    if (snapshot.recompute) {
        Py::Dict recompute;
        recompute.setItem("id", Py::Long(static_cast<unsigned long long>(snapshot.recompute->id)));
        recompute.setItem(
            "state",
            Py::String(documentCommandRecomputeStateName(snapshot.recompute->state)));
        recompute.setItem("completed", Py::Long(snapshot.recompute->completedFeatures));
        recompute.setItem("failed", Py::Long(snapshot.recompute->failedFeatures));
        recompute.setItem("total", Py::Long(snapshot.recompute->totalFeatures));
        recompute.setItem("progress", Py::Float(snapshot.recompute->progress));
        recompute.setItem("diagnostic", Py::String(snapshot.recompute->diagnostic));
        recompute.setItem("terminal", Py::Boolean(snapshot.recompute->terminal()));

        Py::List features;
        for (const auto& feature : snapshot.recompute->features) {
            Py::Dict item;
            item.setItem("feature", Py::String(feature.featureId));
            item.setItem(
                "state",
                Py::String(documentCommandRecomputeFeatureStateName(feature.state)));
            item.setItem("diagnostic", Py::String(feature.diagnostic));
            item.setItem("executed", Py::Boolean(feature.executed));
            features.append(item);
        }
        recompute.setItem("features", features);
        result.setItem("recompute", recompute);
    }

    return Py::new_reference_to(result);
}

void ensureDocumentCommandHandleTypeRegistered()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    PyObject* module = PyImport_AddModule("FreeCAD.App");
    Base::Interpreter().addType(&DocumentCommandHandlePy::Type, module, "DocumentCommandHandle");
    registered = true;
}

}  // namespace

std::string DocumentCommandHandlePy::representation() const
{
    std::stringstream stream;
    stream << "<DocumentCommandHandle id=" << getDocumentCommandHandlePtr()->id() << ">";
    return stream.str();
}

PyObject* DocumentCommandHandlePy::id(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    return PyLong_FromUnsignedLongLong(getDocumentCommandHandlePtr()->id());
}

PyObject* DocumentCommandHandlePy::status(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        return commandSnapshotToPython(getDocumentCommandHandlePtr()->status());
    }
    PY_CATCH;
}

PyObject* DocumentCommandHandlePy::progress(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        return PyFloat_FromDouble(getDocumentCommandHandlePtr()->status().progress);
    }
    PY_CATCH;
}

PyObject* DocumentCommandHandlePy::done(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        return PyBool_FromLong(getDocumentCommandHandlePtr()->status().terminal());
    }
    PY_CATCH;
}

PyObject* DocumentCommandHandlePy::cancel(PyObject* args)
{
    const char* reason = "command cancelled by caller";
    if (!PyArg_ParseTuple(args, "|s", &reason)) {
        return nullptr;
    }
    PY_TRY
    {
        return PyBool_FromLong(getDocumentCommandHandlePtr()->cancel(reason));
    }
    PY_CATCH;
}

PyObject* DocumentCommandHandlePy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int DocumentCommandHandlePy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}

PyObject* App::makeDocumentCommandHandlePy(const DocumentCommandHandle& handle)
{
    ensureDocumentCommandHandleTypeRegistered();
    return new DocumentCommandHandlePy(new DocumentCommandHandle(handle));
}
