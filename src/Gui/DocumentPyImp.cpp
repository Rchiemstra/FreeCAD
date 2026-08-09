/***************************************************************************
 *   Copyright (c) 2007 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include <optional>
#include <sstream>
#include <vector>

#include <App/Document.h>
#include <App/DocumentObjectPy.h>
#include <Base/Matrix.h>
#include <Base/MatrixPy.h>
#include <Base/Stream.h>

#include "Application.h"
#include "MergeDocuments.h"
#include "MDIView.h"
#include "PersonalViewContext.h"
#include "Tree.h"
#include "ViewProviderDocumentObject.h"
#include "ViewProviderExtern.h"

// generated out of Document.pyi
#include "DocumentPy.h"
#include "DocumentPy.cpp"

#include "ViewProviderPy.h"
#include "ViewProviderDocumentObjectPy.h"

using namespace Gui;

namespace
{

std::string personalContextString(PyObject* mapping, const char* key)
{
    PyObject* value = PyDict_GetItemString(mapping, key);
    if (!value) {
        return {};
    }
    if (!PyUnicode_Check(value)) {
        throw Py::TypeError(std::string("personal view context '") + key + "' must be a string");
    }
    const char* text = PyUnicode_AsUTF8(value);
    if (!text) {
        throw Py::Exception();
    }
    return text;
}

std::optional<std::string> personalContextOptionalString(PyObject* mapping, const char* key)
{
    PyObject* value = PyDict_GetItemString(mapping, key);
    if (!value || value == Py_None) {
        return std::nullopt;
    }
    if (!PyUnicode_Check(value)) {
        throw Py::TypeError(std::string("personal view context '") + key
                            + "' must be a string or None");
    }
    const char* text = PyUnicode_AsUTF8(value);
    if (!text) {
        throw Py::Exception();
    }
    return std::string(text);
}

std::vector<std::string> personalContextStringList(PyObject* mapping, const char* key)
{
    PyObject* value = PyDict_GetItemString(mapping, key);
    if (!value) {
        return {};
    }
    PyObject* sequence =
        PySequence_Fast(value, "personal view context path fields must be sequences");
    if (!sequence) {
        throw Py::Exception();
    }
    std::vector<std::string> result;
    try {
        const auto count = PySequence_Fast_GET_SIZE(sequence);
        result.reserve(static_cast<std::size_t>(count));
        auto** items = PySequence_Fast_ITEMS(sequence);
        for (Py_ssize_t index = 0; index < count; ++index) {
            if (!PyUnicode_Check(items[index])) {
                throw Py::TypeError(std::string("personal view context '") + key
                                    + "' entries must be strings");
            }
            const char* text = PyUnicode_AsUTF8(items[index]);
            if (!text) {
                throw Py::Exception();
            }
            result.emplace_back(text);
        }
    }
    catch (...) {
        Py_DECREF(sequence);
        throw;
    }
    Py_DECREF(sequence);
    return result;
}

std::int64_t personalContextInteger(PyObject* mapping, const char* key)
{
    PyObject* value = PyDict_GetItemString(mapping, key);
    if (!value) {
        return 0;
    }
    if (!PyLong_Check(value)) {
        throw Py::TypeError(std::string("personal view context '") + key + "' must be an integer");
    }
    const auto result = PyLong_AsLongLong(value);
    if (PyErr_Occurred()) {
        throw Py::Exception();
    }
    return result;
}

PersonalViewContext personalViewContextFromPython(PyObject* value)
{
    if (!PyDict_Check(value)) {
        throw Py::TypeError("personal view context must be a dict");
    }
    PersonalViewContext context;
    context.camera = personalContextString(value, "camera");
    context.projection = personalContextString(value, "projection");
    context.selectionPaths = personalContextStringList(value, "selection_paths");
    context.preselectionPath = personalContextOptionalString(value, "preselection_path");
    context.expandedTreePaths = personalContextStringList(value, "expanded_tree_paths");
    context.treeHorizontalScroll = personalContextInteger(value, "tree_horizontal_scroll");
    context.treeVerticalScroll = personalContextInteger(value, "tree_vertical_scroll");
    context.activeDocument = personalContextString(value, "active_document");
    context.activeView = personalContextString(value, "active_view");
    context.activeWorkbench = personalContextString(value, "active_workbench");
    context.editFocus = personalContextString(value, "edit_focus");

    PyObject* overlays = PyDict_GetItemString(value, "temporary_overlays");
    if (overlays) {
        PyObject* sequence = PySequence_Fast(overlays, "temporary_overlays must be a sequence");
        if (!sequence) {
            throw Py::Exception();
        }
        try {
            const auto count = PySequence_Fast_GET_SIZE(sequence);
            context.temporaryOverlays.reserve(static_cast<std::size_t>(count));
            auto** items = PySequence_Fast_ITEMS(sequence);
            for (Py_ssize_t index = 0; index < count; ++index) {
                if (!PyDict_Check(items[index])) {
                    throw Py::TypeError("temporary overlay entries must be dicts");
                }
                context.temporaryOverlays.push_back(
                    {personalContextString(items[index], "identifier"),
                     personalContextString(items[index], "kind"),
                     personalContextString(items[index], "payload")});
            }
        }
        catch (...) {
            Py_DECREF(sequence);
            throw;
        }
        Py_DECREF(sequence);
    }
    return context;
}

Py::Dict personalViewContextToPython(const PersonalViewContext& context)
{
    Py::Dict result;
    result["camera"] = Py::String(context.camera);
    result["projection"] = Py::String(context.projection);
    Py::List selection;
    for (const auto& path : context.selectionPaths) {
        selection.append(Py::String(path));
    }
    result["selection_paths"] = selection;
    if (context.preselectionPath) {
        result["preselection_path"] = Py::String(*context.preselectionPath);
    }
    else {
        result["preselection_path"] = Py::None();
    }
    Py::List expanded;
    for (const auto& path : context.expandedTreePaths) {
        expanded.append(Py::String(path));
    }
    result["expanded_tree_paths"] = expanded;
    result["tree_horizontal_scroll"] = Py::Long(context.treeHorizontalScroll);
    result["tree_vertical_scroll"] = Py::Long(context.treeVerticalScroll);
    result["active_document"] = Py::String(context.activeDocument);
    result["active_view"] = Py::String(context.activeView);
    result["active_workbench"] = Py::String(context.activeWorkbench);
    result["edit_focus"] = Py::String(context.editFocus);
    Py::List overlays;
    for (const auto& overlay : context.temporaryOverlays) {
        Py::Dict item;
        item["identifier"] = Py::String(overlay.identifier);
        item["kind"] = Py::String(overlay.kind);
        item["payload"] = Py::String(overlay.payload);
        overlays.append(item);
    }
    result["temporary_overlays"] = overlays;
    return result;
}

}  // namespace

// returns a string which represent the object e.g. when printed in python
std::string DocumentPy::representation() const
{
    std::stringstream str;
    str << "<GUI Document object for " << getDocumentPtr()->getDocument()->getName() << ">";

    return str.str();
}


PyObject* DocumentPy::show(PyObject* args)
{
    char* psFeatStr;
    if (!PyArg_ParseTuple(args, "s;Name of the Feature to show have to be given!", &psFeatStr)) {
        return nullptr;
    }

    PY_TRY
    {
        getDocumentPtr()->setShow(psFeatStr);
        Py_Return;
    }
    PY_CATCH;
}

PyObject* DocumentPy::hide(PyObject* args)
{
    char* psFeatStr;
    if (!PyArg_ParseTuple(args, "s;Name of the Feature to hide have to be given!", &psFeatStr)) {
        return nullptr;
    }

    PY_TRY
    {
        getDocumentPtr()->setHide(psFeatStr);
        Py_Return;
    }
    PY_CATCH;
}

PyObject* DocumentPy::setPos(PyObject* args)
{
    char* psFeatStr;
    Base::Matrix4D mat;
    PyObject* pcMatObj;
    if (!PyArg_ParseTuple(
            args,
            "sO!;Name of the Feature and the transformation matrix have to be given!",
            &psFeatStr,
            &(Base::MatrixPy::Type),
            &pcMatObj
        )) {
        return nullptr;
    }

    mat = static_cast<Base::MatrixPy*>(pcMatObj)->value();

    PY_TRY
    {
        getDocumentPtr()->setPos(psFeatStr, mat);
        Py_Return;
    }
    PY_CATCH;
}

PyObject* DocumentPy::setEdit(PyObject* args)
{
    char* psFeatStr;
    int mod = 0;
    char* subname = nullptr;
    ViewProvider* vp = nullptr;
    App::DocumentObject* obj = nullptr;

    // by name
    if (PyArg_ParseTuple(args, "s|is", &psFeatStr, &mod, &subname)) {
        obj = getDocumentPtr()->getDocument()->getObject(psFeatStr);
        if (!obj) {
            PyErr_Format(Base::PyExc_FC_GeneralError, "No such object found in document: '%s'", psFeatStr);
            return nullptr;
        }
    }
    else {
        PyErr_Clear();
        PyObject* pyObj;
        if (!PyArg_ParseTuple(args, "O|is", &pyObj, &mod, &subname)) {
            return nullptr;
        }

        if (PyObject_TypeCheck(pyObj, &App::DocumentObjectPy::Type)) {
            obj = static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr();
        }
        else if (PyObject_TypeCheck(pyObj, &ViewProviderPy::Type)) {
            vp = static_cast<ViewProviderPy*>(pyObj)->getViewProviderPtr();
        }
        else {
            PyErr_SetString(
                PyExc_TypeError,
                "Expect the first argument to be string, DocumentObject or ViewObject"
            );
            return nullptr;
        }
    }

    if (!vp) {
        if (!obj || !obj->isAttachedToDocument()
            || !(vp = Application::Instance->getViewProvider(obj))) {
            PyErr_SetString(PyExc_ValueError, "Invalid document object");
            return nullptr;
        }
    }

    bool ok = getDocumentPtr()->setEdit(vp, mod, subname);

    return PyBool_FromLong(ok ? 1 : 0);
}

PyObject* DocumentPy::getInEdit(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    ViewProvider* vp = getDocumentPtr()->getInEdit();
    if (vp) {
        return vp->getPyObject();
    }

    Py_Return;
}

PyObject* DocumentPy::resetEdit(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    getDocumentPtr()->resetEdit();

    Py_Return;
}

PyObject* DocumentPy::addAnnotation(PyObject* args)
{
    char *psAnnoName, *psFileName, *psModName = nullptr;
    if (!PyArg_ParseTuple(
            args,
            "ss|s;Name of the Annotation and a file name have to be given!",
            &psAnnoName,
            &psFileName,
            &psModName
        )) {
        return nullptr;
    }

    PY_TRY
    {
        auto pcExt = new ViewProviderExtern();

        pcExt->setModeByFile(psModName ? psModName : "Main", psFileName);
        pcExt->adjustDocumentName(getDocumentPtr()->getDocument()->getName());
        getDocumentPtr()->setAnnotationViewProvider(psAnnoName, pcExt);

        Py_Return;
    }
    PY_CATCH;
}

PyObject* DocumentPy::update(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    PY_TRY
    {
        getDocumentPtr()->onUpdate();
        Py_Return;
    }
    PY_CATCH;
}

PyObject* DocumentPy::getObject(PyObject* args)
{
    char* sName;
    if (!PyArg_ParseTuple(args, "s", &sName)) {
        return nullptr;
    }

    PY_TRY
    {
        ViewProvider* pcView = getDocumentPtr()->getViewProviderByName(sName);
        if (pcView) {
            return pcView->getPyObject();
        }
        else {
            Py_Return;
        }
    }
    PY_CATCH;
}

PyObject* DocumentPy::activeObject(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    PY_TRY
    {
        App::DocumentObject* pcFtr = getDocumentPtr()->getDocument()->getActiveObject();
        if (pcFtr) {
            ViewProvider* pcView = getDocumentPtr()->getViewProvider(pcFtr);
            return pcView->getPyObject();
        }
        else {
            Py_Return;
        }
    }
    PY_CATCH;
}

PyObject* DocumentPy::activeView(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    PY_TRY
    {
        Gui::MDIView* pcView = getDocumentPtr()->getActiveView();
        if (pcView) {
            // already incremented in getPyObject().
            return pcView->getPyObject();
        }
        else {
            Py_Return;
        }
    }
    PY_CATCH;
}

PyObject* DocumentPy::createView(PyObject* args)
{
    char* sType;
    if (!PyArg_ParseTuple(args, "s", &sType)) {
        return nullptr;
    }

    Base::Type type = Base::Type::fromName(sType);
    if (type.isBad()) {
        PyErr_Format(PyExc_TypeError, "'%s' is not a valid type", sType);
        return nullptr;
    }

    PY_TRY
    {
        Gui::MDIView* pcView = getDocumentPtr()->createView(type);
        if (pcView) {
            return pcView->getPyObject();
        }
        else {
            Py_Return;
        }
    }
    PY_CATCH;
}

PyObject* DocumentPy::mdiViewsOfType(PyObject* args) const
{
    char* sType;
    if (!PyArg_ParseTuple(args, "s", &sType)) {
        return nullptr;
    }

    Base::Type type = Base::Type::fromName(sType);
    if (type.isBad()) {
        PyErr_Format(PyExc_TypeError, "'%s' is not a valid type", sType);
        return nullptr;
    }

    PY_TRY
    {
        std::list<Gui::MDIView*> views = getDocumentPtr()->getMDIViewsOfType(type);
        Py::List list;
        for (auto it : views) {
            list.append(Py::asObject(it->getPyObject()));
        }
        return Py::new_reference_to(list);
    }
    PY_CATCH;
}

PyObject* DocumentPy::save(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    PY_TRY
    {
        bool ok = getDocumentPtr()->save();
        return Py::new_reference_to(Py::Boolean(ok));
    }
    PY_CATCH;
}

PyObject* DocumentPy::saveAs(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    PY_TRY
    {
        bool ok = getDocumentPtr()->saveAs();
        return Py::new_reference_to(Py::Boolean(ok));
    }
    PY_CATCH;
}

PyObject* DocumentPy::sendMsgToViews(PyObject* args)
{
    char* msg;
    if (!PyArg_ParseTuple(args, "s", &msg)) {
        return nullptr;
    }

    PY_TRY
    {
        getDocumentPtr()->sendMsgToViews(msg);
        Py_Return;
    }
    PY_CATCH;
}

PyObject* DocumentPy::mergeProject(PyObject* args)
{
    char* filename;
    if (!PyArg_ParseTuple(args, "s", &filename)) {
        return nullptr;
    }

    PY_TRY
    {
        Base::FileInfo fi(filename);
        Base::ifstream str(fi, std::ios::in | std::ios::binary);
        App::Document* doc = getDocumentPtr()->getDocument();
        MergeDocuments md(doc);
        md.importObjects(str);

        Py_Return;
    }
    PY_CATCH;
}

PyObject* DocumentPy::toggleTreeItem(PyObject* args)
{
    PyObject* object;
    const char* subname = nullptr;
    int mod = 0;
    if (!PyArg_ParseTuple(args, "O!|is", &(App::DocumentObjectPy::Type), &object, &mod, &subname)) {
        return nullptr;
    }

    App::DocumentObject* Object = static_cast<App::DocumentObjectPy*>(object)->getDocumentObjectPtr();
    App::DocumentObject* parent = nullptr;
    if (subname) {
        App::DocumentObject* sobj = Object->getSubObject(subname);
        if (!sobj) {
            PyErr_SetString(PyExc_ValueError, "Subobject not found");
            return nullptr;
        }

        parent = Object;
        Object = sobj;
    }

    // get the gui document of the Assembly Item
    // ActiveAppDoc = Item->getDocument();
    // ActiveGuiDoc = Gui::Application::Instance->getDocument(getDocumentPtr());
    auto ActiveVp = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        getDocumentPtr()->getViewProvider(Object)
    );
    switch (mod) {
        case 0:
            getDocumentPtr()->signalExpandObject(*ActiveVp, TreeItemMode::ToggleItem, parent, subname);
            break;
        case 1:
            getDocumentPtr()->signalExpandObject(*ActiveVp, TreeItemMode::CollapseItem, parent, subname);
            break;
        case 2:
            getDocumentPtr()->signalExpandObject(*ActiveVp, TreeItemMode::ExpandItem, parent, subname);
            break;
        case 3:
            getDocumentPtr()->signalExpandObject(*ActiveVp, TreeItemMode::ExpandPath, parent, subname);
            break;
        default:
            PyErr_SetString(PyExc_ValueError, "Item mode out of range");
            return nullptr;
    }

    Py_Return;
}

PyObject* DocumentPy::scrollToTreeItem(PyObject* args)
{
    PyObject* view;
    if (!PyArg_ParseTuple(args, "O!", &(Gui::ViewProviderDocumentObjectPy::Type), &view)) {
        return nullptr;
    }

    Gui::ViewProviderDocumentObject* vp
        = static_cast<ViewProviderDocumentObjectPy*>(view)->getViewProviderDocumentObjectPtr();
    getDocumentPtr()->signalScrollToObject(*vp);

    Py_Return;
}

PyObject* DocumentPy::toggleInSceneGraph(PyObject* args)
{
    PyObject* view;
    if (!PyArg_ParseTuple(args, "O!", &(Gui::ViewProviderPy::Type), &view)) {
        return nullptr;
    }

    Gui::ViewProvider* vp = static_cast<ViewProviderPy*>(view)->getViewProviderPtr();
    getDocumentPtr()->toggleInSceneGraph(vp);

    Py_Return;
}

Py::Object DocumentPy::getActiveObject() const
{
    App::DocumentObject* object = getDocumentPtr()->getDocument()->getActiveObject();
    if (object) {
        ViewProvider* viewObj = getDocumentPtr()->getViewProvider(object);
        return Py::Object(viewObj->getPyObject(), true);
    }
    else {
        return Py::None();
    }
}

void DocumentPy::setActiveObject(Py::Object /*arg*/)
{
    throw Py::AttributeError("'Document' object attribute 'ActiveObject' is read-only");
}

