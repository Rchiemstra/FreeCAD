// SPDX-License-Identifier: LGPL-2.1-or-later
#include "DragFeedback.h"
#include <atomic>
#include <App/DocumentObjectPy.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include "ViewProviderAssembly.h"

#ifdef ASSEMBLY_ENABLE_TEST_HOOKS
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/View3DInventor.h>
#include <Inventor/events/SoKeyboardEvent.h>
#endif

namespace AssemblyGui
{
namespace
{
// Installed only while opted in. Python unregisters on document/panel closure.
PyObject* callback = nullptr;
std::atomic<bool> active {false};
}

Py::Object setDragFeedbackCallbackPy(const Py::Tuple& args)
{
    PyObject* value = nullptr;
    if (!PyArg_ParseTuple(args.ptr(), "O", &value)) {
        throw Py::Exception();
    }
    if (value != Py_None && !PyCallable_Check(value)) {
        throw Py::TypeError("Expected a callable or None");
    }
    Py_XDECREF(callback);
    callback = value == Py_None ? nullptr : Py_NewRef(value);
    active.store(callback != nullptr, std::memory_order_relaxed);
    return Py::None();
}

void notifyDragFeedback(App::DocumentObject* assembly, bool ended) noexcept
{
    if (!active.load(std::memory_order_relaxed)) {
        return;
    }
    Base::PyGILStateLocker lock;
    if (!callback) {
        return;
    }
    PyObject* callable = Py_NewRef(callback);
    Py::Object host(assembly->getPyObject(), true);
    PyObject* result = PyObject_CallFunction(callable, "Oi", host.ptr(), ended);
    Py_DECREF(callable);
    if (!result) {
        PyErr_Clear();
        Py_CLEAR(callback);
        active.store(false, std::memory_order_relaxed);
        Base::Console().warning("Contact feedback disabled: drag observer failed\n");
    }
    Py_XDECREF(result);
}

void finishDragFeedback(void* data, SoDragger*)
{
    notifyDragFeedback(static_cast<ViewProviderAssembly*>(data)->getObject(), true);
}

#ifdef ASSEMBLY_ENABLE_TEST_HOOKS
Py::Object motionDragEventPy(const Py::Tuple& args)
{
    PyObject* hostPy = nullptr;
    const char* action = nullptr;
    short x = 0, y = 0;
    if (!PyArg_ParseTuple(args.ptr(), "O!s|hh", &App::DocumentObjectPy::Type,
                          &hostPy, &action, &x, &y)) {
        throw Py::Exception();
    }
    auto* host = static_cast<App::DocumentObjectPy*>(hostPy)->getDocumentObjectPtr();
    auto* vp = dynamic_cast<ViewProviderAssembly*>(Gui::Application::Instance->getViewProvider(host));
    auto* view = vp ? dynamic_cast<Gui::View3DInventor*>(vp->getDocument()->getActiveView()) : nullptr;
    if (!view || !vp->isInEditMode()) {
        throw Py::ValueError("Assembly must be in edit in a 3D view");
    }
    const std::string event(action);
    const SbVec2s point(x, y);
    if (event == "press" || event == "release") {
        vp->mouseButtonPressed(1, event == "press", point, view->getViewer());
    }
    else if (event == "move") {
        vp->mouseMove(point, view->getViewer());
    }
    else if (event == "escape") {
        vp->keyPressed(true, SoKeyboardEvent::ESCAPE);
    }
    else {
        throw Py::ValueError("Expected press, move, release or escape");
    }
    return Py::Boolean(vp->partMoving);
}
#endif
}  // namespace AssemblyGui
