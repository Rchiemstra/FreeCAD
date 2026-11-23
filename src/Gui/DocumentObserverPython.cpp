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
#include <Python.h>

#include "Application.h"
#include "Document.h"
#include "DocumentObserverPython.h"
#include "GuiPythonGate.h"
#include "ViewProvider.h"
#include "ViewProviderDocumentObject.h"

#include <App/Document.h>
#include <App/DocumentObject.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


using namespace Gui;

namespace
{

struct ObserverEntry
{
    std::uint64_t id {0};
    std::shared_ptr<DocumentObserverPython> observer;
};

struct ObserverRegistry
{
    std::uint64_t nextId {1};
    std::vector<ObserverEntry> entries;
};

// All accesses to observerRegistry() are serialized by the Python GIL:
// - addObserver() and removeObserver() hold Base::PyGILStateLocker.
// - deliverDeferredValueEvent() is invoked by GuiPythonGate::pumpQueuedCallbacks()
//   under its existing GilTryLock scope and verifies PyGILState_Check().
// - Registered fastsignals slots look up entries only inside GuiPythonGate::tryAdmit()'s
//   admitted callback, which runs under GilTryLock's existing GIL scope.
//
// No iterator or reference into registry.entries may be held across any Python
// execution (__getattr__, __eq__, Base::pyCall, or Py_DECREF/__del__), because
// Python code may re-entrantly mutate the registry or temporarily release the GIL.
//
// Intentional process-lifetime storage: observers never explicitly removed
// before Base::Interpreter().finalize() / Py_Finalize() remain allocated for
// process lifetime so post-finalization C++ static destruction never calls
// ~Py::Object() / Py_DECREF on a finalized interpreter (matching the historical
// raw-pointer _instances vector).
ObserverRegistry& observerRegistry()
{
    static auto* registry = new ObserverRegistry;
    return *registry;
}

// Precondition: Py_IsInitialized() && PyGILState_Check().
std::shared_ptr<DocumentObserverPython> findRegisteredObserver(const std::uint64_t registrationId)
{
    auto& registry = observerRegistry();
    const auto it = std::find_if(
        registry.entries.begin(),
        registry.entries.end(),
        [registrationId](const ObserverEntry& entry) { return entry.id == registrationId; });
    return (it != registry.entries.end()) ? it->observer : nullptr;
}

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

std::string makeDocumentPayloadJson(const Gui::Document& doc)
{
    std::string documentName;
    if (const auto* appDocument = doc.getDocument()) {
        documentName = appDocument->getName();
    }
    return std::string(R"({"document":")") + escapeJsonString(documentName) + R"("})";
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

std::string makePropertyPayloadJson(const Gui::ViewProvider& view, const char* propName)
{
    const std::string payload = makeObjectPayloadJson(view);
    return payload.substr(0, payload.size() - 1) + R"(,"property":")" + escapeJsonString(propName)
        + R"("})";
}

void invokeDocumentSlot(const Py::Object& slotCallable, const Gui::Document& doc)
{
    try {
        const Py::Object callable = slotCallable;
        if (callable.isNone()) {
            return;
        }
        Py::Tuple args(1);
        args.setItem(0, Py::asObject(const_cast<Gui::Document&>(doc).getPyObject()));
        Base::pyCall(callable.ptr(), args.ptr());
    }
    catch (Py::Exception&) {
        Base::PyException e;
        e.reportException();
    }
}

void invokeObjectSlot(const Py::Object& slotCallable, const Gui::ViewProvider& obj)
{
    try {
        const Py::Object callable = slotCallable;
        if (callable.isNone()) {
            return;
        }
        Py::Tuple args(1);
        args.setItem(0, Py::asObject(const_cast<Gui::ViewProvider&>(obj).getPyObject()));
        Base::pyCall(callable.ptr(), args.ptr());
    }
    catch (Py::Exception&) {
        Base::PyException e;
        e.reportException();
    }
}

void invokePropertySlot(
    const Py::Object& slotCallable,
    const Gui::ViewProvider& obj,
    const char* propName)
{
    try {
        const Py::Object callable = slotCallable;
        if (callable.isNone()) {
            return;
        }
        Py::Tuple args(2);
        args.setItem(0, Py::asObject(const_cast<Gui::ViewProvider&>(obj).getPyObject()));
        args.setItem(1, Py::String(propName));
        Base::pyCall(callable.ptr(), args.ptr());
    }
    catch (Py::Exception&) {
        Base::PyException e;
        e.reportException();
    }
}