Py::Object DocumentPy::getActiveView() const
{
    Gui::MDIView* view = getDocumentPtr()->getActiveView();
    if (view) {
        // already incremented in getPyObject().
        return Py::Object(view->getPyObject(), true);
    }
    else {
        return Py::None();
    }
}

void DocumentPy::setActiveView(Py::Object /*arg*/)
{
    throw Py::AttributeError("'Document' object attribute 'ActiveView' is read-only");
}

Py::Object DocumentPy::getDocument() const
{
    App::Document* doc = getDocumentPtr()->getDocument();
    if (doc) {
        // already incremented in getPyObject().
        return Py::Object(doc->getPyObject(), true);
    }
    else {
        return Py::None();
    }
}

Py::Object DocumentPy::getEditingTransform() const
{
    return Py::asObject(
        new Base::MatrixPy(new Base::Matrix4D(getDocumentPtr()->getEditingTransform()))
    );
}

void DocumentPy::setEditingTransform(Py::Object arg)
{
    if (!PyObject_TypeCheck(arg.ptr(), &Base::MatrixPy::Type)) {
        throw Py::TypeError("Expecting type of matrix");
    }

    getDocumentPtr()->setEditingTransform(*static_cast<Base::MatrixPy*>(arg.ptr())->getMatrixPtr());
}

