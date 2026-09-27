/***************************************************************************
 *   Copyright (c) 2018 Stefan Tröger <stefantroeger@gmx.net>              *
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

#include <Base/Interpreter.h>

#include "Application.h"
#include "Document.h"
#include "DocumentObserverPython.h"
#include "GuiPythonGate.h"
#include "ViewProvider.h"
#include "ViewProviderDocumentObject.h"

#include <App/Document.h>
#include <App/DocumentObject.h>

#include <string_view>


using namespace Gui;
namespace sp = std::placeholders;

std::vector<DocumentObserverPython*> DocumentObserverPython::_instances;

void DocumentObserverPython::addObserver(const Py::Object& obj)
{
    _instances.push_back(new DocumentObserverPython(obj));
}

void DocumentObserverPython::removeObserver(const Py::Object& obj)
{
    DocumentObserverPython* obs = nullptr;
    for (std::vector<DocumentObserverPython*>::iterator it = _instances.begin();
         it != _instances.end();
         ++it) {
        if ((*it)->inst == obj) {
            obs = *it;
            _instances.erase(it);
            break;
        }
    }

    delete obs;
}

DocumentObserverPython::DocumentObserverPython(const Py::Object& obj)
    : inst(obj)
{
    // NOLINTBEGIN
#define FC_PY_ELEMENT_ARG1(_name1, _name2) \
    do { \
        FC_PY_GetCallable(obj.ptr(), "slot" #_name1, py##_name1.py); \
        if (!py##_name1.py.isNone()) \
            py##_name1.slot = Application::Instance->signal##_name2.connect( \
                std::bind(&DocumentObserverPython::slot##_name1, this, sp::_1) \
            ); \
    } while (0);

#define FC_PY_ELEMENT_ARG2(_name1, _name2) \
    do { \
        FC_PY_GetCallable(obj.ptr(), "slot" #_name1, py##_name1.py); \
        if (!py##_name1.py.isNone()) \
            py##_name1.slot = Application::Instance->signal##_name2.connect( \
                std::bind(&DocumentObserverPython::slot##_name1, this, sp::_1, sp::_2) \
            ); \
    } while (0);

    FC_PY_ELEMENT_ARG1(CreatedDocument, NewDocument)
    FC_PY_ELEMENT_ARG1(DeletedDocument, DeleteDocument)
    FC_PY_ELEMENT_ARG1(RelabelDocument, RelabelDocument)
    FC_PY_ELEMENT_ARG1(RenameDocument, RenameDocument)
    FC_PY_ELEMENT_ARG1(ActivateDocument, ActiveDocument)
    FC_PY_ELEMENT_ARG1(CreatedObject, NewObject)
    FC_PY_ELEMENT_ARG1(DeletedObject, DeletedObject)
    FC_PY_ELEMENT_ARG2(BeforeChangeObject, BeforeChangeObject)
    FC_PY_ELEMENT_ARG2(ChangedObject, ChangedObject)
    FC_PY_ELEMENT_ARG1(InEdit, InEdit)
    FC_PY_ELEMENT_ARG1(ResetEdit, ResetEdit)
    // NOLINTEND
}

DocumentObserverPython::~DocumentObserverPython() = default;

namespace
{

std::string escapeJsonString(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        if (character == '"' || character == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

std::string jsonStringField(const std::string& payloadJson, const char* key)
{
    const std::string needle = std::string(R"(")") + key + R"(":")";
    const auto start = payloadJson.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    std::size_t position = start + needle.size();
    std::string value;
    while (position < payloadJson.size()) {
        const char character = payloadJson[position++];
        if (character == '\\' && position < payloadJson.size()) {
            value.push_back(payloadJson[position++]);
            continue;
        }
        if (character == '"') {
            break;
        }
        value.push_back(character);
    }
    return value;
}

std::string makeObjectPayloadJson(const Gui::ViewProvider& view)
{
    std::string documentName;
    std::string objectName;
    if (const auto* objectView = dynamic_cast<const ViewProviderDocumentObject*>(&view)) {
        if (const auto* object = objectView->getObject()) {
            if (const auto* document = object->getDocument()) {
                documentName = document->getName();
                objectName = object->getNameInDocument();
            }
        }
    }
    return std::string(R"({"document":")") + escapeJsonString(documentName) + R"(","object":")"
        + escapeJsonString(objectName) + R"("})";
}

template<typename Fn>
void admitObserverDelivery(Fn&& fn, const char* eventKind, const std::string& payloadJson)
{
    const auto outcome = GuiPythonGate::tryAdmit(
        GuiPythonGateCallbackKind::ObserverDelivery,
        std::forward<Fn>(fn));
    if (outcome.queued()) {
        GuiPythonObserverValueEvent event;
        event.eventKind = eventKind;
        event.payloadJson = payloadJson;
        GuiPythonGate::enqueueObserverValueEvent(std::move(event));
    }
}

}  // namespace

void DocumentObserverPython::installValueEventHandler()
{
    GuiPythonGate::setObserverValueEventHandler(
        [](const GuiPythonObserverValueEvent& event) {
            for (auto* observer : _instances) {
                observer->deliverValueEvent(event);
            }
        });
}

void DocumentObserverPython::deliverValueEvent(const GuiPythonObserverValueEvent& event)
{
    const std::string documentName = jsonStringField(event.payloadJson, "document");
    if (documentName.empty()) {
        return;
    }
    auto* appDocument = App::GetApplication().getDocument(documentName.c_str());
    if (!appDocument || !Application::Instance) {
        return;
    }
    auto* guiDocument = Application::Instance->getDocument(appDocument);
    if (!guiDocument) {
        return;
    }

    Base::PyGILStateLocker lock;
    try {
        if (event.eventKind == "CreatedDocument" || event.eventKind == "DeletedDocument"
            || event.eventKind == "RelabelDocument" || event.eventKind == "RenameDocument"
            || event.eventKind == "ActivateDocument") {
            Py::Tuple args(1);
            args.setItem(0, Py::asObject(guiDocument->getPyObject()));
            if (event.eventKind == "CreatedDocument") {
                Base::pyCall(pyCreatedDocument.ptr(), args.ptr());
            }
            else if (event.eventKind == "DeletedDocument") {
                Base::pyCall(pyDeletedDocument.ptr(), args.ptr());
            }
            else if (event.eventKind == "RelabelDocument") {
                Base::pyCall(pyRelabelDocument.ptr(), args.ptr());
            }
            else if (event.eventKind == "RenameDocument") {
                Base::pyCall(pyRenameDocument.ptr(), args.ptr());
            }
            else {
                Base::pyCall(pyActivateDocument.ptr(), args.ptr());
            }
            return;
        }

        const std::string objectName = jsonStringField(event.payloadJson, "object");
        if (objectName.empty()) {
            return;
        }
        auto* object = appDocument->getObject(objectName.c_str());
        if (!object) {
            return;
        }
        auto* viewProvider = guiDocument->getViewProvider(object);
        if (!viewProvider) {
            return;
        }

        if (event.eventKind == "CreatedObject" || event.eventKind == "DeletedObject") {
            Py::Tuple args(1);
            args.setItem(0, Py::asObject(viewProvider->getPyObject()));
            if (event.eventKind == "CreatedObject") {
                Base::pyCall(pyCreatedObject.ptr(), args.ptr());
            }
            else {
                Base::pyCall(pyDeletedObject.ptr(), args.ptr());
            }
            return;
        }

        const std::string propertyName = jsonStringField(event.payloadJson, "property");
        if (event.eventKind == "BeforeChangeObject" || event.eventKind == "ChangedObject") {
            if (propertyName.empty()) {
                return;
            }
            Py::Tuple args(2);
            args.setItem(0, Py::asObject(viewProvider->getPyObject()));
            args.setItem(1, Py::String(propertyName));
            if (event.eventKind == "BeforeChangeObject") {
                Base::pyCall(pyBeforeChangeObject.ptr(), args.ptr());
            }
            else {
                Base::pyCall(pyChangedObject.ptr(), args.ptr());
            }
            return;
        }

        if (event.eventKind == "InEdit" || event.eventKind == "ResetEdit") {
            auto* objectView = freecad_cast<ViewProviderDocumentObject*>(viewProvider);
            if (!objectView) {
                return;
            }
            Py::Tuple args(1);
            args.setItem(0, Py::asObject(objectView->getPyObject()));
            if (event.eventKind == "InEdit") {
                Base::pyCall(pyInEdit.ptr(), args.ptr());
            }
            else {
                Base::pyCall(pyResetEdit.ptr(), args.ptr());
            }
        }
    }
    catch (Py::Exception&) {
        Base::PyException exception;
        exception.reportException();
    }
}

void DocumentObserverPython::slotCreatedDocument(const Gui::Document& Doc)
{
    admitObserverDelivery(
        [this, &Doc] {
            try {
                Py::Tuple args(1);
                args.setItem(0, Py::asObject(const_cast<Gui::Document&>(Doc).getPyObject()));
                Base::pyCall(pyCreatedDocument.ptr(), args.ptr());
            }
            catch (Py::Exception&) {
                Base::PyException e;
                e.reportException();
            }
        },
        "CreatedDocument",
        std::string("{\"document\":\"") + Doc.getDocument()->getName() + "\"}");
}

void DocumentObserverPython::slotDeletedDocument(const Gui::Document& Doc)
{
    admitObserverDelivery(
        [this, &Doc] {
            try {
                Py::Tuple args(1);
                args.setItem(0, Py::asObject(const_cast<Gui::Document&>(Doc).getPyObject()));
                Base::pyCall(pyDeletedDocument.ptr(), args.ptr());
            }
            catch (Py::Exception&) {
                Base::PyException e;
                e.reportException();
            }
        },
        "DeletedDocument",
        std::string("{\"document\":\"") + Doc.getDocument()->getName() + "\"}");
}

void DocumentObserverPython::slotRelabelDocument(const Gui::Document& Doc)
{
    admitObserverDelivery(
        [this, &Doc] {
            try {
                Py::Tuple args(1);
                args.setItem(0, Py::asObject(const_cast<Gui::Document&>(Doc).getPyObject()));
                Base::pyCall(pyRelabelDocument.ptr(), args.ptr());
            }
            catch (Py::Exception&) {
                Base::PyException e;
                e.reportException();
            }
        },
        "RelabelDocument",
        std::string("{\"document\":\"") + Doc.getDocument()->getName() + "\"}");
}

void DocumentObserverPython::slotRenameDocument(const Gui::Document& Doc)
{
    admitObserverDelivery(
        [this, &Doc] {
            try {
                Py::Tuple args(1);
                args.setItem(0, Py::asObject(const_cast<Gui::Document&>(Doc).getPyObject()));
                Base::pyCall(pyRenameDocument.ptr(), args.ptr());
            }
            catch (Py::Exception&) {
                Base::PyException e;
                e.reportException();
            }
        },
        "RenameDocument",
        std::string("{\"document\":\"") + Doc.getDocument()->getName() + "\"}");
}

void DocumentObserverPython::slotActivateDocument(const Gui::Document& Doc)
{
    admitObserverDelivery(
        [this, &Doc] {
            try {
                Py::Tuple args(1);
                args.setItem(0, Py::asObject(const_cast<Gui::Document&>(Doc).getPyObject()));
                Base::pyCall(pyActivateDocument.ptr(), args.ptr());
            }
            catch (Py::Exception&) {
                Base::PyException e;
                e.reportException();
            }
        },
        "ActivateDocument",
        std::string("{\"document\":\"") + Doc.getDocument()->getName() + "\"}");
}

void DocumentObserverPython::slotCreatedObject(const Gui::ViewProvider& Obj)
{
    admitObserverDelivery(
        [this, &Obj] {
            try {
                Py::Tuple args(1);
                args.setItem(0, Py::asObject(const_cast<Gui::ViewProvider&>(Obj).getPyObject()));
                Base::pyCall(pyCreatedObject.ptr(), args.ptr());
            }
            catch (Py::Exception&) {
                Base::PyException e;
                e.reportException();
            }
        },
        "CreatedObject",
        makeObjectPayloadJson(Obj));
}

void DocumentObserverPython::slotDeletedObject(const Gui::ViewProvider& Obj)
{
    admitObserverDelivery(
        [this, &Obj] {
            try {
                Py::Tuple args(1);
                args.setItem(0, Py::asObject(const_cast<Gui::ViewProvider&>(Obj).getPyObject()));
                Base::pyCall(pyDeletedObject.ptr(), args.ptr());
            }
            catch (Py::Exception&) {
                Base::PyException e;
                e.reportException();
            }
        },
        "DeletedObject",
        makeObjectPayloadJson(Obj));
}

void DocumentObserverPython::slotBeforeChangeObject(
    const Gui::ViewProvider& Obj,
    const App::Property& Prop
)
{
    const char* prop_name = Obj.getPropertyName(&Prop);
    if (!prop_name) {
        return;
    }
    const std::string payload = makeObjectPayloadJson(Obj);
    admitObserverDelivery(
        [this, &Obj, prop_name] {
            try {
                Py::Tuple args(2);
                args.setItem(0, Py::asObject(const_cast<Gui::ViewProvider&>(Obj).getPyObject()));
                args.setItem(1, Py::String(prop_name));
                Base::pyCall(pyBeforeChangeObject.ptr(), args.ptr());
            }
            catch (Py::Exception&) {
                Base::PyException e;
                e.reportException();
            }
        },
        "BeforeChangeObject",
        payload.substr(0, payload.size() - 1) + R"(,"property":")" + escapeJsonString(prop_name)
            + R"("})");
}

void DocumentObserverPython::slotChangedObject(const Gui::ViewProvider& Obj, const App::Property& Prop)
{
    const char* prop_name = Obj.getPropertyName(&Prop);
    if (!prop_name) {
        return;
    }
    const std::string payload = makeObjectPayloadJson(Obj);
    admitObserverDelivery(
        [this, &Obj, prop_name] {
            try {
                Py::Tuple args(2);
                args.setItem(0, Py::asObject(const_cast<Gui::ViewProvider&>(Obj).getPyObject()));
                args.setItem(1, Py::String(prop_name));
                Base::pyCall(pyChangedObject.ptr(), args.ptr());
            }
            catch (Py::Exception&) {
                Base::PyException e;
                e.reportException();
            }
        },
        "ChangedObject",
        payload.substr(0, payload.size() - 1) + R"(,"property":")" + escapeJsonString(prop_name)
            + R"("})");
}

void DocumentObserverPython::slotInEdit(const Gui::ViewProviderDocumentObject& Obj)
{
    admitObserverDelivery(
        [this, &Obj] {
            try {
                Py::Tuple args(1);
                args.setItem(
                    0,
                    Py::asObject(const_cast<Gui::ViewProviderDocumentObject&>(Obj).getPyObject()));
                Base::pyCall(pyInEdit.ptr(), args.ptr());
            }
            catch (Py::Exception&) {
                Base::PyException e;
                e.reportException();
            }
        },
        "InEdit",
        makeObjectPayloadJson(Obj));
}

void DocumentObserverPython::slotResetEdit(const Gui::ViewProviderDocumentObject& Obj)
{
    admitObserverDelivery(
        [this, &Obj] {
            try {
                Py::Tuple args(1);
                args.setItem(
                    0,
                    Py::asObject(const_cast<Gui::ViewProviderDocumentObject&>(Obj).getPyObject()));
                Base::pyCall(pyResetEdit.ptr(), args.ptr());
            }
            catch (Py::Exception&) {
                Base::PyException e;
                e.reportException();
            }
        },
        "ResetEdit",
        makeObjectPayloadJson(Obj));
}
