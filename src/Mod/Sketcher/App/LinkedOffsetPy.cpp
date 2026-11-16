// SPDX-License-Identifier: LGPL-2.1-or-later

#include <Base/Exception.h>
#include <stdexcept>
#include <Standard_Failure.hxx>
#include "LinkedOffset.h"
#include "SketchObjectPy.h"

namespace
{
PyObject* listRelations(PyObject*, PyObject* argument)
{
    if (!PyObject_TypeCheck(argument, &Sketcher::SketchObjectPy::Type)) {
        PyErr_SetString(PyExc_TypeError, "Expected a Sketcher sketch.");
        return nullptr;
    }
    auto* wrapper = static_cast<Sketcher::SketchObjectPy*>(argument);
    if (!wrapper->Base::PyObjectBase::isValid()) {
        PyErr_SetString(PyExc_RuntimeError, "The sketch has been deleted.");
        return nullptr;
    }
    try {
        auto* sketch = wrapper->getSketchObjectPtr();
        const auto snapshot = Sketcher::linkedOffsetsJson(*sketch);
        PyObject* json = PyImport_ImportModule("json");
        if (!json) {
            return nullptr;
        }
        PyObject* result = PyObject_CallMethod(json, "loads", "s", snapshot.c_str());
        Py_DECREF(json);
        return result;
    }
    catch (const std::exception& error) {
        PyErr_SetString(PyExc_RuntimeError, error.what());
        return nullptr;
    }
}
PyObject* operate(PyObject* args, Sketcher::LinkedOffsetOperation operation)
{
    PyObject* argument;
    PyObject* selection = Py_None;
    double distance = 1, tolerance = 0.01;
    if (operation == Sketcher::LinkedOffsetOperation::Edit) {
        if (!PyArg_ParseTuple(args, "OOdd", &argument, &selection, &distance, &tolerance)) {
            return nullptr;
        }
    }
    else if (!PyArg_ParseTuple(args, "O|O", &argument, &selection)) {
        return nullptr;
    }
    if (!PyObject_TypeCheck(argument, &Sketcher::SketchObjectPy::Type)) {
        PyErr_SetString(PyExc_TypeError, "Expected a Sketcher sketch.");
        return nullptr;
    }
    auto* wrapper = static_cast<Sketcher::SketchObjectPy*>(argument);
    if (!wrapper->Base::PyObjectBase::isValid()) {
        PyErr_SetString(PyExc_RuntimeError, "The sketch has been deleted.");
        return nullptr;
    }
    std::vector<std::string> ids;
    if (selection != Py_None) {
        if (!PyList_Check(selection) && !PyTuple_Check(selection)) {
            PyErr_SetString(PyExc_TypeError, "Expected a list or tuple of relation UUIDs.");
            return nullptr;
        }
        const auto size = PySequence_Size(selection);
        for (Py_ssize_t i = 0; i < size; ++i) {
            PyObject* item = PySequence_GetItem(selection, i);
            if (!item) {
                return nullptr;
            }
            Py_ssize_t length = 0;
            const char* id = PyUnicode_Check(item)
                ? PyUnicode_AsUTF8AndSize(item, &length) : nullptr;
            if (!id) {
                Py_DECREF(item);
                PyErr_SetString(PyExc_TypeError, "Expected relation UUID strings.");
                return nullptr;
            }
            ids.emplace_back(id, size_t(length));
            Py_DECREF(item);
        }
        if (ids.empty()) {
            PyErr_SetString(PyExc_ValueError, "Empty relation selection; use None for all.");
            return nullptr;
        }
    }
    try {
        Sketcher::operateLinkedOffsets(*wrapper->getSketchObjectPtr(), ids, operation,
                                       distance, tolerance);
        Py_RETURN_NONE;
    }
    catch (const std::invalid_argument& error) {
        PyErr_SetString(PyExc_ValueError, error.what());
    }
    catch (const Standard_Failure& error) {
        PyErr_SetString(PyExc_RuntimeError, error.GetMessageString());
    }
    catch (const std::exception& error) {
        PyErr_SetString(PyExc_RuntimeError, error.what());
    }
    return nullptr;
}

PyObject* refresh(PyObject*, PyObject* args)
{
    return operate(args, Sketcher::LinkedOffsetOperation::Refresh);
}
PyObject* edit(PyObject*, PyObject* args)
{
    return operate(args, Sketcher::LinkedOffsetOperation::Edit);
}
PyObject* detach(PyObject*, PyObject* args)
{
    return operate(args, Sketcher::LinkedOffsetOperation::Detach);
}

PyMethodDef methods[] = {
    {"listLinkedOffsets", listRelations, METH_O,
     "listLinkedOffsets(sketch) -> detached relation/status/parameter snapshots."},
    {"refreshLinkedOffsets", refresh, METH_VARARGS,
     "refreshLinkedOffsets(sketch, ids=None): explicit refresh; None selects all.\n"
     "Refusals raise ValueError without a transaction; all selected relations are atomic."},
    {"editLinkedOffsets", edit, METH_VARARGS,
     "editLinkedOffsets(sketch, ids, distance, tolerance): edit and explicitly refresh.\n"
     "Signed lengths are in mm. None selects all relations."},
    {"detachLinkedOffsets", detach, METH_VARARGS,
     "detachLinkedOffsets(sketch, ids=None): remove ownership and proven owned Blocks.\n"
     "Keep geometry, user constraints and opaque metadata. None selects all."}
};
}  // namespace

void Sketcher::registerLinkedOffsetPython(PyObject* module)
{
    for (auto& method : methods) {
        PyModule_AddObject(module, method.ml_name, PyCFunction_New(&method, nullptr));
    }
}