Py::Object DocumentPy::getInEditInfo() const
{
    ViewProviderDocumentObject* vp = nullptr;
    std::string subname, subelement;
    int mode = 0;
    getDocumentPtr()->getInEdit(&vp, &subname, &mode, &subelement);
    if (!vp || !vp->getObject() || !vp->getObject()->isAttachedToDocument()) {
        return Py::None();
    }

    return Py::TupleN(
        Py::Object(vp->getObject()->getPyObject(), true),
        Py::String(subname),
        Py::String(subelement),
        Py::Long(mode)
    );
}

void DocumentPy::setInEditInfo(Py::Object arg)
{
    PyObject* pyobj;
    const char* subname;
    if (!PyArg_ParseTuple(arg.ptr(), "O!s", &Gui::ViewProviderDocumentObjectPy::Type, &pyobj, &subname)) {
        throw Py::Exception();
    }

    getDocumentPtr()->setInEdit(
        static_cast<ViewProviderDocumentObjectPy*>(pyobj)->getViewProviderDocumentObjectPtr(),
        subname
    );
}

Py::Long DocumentPy::getEditMode() const
{
    int mode = -1;
    getDocumentPtr()->getInEdit(nullptr, nullptr, &mode);

    return Py::Long(mode);
}

PyObject* DocumentPy::openCommand(PyObject* arg, PyObject* /*kwd*/)
{
    const char* name = nullptr;
    if (!PyArg_ParseTuple(arg, "s", &name)) {
        throw Py::Exception();
    }
    int tid = getDocumentPtr()->openCommand(name);

    return Py::new_reference_to(Py::Long(tid));
}
PyObject* DocumentPy::commitCommand(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    getDocumentPtr()->commitCommand();

    Py_Return;
}
PyObject* DocumentPy::abortCommand(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    getDocumentPtr()->abortCommand();

    Py_Return;
}

