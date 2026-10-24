// SPDX-License-Identifier: LGPL-2.1-or-later

#include <Base/Exception.h>
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
PyMethodDef method = {
    "listLinkedOffsets",
    listRelations,
    METH_O,
    "listLinkedOffsets(sketch) -> detached relation/status/parameter snapshots.\n"
    "Inspection only; geometry is never regenerated. Future record versions are ignored."
};
}  // namespace

void Sketcher::registerLinkedOffsetPython(PyObject* module)
{
    PyModule_AddObject(module, method.ml_name, PyCFunction_New(&method, nullptr));
}
