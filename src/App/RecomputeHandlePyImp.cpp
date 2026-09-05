// SPDX-License-Identifier: LGPL-2.1-or-later

#include "RecomputeHandle.h"

#include <cmath>
#include <limits>
#include <sstream>

#include "RecomputeHandlePy.h"
#include "RecomputeHandlePy.cpp"

using namespace App;

namespace
{

PyObject* snapshotToPython(const DocumentRecomputeSnapshot& snapshot)
{
    Py::Dict result;
    result.setItem("id", Py::Long(static_cast<unsigned long long>(snapshot.id)));
    result.setItem("state", Py::String(documentRecomputeStateName(snapshot.state)));
    result.setItem("completed", Py::Long(snapshot.completedFeatures));
    result.setItem("failed", Py::Long(snapshot.failedFeatures));
    result.setItem("total", Py::Long(snapshot.totalFeatures));
    result.setItem("progress", Py::Float(snapshot.progress));
    result.setItem("diagnostic", Py::String(snapshot.diagnostic));
    result.setItem("terminal", Py::Boolean(snapshot.terminal()));

    Py::List features;
    for (const auto& feature : snapshot.features) {
        Py::Dict item;
        item.setItem("feature", Py::String(feature.featureId));
        item.setItem("state", Py::String(documentRecomputeFeatureStateName(feature.state)));
        item.setItem("diagnostic", Py::String(feature.diagnostic));
        features.append(item);
    }
    result.setItem("features", features);
    return Py::new_reference_to(result);
}

}  // namespace

std::string RecomputeHandlePy::representation() const
{
    std::stringstream stream;
    stream << "<RecomputeHandle id=" << getRecomputeHandlePtr()->id() << ">";
    return stream.str();
}

PyObject* RecomputeHandlePy::id(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    return PyLong_FromUnsignedLongLong(getRecomputeHandlePtr()->id());
}

PyObject* RecomputeHandlePy::status(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        return snapshotToPython(getRecomputeHandlePtr()->status());
    }
    PY_CATCH;
}

PyObject* RecomputeHandlePy::progress(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        return PyFloat_FromDouble(getRecomputeHandlePtr()->status().progress);
    }
    PY_CATCH;
}

PyObject* RecomputeHandlePy::done(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        return PyBool_FromLong(getRecomputeHandlePtr()->status().terminal());
    }
    PY_CATCH;
}

PyObject* RecomputeHandlePy::cancel(PyObject* args)
{
    const char* reason = "recompute cancelled by caller";
    if (!PyArg_ParseTuple(args, "|s", &reason)) {
        return nullptr;
    }
    PY_TRY
    {
        return PyBool_FromLong(getRecomputeHandlePtr()->cancel(reason));
    }
    PY_CATCH;
}

PyObject* RecomputeHandlePy::wait(PyObject* args)
{
    double timeoutSeconds = 360.0;
    if (!PyArg_ParseTuple(args, "|d", &timeoutSeconds)) {
        return nullptr;
    }
    if (!std::isfinite(timeoutSeconds) || timeoutSeconds < 0.0
        || timeoutSeconds > 86'400.0) {
        PyErr_SetString(PyExc_ValueError, "timeout must be finite and between 0 and 86400 seconds");
        return nullptr;
    }
    const auto milliseconds = static_cast<long long>(timeoutSeconds * 1000.0);
    PY_TRY
    {
        return snapshotToPython(
            getRecomputeHandlePtr()->wait(std::chrono::milliseconds(milliseconds)));
    }
    PY_CATCH;
}

PyObject* RecomputeHandlePy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int RecomputeHandlePy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