PyObject* DocumentPy::storePersonalViewContext(PyObject* args)
{
    const char* actorId = nullptr;
    PyObject* context = nullptr;
    if (!PyArg_ParseTuple(args, "sO", &actorId, &context)) {
        return nullptr;
    }
    PY_TRY
    {
        getDocumentPtr()->storePersonalViewContext(
            actorId, personalViewContextFromPython(context));
        Py_Return;
    }
    PY_CATCH;
}

PyObject* DocumentPy::getPersonalViewContext(PyObject* args) const
{
    const char* actorId = nullptr;
    if (!PyArg_ParseTuple(args, "s", &actorId)) {
        return nullptr;
    }
    PY_TRY
    {
        const auto context = getDocumentPtr()->personalViewContext(actorId);
        if (!context) {
            Py_Return;
        }
        return Py::new_reference_to(personalViewContextToPython(*context));
    }
    PY_CATCH;
}

PyObject* DocumentPy::removePersonalViewContext(PyObject* args)
{
    const char* actorId = nullptr;
    if (!PyArg_ParseTuple(args, "s", &actorId)) {
        return nullptr;
    }
    PY_TRY
    {
        return Py::new_reference_to(
            Py::Boolean(getDocumentPtr()->removePersonalViewContext(actorId)));
    }
    PY_CATCH;
}

