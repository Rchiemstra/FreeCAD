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


#pragma once

#include <FCGlobal.h>
#include <Gui/GuiPythonGate.h>
#include <fastsignals/signal.h>
#include <CXX/Objects.hxx>
#include <string>
#include <vector>

namespace App
{
class Property;
}

namespace Gui
{

class Document;
class ViewProvider;
class ViewProviderDocumentObject;

/**
 * The DocumentObserverPython class is used to notify registered Python instances
 * whenever something happens to a document, like creation, destruction, adding or
 * removing viewproviders or when viewprovider property changes. This is the equivalent to the app
 * python document observer
 */
class GuiExport DocumentObserverPython
{

public:
    /// Constructor
    DocumentObserverPython(const Py::Object& obj);
    virtual ~DocumentObserverPython();

    static void addObserver(const Py::Object& obj);
    static void removeObserver(const Py::Object& obj);
    static void installValueEventHandler();

private:
    // Narrow thread-local seam: tests pause a real deferred signal invocation
    // before it copies the identity owned by the connected callable.
    friend class DocumentObserverPythonTestAccess;
    static void setBeforeDeferredIdentityCopyForTest(void (*hook)());
    void deliverValueEvent(const GuiPythonObserverValueEvent& event);
    /** Checks if a new document was created */
    static void slotCreatedDocument(
        DocumentObserverPython* observer,
        const std::string& observerToken,
        const Gui::Document& Doc);
    /** Checks if the given document is about to be closed */
    static void slotDeletedDocument(
        DocumentObserverPython* observer,
        const std::string& observerToken,
        const Gui::Document& Doc);
    /** Checks if the given document is relabeled */
    static void slotRelabelDocument(
        DocumentObserverPython* observer,
        const std::string& observerToken,
        const Gui::Document& Doc);
    /** Checks if the given document is renamed */
    static void slotRenameDocument(
        DocumentObserverPython* observer,
        const std::string& observerToken,
        const Gui::Document& Doc);
    /** Checks if the given document is activated */
    static void slotActivateDocument(
        DocumentObserverPython* observer,
        const std::string& observerToken,
        const Gui::Document& Doc);
    /** Checks if a new object was added. */
    static void slotCreatedObject(
        DocumentObserverPython* observer,
        const std::string& observerToken,
        const Gui::ViewProvider& Obj);
    /** Checks if the given object is about to be removed. */
    static void slotDeletedObject(
        DocumentObserverPython* observer,
        const std::string& observerToken,
        const Gui::ViewProvider& Obj);
    /** The property of an observed object has changed */
    static void slotBeforeChangeObject(
        DocumentObserverPython* observer,
        const std::string& observerToken,
        const Gui::ViewProvider& Obj, const App::Property& Prop);
    /** The property of an observed object has changed */
    static void slotChangedObject(
        DocumentObserverPython* observer,
        const std::string& observerToken,
        const Gui::ViewProvider& Obj, const App::Property& Prop);
    /** The object was set into edit mode */
    static void slotInEdit(
        DocumentObserverPython* observer,
        const std::string& observerToken,
        const Gui::ViewProviderDocumentObject& Obj);
    /** The has left edit mode */
    static void slotResetEdit(
        DocumentObserverPython* observer,
        const std::string& observerToken,
        const Gui::ViewProviderDocumentObject& Obj);

private:
    Py::Object inst;
    const std::string observerToken;
    static std::vector<DocumentObserverPython*> _instances;

    using Connection = struct PythonObject
    {
        fastsignals::scoped_connection slot;
        Py::Object py;
        PyObject* ptr()
        {
            return py.ptr();
        }
    };

    Connection pyCreatedDocument;
    Connection pyDeletedDocument;
    Connection pyRelabelDocument;
    Connection pyRenameDocument;
    Connection pyActivateDocument;
    Connection pyCreatedObject;
    Connection pyDeletedObject;
    Connection pyBeforeChangeObject;
    Connection pyChangedObject;
    Connection pyInEdit;
    Connection pyResetEdit;
};

}  // namespace Gui