void invokeEditSlot(const Py::Object& slotCallable, const Gui::ViewProviderDocumentObject& obj)
{
    try {
        const Py::Object callable = slotCallable;
        if (callable.isNone()) {
            return;
        }
        Py::Tuple args(1);
        args.setItem(
            0,
            Py::asObject(const_cast<Gui::ViewProviderDocumentObject&>(obj).getPyObject()));
        Base::pyCall(callable.ptr(), args.ptr());
    }
    catch (Py::Exception&) {
        Base::PyException e;
        e.reportException();
    }
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

template<typename Fn>
void admitRegisteredObserverDelivery(
    const std::uint64_t registrationId,
    Fn&& fn,
    const char* eventKind,
    const std::string& payloadJson)
{
    admitObserverDelivery(
        [registrationId, &fn]() {
            if (!Py_IsInitialized() || !PyGILState_Check()) {
                return;
            }
            std::shared_ptr<DocumentObserverPython> observer =
                findRegisteredObserver(registrationId);
            if (!observer) {
                return;
            }
            fn(*observer);
            // `observer` drops here before returning from GuiPythonGate::tryAdmit(),
            // so even on self-removal every Py::Object member is released inside
            // tryAdmit()'s existing GilTryLock scope.
        },
        eventKind,
        payloadJson);
}

}  // namespace

void DocumentObserverPython::addObserver(const Py::Object& obj)
{
    if (!Py_IsInitialized()) {
        return;
    }
    Base::PyGILStateLocker lock;

    std::shared_ptr<DocumentObserverPython> observer(
        new DocumentObserverPython(obj, DeferConnectTag {}));

    auto& registry = observerRegistry();
    // Reserve capacity before connecting slots so appending the new entry
    // cannot throw or trigger observer destruction mid-mutation.
    registry.entries.reserve(registry.entries.size() + 1);
    const std::uint64_t id = registry.nextId++;
    observer->connectRegisteredSlots(id);
    registry.entries.push_back(ObserverEntry {id, std::move(observer)});
}

void DocumentObserverPython::removeObserver(const Py::Object& obj)
{
    if (!Py_IsInitialized()) {
        return;
    }
    Base::PyGILStateLocker lock;

    auto& registry = observerRegistry();
    // Snapshot entries before Python equality (__eq__), which may re-entrantly
    // mutate registry.entries or temporarily release the GIL.
    std::vector<ObserverEntry> snapshot = registry.entries;

    std::uint64_t matchedId = 0;
    for (const auto& entry : snapshot) {
        if (entry.observer->inst == obj) {
            matchedId = entry.id;
            break;
        }
    }
    // Release snapshot references before searching/erasing registry.entries so
    // any Python __del__ finalizer for an entry already removed during __eq__
    // runs with no registry iterators held.
    snapshot.clear();

    if (matchedId == 0) {
        return;
    }

    std::shared_ptr<DocumentObserverPython> removed;
    const auto it = std::find_if(
        registry.entries.begin(),
        registry.entries.end(),
        [matchedId](const ObserverEntry& entry) { return entry.id == matchedId; });
    if (it != registry.entries.end()) {
        // Move the shared_ptr out before vector::erase so erase never invokes
        // ~DocumentObserverPython() or Python finalizers while shifting elements.
        removed = std::move(it->observer);
        removed->deactivate();
        registry.entries.erase(it);
    }

    if (removed) {
        removed->disconnectSlots();
    }
    // `removed` drops here under the active GIL with registry.entries already
    // updated and all slots disconnected.
}