PyObject* DocumentPy::renderPersonalViewContext(PyObject* args) const
{
    const char* actorId = nullptr;
    const char* background = "Current";
    Gui::PersonalViewImageOptions options;
    if (!PyArg_ParseTuple(args,
                          "s|iisi",
                          &actorId,
                          &options.width,
                          &options.height,
                          &background,
                          &options.samples)) {
        return nullptr;
    }
    options.background = background;
    PY_TRY
    {
        const auto png = getDocumentPtr()->renderPersonalViewContext(actorId, options);
        if (!png) {
            Py_Return;
        }
        return PyBytes_FromStringAndSize(
            reinterpret_cast<const char*>(png->data()),
            static_cast<Py_ssize_t>(png->size()));
    }
    PY_CATCH;
}

Py::Boolean DocumentPy::getTransacting() const
{
    return {getDocumentPtr()->isPerformingTransaction()};
}

Py::Boolean DocumentPy::getModified() const
{
    return {getDocumentPtr()->isModified()};
}

void DocumentPy::setModified(Py::Boolean arg)
{
    getDocumentPtr()->setModified(arg);
}

Py::List DocumentPy::getTreeRootObjects() const
{
    std::vector<App::DocumentObject*> objs = getDocumentPtr()->getTreeRootObjects();
    Py::List res;

    for (auto obj : objs) {
        // Note: Here we must force the Py::Object to own this Python object as getPyObject()
        // increments the counter
        res.append(Py::Object(obj->getPyObject(), true));
    }

    return res;
}