DocumentObserverPython::DocumentObserverPython(const Py::Object& obj, DeferConnectTag)
    : inst(obj)
{
    // NOLINTBEGIN
#define FC_PY_GET_SLOT_CALLABLE(_name) \
    FC_PY_GetCallable(obj.ptr(), "slot" #_name, py##_name.py)

    FC_PY_GET_SLOT_CALLABLE(CreatedDocument);
    FC_PY_GET_SLOT_CALLABLE(DeletedDocument);
    FC_PY_GET_SLOT_CALLABLE(RelabelDocument);
    FC_PY_GET_SLOT_CALLABLE(RenameDocument);
    FC_PY_GET_SLOT_CALLABLE(ActivateDocument);
    FC_PY_GET_SLOT_CALLABLE(CreatedObject);
    FC_PY_GET_SLOT_CALLABLE(DeletedObject);
    FC_PY_GET_SLOT_CALLABLE(BeforeChangeObject);
    FC_PY_GET_SLOT_CALLABLE(ChangedObject);
    FC_PY_GET_SLOT_CALLABLE(InEdit);
    FC_PY_GET_SLOT_CALLABLE(ResetEdit);
#undef FC_PY_GET_SLOT_CALLABLE
    // NOLINTEND
}

DocumentObserverPython::DocumentObserverPython(const Py::Object& obj)
    : DocumentObserverPython(obj, DeferConnectTag {})
{
    connectDirectSlots();
}

DocumentObserverPython::~DocumentObserverPython()
{
    deactivate();
    disconnectSlots();
}

void DocumentObserverPython::connectDirectSlots()
{
    if (!Application::Instance) {
        return;
    }
    auto* app = Application::Instance;
    if (!pyCreatedDocument.py.isNone()) {
        pyCreatedDocument.slot = app->signalNewDocument.connect(
            [this](const Gui::Document& doc, bool) { slotCreatedDocument(doc); });
    }
    if (!pyDeletedDocument.py.isNone()) {
        pyDeletedDocument.slot = app->signalDeleteDocument.connect(
            [this](const Gui::Document& doc) { slotDeletedDocument(doc); });
    }
    if (!pyRelabelDocument.py.isNone()) {
        pyRelabelDocument.slot = app->signalRelabelDocument.connect(
            [this](const Gui::Document& doc) { slotRelabelDocument(doc); });
    }
    if (!pyRenameDocument.py.isNone()) {
        pyRenameDocument.slot = app->signalRenameDocument.connect(
            [this](const Gui::Document& doc) { slotRenameDocument(doc); });
    }
    if (!pyActivateDocument.py.isNone()) {
        pyActivateDocument.slot = app->signalActiveDocument.connect(
            [this](const Gui::Document& doc) { slotActivateDocument(doc); });
    }
    if (!pyCreatedObject.py.isNone()) {
        pyCreatedObject.slot = app->signalNewObject.connect(
            [this](const Gui::ViewProvider& view) { slotCreatedObject(view); });
    }
    if (!pyDeletedObject.py.isNone()) {
        pyDeletedObject.slot = app->signalDeletedObject.connect(
            [this](const Gui::ViewProvider& view) { slotDeletedObject(view); });
    }
    if (!pyBeforeChangeObject.py.isNone()) {
        pyBeforeChangeObject.slot = app->signalBeforeChangeObject.connect(
            [this](const Gui::ViewProvider& view, const App::Property& prop) {
                slotBeforeChangeObject(view, prop);
            });
    }
    if (!pyChangedObject.py.isNone()) {
        pyChangedObject.slot = app->signalChangedObject.connect(
            [this](const Gui::ViewProvider& view, const App::Property& prop) {
                slotChangedObject(view, prop);
            });
    }
    if (!pyInEdit.py.isNone()) {
        pyInEdit.slot = app->signalInEdit.connect(
            [this](const Gui::ViewProviderDocumentObject& view) { slotInEdit(view); });
    }
    if (!pyResetEdit.py.isNone()) {
        pyResetEdit.slot = app->signalResetEdit.connect(
            [this](const Gui::ViewProviderDocumentObject& view) { slotResetEdit(view); });
    }
}

void DocumentObserverPython::connectRegisteredSlots(const std::uint64_t registrationId)
{
    activeFlag = std::make_shared<std::atomic<bool>>(true);
    if (!Application::Instance) {
        return;
    }
    auto* app = Application::Instance;
    const auto flag = activeFlag;

    if (!pyCreatedDocument.py.isNone()) {
        pyCreatedDocument.slot = app->signalNewDocument.connect(
            [registrationId, flag](const Gui::Document& doc, bool) {
                if (!flag->load(std::memory_order_acquire)) {
                    return;
                }
                admitRegisteredObserverDelivery(
                    registrationId,
                    [&doc](DocumentObserverPython& self) {
                        invokeDocumentSlot(self.pyCreatedDocument.py, doc);
                    },
                    "CreatedDocument",
                    makeDocumentPayloadJson(doc));
            });
    }
    if (!pyDeletedDocument.py.isNone()) {
        pyDeletedDocument.slot = app->signalDeleteDocument.connect(
            [registrationId, flag](const Gui::Document& doc) {
                if (!flag->load(std::memory_order_acquire)) {
                    return;
                }
                admitRegisteredObserverDelivery(
                    registrationId,
                    [&doc](DocumentObserverPython& self) {
                        invokeDocumentSlot(self.pyDeletedDocument.py, doc);
                    },
                    "DeletedDocument",
                    makeDocumentPayloadJson(doc));
            });
    }
    if (!pyRelabelDocument.py.isNone()) {
        pyRelabelDocument.slot = app->signalRelabelDocument.connect(
            [registrationId, flag](const Gui::Document& doc) {
                if (!flag->load(std::memory_order_acquire)) {
                    return;
                }
                admitRegisteredObserverDelivery(
                    registrationId,
                    [&doc](DocumentObserverPython& self) {
                        invokeDocumentSlot(self.pyRelabelDocument.py, doc);
                    },
                    "RelabelDocument",
                    makeDocumentPayloadJson(doc));
            });
    }
    if (!pyRenameDocument.py.isNone()) {
        pyRenameDocument.slot = app->signalRenameDocument.connect(
            [registrationId, flag](const Gui::Document& doc) {
                if (!flag->load(std::memory_order_acquire)) {
                    return;
                }
                admitRegisteredObserverDelivery(
                    registrationId,
                    [&doc](DocumentObserverPython& self) {
                        invokeDocumentSlot(self.pyRenameDocument.py, doc);
                    },
                    "RenameDocument",
                    makeDocumentPayloadJson(doc));
            });
    }
    if (!pyActivateDocument.py.isNone()) {
        pyActivateDocument.slot = app->signalActiveDocument.connect(
            [registrationId, flag](const Gui::Document& doc) {
                if (!flag->load(std::memory_order_acquire)) {
                    return;
                }
                admitRegisteredObserverDelivery(
                    registrationId,
                    [&doc](DocumentObserverPython& self) {
                        invokeDocumentSlot(self.pyActivateDocument.py, doc);
                    },
                    "ActivateDocument",
                    makeDocumentPayloadJson(doc));
            });
    }
    if (!pyCreatedObject.py.isNone()) {
        pyCreatedObject.slot = app->signalNewObject.connect(
            [registrationId, flag](const Gui::ViewProvider& view) {
                if (!flag->load(std::memory_order_acquire)) {
                    return;
                }
                admitRegisteredObserverDelivery(
                    registrationId,
                    [&view](DocumentObserverPython& self) {
                        invokeObjectSlot(self.pyCreatedObject.py, view);
                    },
                    "CreatedObject",
                    makeObjectPayloadJson(view));
            });
    }
    if (!pyDeletedObject.py.isNone()) {
        pyDeletedObject.slot = app->signalDeletedObject.connect(
            [registrationId, flag](const Gui::ViewProvider& view) {
                if (!flag->load(std::memory_order_acquire)) {
                    return;
                }
                admitRegisteredObserverDelivery(
                    registrationId,
                    [&view](DocumentObserverPython& self) {
                        invokeObjectSlot(self.pyDeletedObject.py, view);
                    },
                    "DeletedObject",
                    makeObjectPayloadJson(view));
            });
    }
    if (!pyBeforeChangeObject.py.isNone()) {
        pyBeforeChangeObject.slot = app->signalBeforeChangeObject.connect(
            [registrationId, flag](const Gui::ViewProvider& view, const App::Property& prop) {
                if (!flag->load(std::memory_order_acquire)) {
                    return;
                }
                const char* propName = view.getPropertyName(&prop);
                if (!propName) {
                    return;
                }
                admitRegisteredObserverDelivery(
                    registrationId,
                    [&view, propName](DocumentObserverPython& self) {
                        invokePropertySlot(self.pyBeforeChangeObject.py, view, propName);
                    },
                    "BeforeChangeObject",
                    makePropertyPayloadJson(view, propName));
            });
    }
    if (!pyChangedObject.py.isNone()) {
        pyChangedObject.slot = app->signalChangedObject.connect(
            [registrationId, flag](const Gui::ViewProvider& view, const App::Property& prop) {
                if (!flag->load(std::memory_order_acquire)) {
                    return;
                }
                const char* propName = view.getPropertyName(&prop);
                if (!propName) {
                    return;
                }
                admitRegisteredObserverDelivery(
                    registrationId,
                    [&view, propName](DocumentObserverPython& self) {
                        invokePropertySlot(self.pyChangedObject.py, view, propName);
                    },
                    "ChangedObject",
                    makePropertyPayloadJson(view, propName));
            });
    }
    if (!pyInEdit.py.isNone()) {
        pyInEdit.slot = app->signalInEdit.connect(
            [registrationId, flag](const Gui::ViewProviderDocumentObject& view) {
                if (!flag->load(std::memory_order_acquire)) {
                    return;
                }
                admitRegisteredObserverDelivery(
                    registrationId,
                    [&view](DocumentObserverPython& self) {
                        invokeEditSlot(self.pyInEdit.py, view);
                    },
                    "InEdit",
                    makeObjectPayloadJson(view));
            });
    }
    if (!pyResetEdit.py.isNone()) {
        pyResetEdit.slot = app->signalResetEdit.connect(
            [registrationId, flag](const Gui::ViewProviderDocumentObject& view) {
                if (!flag->load(std::memory_order_acquire)) {
                    return;
                }
                admitRegisteredObserverDelivery(
                    registrationId,
                    [&view](DocumentObserverPython& self) {
                        invokeEditSlot(self.pyResetEdit.py, view);
                    },
                    "ResetEdit",
                    makeObjectPayloadJson(view));
            });
    }
}

void DocumentObserverPython::deactivate() noexcept
{
    if (activeFlag) {
        activeFlag->store(false, std::memory_order_release);
    }
}

void DocumentObserverPython::disconnectSlots() noexcept
{
    pyCreatedDocument.slot.disconnect();
    pyDeletedDocument.slot.disconnect();
    pyRelabelDocument.slot.disconnect();
    pyRenameDocument.slot.disconnect();
    pyActivateDocument.slot.disconnect();
    pyCreatedObject.slot.disconnect();
    pyDeletedObject.slot.disconnect();
    pyBeforeChangeObject.slot.disconnect();
    pyChangedObject.slot.disconnect();
    pyInEdit.slot.disconnect();
    pyResetEdit.slot.disconnect();
}

Py::Object DocumentObserverPython::callableForEventKind(std::string_view eventKind) const
{
    if (eventKind == "CreatedDocument") {
        return pyCreatedDocument.py;
    }
    if (eventKind == "DeletedDocument") {
        return pyDeletedDocument.py;
    }
    if (eventKind == "RelabelDocument") {
        return pyRelabelDocument.py;
    }
    if (eventKind == "RenameDocument") {
        return pyRenameDocument.py;
    }
    if (eventKind == "ActivateDocument") {
        return pyActivateDocument.py;
    }
    if (eventKind == "CreatedObject") {
        return pyCreatedObject.py;
    }
    if (eventKind == "DeletedObject") {
        return pyDeletedObject.py;
    }
    if (eventKind == "BeforeChangeObject") {
        return pyBeforeChangeObject.py;
    }
    if (eventKind == "ChangedObject") {
        return pyChangedObject.py;
    }
    if (eventKind == "InEdit") {
        return pyInEdit.py;
    }
    if (eventKind == "ResetEdit") {
        return pyResetEdit.py;
    }
    return Py::None();
}

void DocumentObserverPython::installValueEventHandler()
{
    GuiPythonGate::setObserverValueEventHandler(
        &DocumentObserverPython::deliverDeferredValueEvent);
}

void DocumentObserverPython::deliverDeferredValueEvent(const GuiPythonObserverValueEvent& event)
{
    // GuiPythonGate::pumpQueuedCallbacks() acquires the GIL via GilTryLock
    // before invoking this handler. Require Python initialized and GIL held so
    // observerRegistry() is never read without GIL serialization and this path
    // never blocks the GUI thread waiting for the GIL.
    if (!Py_IsInitialized() || !PyGILState_Check()) {
        return;
    }

    auto& registry = observerRegistry();
    std::vector<std::uint64_t> candidateIds;
    candidateIds.reserve(registry.entries.size());
    for (const auto& entry : registry.entries) {
        candidateIds.push_back(entry.id);
    }

    for (const std::uint64_t registrationId : candidateIds) {
        if (!Py_IsInitialized() || !PyGILState_Check()) {
            break;
        }
        std::shared_ptr<DocumentObserverPython> observer =
            findRegisteredObserver(registrationId);
        if (!observer) {
            continue;
        }
        observer->deliverValueEvent(event);
        // `observer` drops here under the active GIL with no iterators into
        // registry.entries held, so self-removal teardown and any Python
        // __del__ finalizers run safely before the next registration lookup.
    }
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
    const Py::Object callable = callableForEventKind(event.eventKind);
    if (callable.isNone()) {
        return;
    }

    try {
        if (event.eventKind == "CreatedDocument" || event.eventKind == "DeletedDocument"
            || event.eventKind == "RelabelDocument" || event.eventKind == "RenameDocument"
            || event.eventKind == "ActivateDocument") {
            Py::Tuple args(1);
            args.setItem(0, Py::asObject(guiDocument->getPyObject()));
            Base::pyCall(callable.ptr(), args.ptr());
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
            Base::pyCall(callable.ptr(), args.ptr());
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
            Base::pyCall(callable.ptr(), args.ptr());
            return;
        }

        if (event.eventKind == "InEdit" || event.eventKind == "ResetEdit") {
            auto* objectView = freecad_cast<ViewProviderDocumentObject*>(viewProvider);
            if (!objectView) {
                return;
            }
            Py::Tuple args(1);
            args.setItem(0, Py::asObject(objectView->getPyObject()));
            Base::pyCall(callable.ptr(), args.ptr());
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
        [this, &Doc] { invokeDocumentSlot(pyCreatedDocument.py, Doc); },
        "CreatedDocument",
        makeDocumentPayloadJson(Doc));
}

void DocumentObserverPython::slotDeletedDocument(const Gui::Document& Doc)
{
    admitObserverDelivery(
        [this, &Doc] { invokeDocumentSlot(pyDeletedDocument.py, Doc); },
        "DeletedDocument",
        makeDocumentPayloadJson(Doc));
}

void DocumentObserverPython::slotRelabelDocument(const Gui::Document& Doc)
{
    admitObserverDelivery(
        [this, &Doc] { invokeDocumentSlot(pyRelabelDocument.py, Doc); },
        "RelabelDocument",
        makeDocumentPayloadJson(Doc));
}

void DocumentObserverPython::slotRenameDocument(const Gui::Document& Doc)
{
    admitObserverDelivery(
        [this, &Doc] { invokeDocumentSlot(pyRenameDocument.py, Doc); },
        "RenameDocument",
        makeDocumentPayloadJson(Doc));
}

void DocumentObserverPython::slotActivateDocument(const Gui::Document& Doc)
{
    admitObserverDelivery(
        [this, &Doc] { invokeDocumentSlot(pyActivateDocument.py, Doc); },
        "ActivateDocument",
        makeDocumentPayloadJson(Doc));
}

void DocumentObserverPython::slotCreatedObject(const Gui::ViewProvider& Obj)
{
    admitObserverDelivery(
        [this, &Obj] { invokeObjectSlot(pyCreatedObject.py, Obj); },
        "CreatedObject",
        makeObjectPayloadJson(Obj));
}

void DocumentObserverPython::slotDeletedObject(const Gui::ViewProvider& Obj)
{
    admitObserverDelivery(
        [this, &Obj] { invokeObjectSlot(pyDeletedObject.py, Obj); },
        "DeletedObject",
        makeObjectPayloadJson(Obj));
}

void DocumentObserverPython::slotBeforeChangeObject(
    const Gui::ViewProvider& Obj,
    const App::Property& Prop)
{
    const char* propName = Obj.getPropertyName(&Prop);
    if (!propName) {
        return;
    }
    admitObserverDelivery(
        [this, &Obj, propName] { invokePropertySlot(pyBeforeChangeObject.py, Obj, propName); },
        "BeforeChangeObject",
        makePropertyPayloadJson(Obj, propName));
}

void DocumentObserverPython::slotChangedObject(
    const Gui::ViewProvider& Obj,
    const App::Property& Prop)
{
    const char* propName = Obj.getPropertyName(&Prop);
    if (!propName) {
        return;
    }
    admitObserverDelivery(
        [this, &Obj, propName] { invokePropertySlot(pyChangedObject.py, Obj, propName); },
        "ChangedObject",
        makePropertyPayloadJson(Obj, propName));
}

void DocumentObserverPython::slotInEdit(const Gui::ViewProviderDocumentObject& Obj)
{
    admitObserverDelivery(
        [this, &Obj] { invokeEditSlot(pyInEdit.py, Obj); },
        "InEdit",
        makeObjectPayloadJson(Obj));
}

void DocumentObserverPython::slotResetEdit(const Gui::ViewProviderDocumentObject& Obj)
{
    admitObserverDelivery(
        [this, &Obj] { invokeEditSlot(pyResetEdit.py, Obj); },
        "ResetEdit",
        makeObjectPayloadJson(Obj));
}