PyObject* DocumentPy::getCustomAttributes(const char* attr) const
{
    // Note: Here we want to return only a document object if its
    // name matches 'attr'. However, it is possible to have an object
    // with the same name as an attribute. If so, we return 0 as other-
    // wise it wouldn't be possible to address this attribute any more.
    // The object must then be addressed by the getObject() method directly.
    if (!this->ob_type->tp_dict) {
        if (PyType_Ready(this->ob_type) < 0) {
            return nullptr;
        }
    }

    PyObject* item = PyDict_GetItemString(this->ob_type->tp_dict, attr);
    if (item) {
        return nullptr;
    }

    // search for an object with this name
    ViewProvider* obj = getDocumentPtr()->getViewProviderByName(attr);

    return (obj ? obj->getPyObject() : nullptr);
}

int DocumentPy::setCustomAttributes(const char* attr, PyObject*)
{
    // Note: Here we want to return only a document object if its
    // name matches 'attr'. However, it is possible to have an object
    // with the same name as an attribute. If so, we return 0 as other-
    // wise it wouldn't be possible to address this attribute any more.
    // The object must then be addressed by the getObject() method directly.
    if (!this->ob_type->tp_dict) {
        if (PyType_Ready(this->ob_type) < 0) {
            return 0;
        }
    }

    PyObject* item = PyDict_GetItemString(this->ob_type->tp_dict, attr);
    if (item) {
        return 0;
    }

    ViewProvider* obj = getDocumentPtr()->getViewProviderByName(attr);
    if (obj) {
        std::stringstream str;
        str << "'Document' object attribute '" << attr << "' must not be set this way" << std::ends;
        throw Py::AttributeError(str.str());
    }

    return 0;
}
