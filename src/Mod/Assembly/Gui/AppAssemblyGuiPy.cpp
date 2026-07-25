// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2023 Ondsel <development@ondsel.com>                     *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/


#include <Base/Interpreter.h>

#include "ViewProviderReviewNote.h"


namespace AssemblyGui
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("AssemblyGui")
    {
        add_varargs_method(
            "resetReviewNoteTestHooks",
            &Module::resetReviewNoteTestHooks,
            "resetReviewNoteTestHooks() — clear ViewProviderReviewNote test injectors"
        );
        add_varargs_method(
            "setReviewNoteTestInjectThrowAfterCoords",
            &Module::setReviewNoteTestInjectThrowAfterCoords,
            "setReviewNoteTestInjectThrowAfterCoords(count)"
        );
        add_varargs_method(
            "setReviewNoteTestInjectNestedCamera",
            &Module::setReviewNoteTestInjectNestedCamera,
            "setReviewNoteTestInjectNestedCamera(count)"
        );
        add_varargs_method(
            "reviewNoteTestNestedDirtyMarkedCount",
            &Module::reviewNoteTestNestedDirtyMarkedCount,
            "reviewNoteTestNestedDirtyMarkedCount() -> int"
        );
        add_varargs_method(
            "reviewNoteTestApplyExceptionsCaughtCount",
            &Module::reviewNoteTestApplyExceptionsCaughtCount,
            "reviewNoteTestApplyExceptionsCaughtCount() -> int"
        );
        initialize("This module is the Assembly module.");  // register with Python
    }

private:
    Py::Object resetReviewNoteTestHooks(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        ViewProviderReviewNote::resetTestHooks();
        return Py::None();
    }

    Py::Object setReviewNoteTestInjectThrowAfterCoords(const Py::Tuple& args)
    {
        int count = 0;
        if (!PyArg_ParseTuple(args.ptr(), "i", &count)) {
            throw Py::Exception();
        }
        ViewProviderReviewNote::setTestInjectThrowAfterCoords(count);
        return Py::None();
    }

    Py::Object setReviewNoteTestInjectNestedCamera(const Py::Tuple& args)
    {
        int count = 0;
        if (!PyArg_ParseTuple(args.ptr(), "i", &count)) {
            throw Py::Exception();
        }
        ViewProviderReviewNote::setTestInjectNestedCamera(count);
        return Py::None();
    }

    Py::Object reviewNoteTestNestedDirtyMarkedCount(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        return Py::Long(ViewProviderReviewNote::testNestedDirtyMarkedCount());
    }

    Py::Object reviewNoteTestApplyExceptionsCaughtCount(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        return Py::Long(ViewProviderReviewNote::testApplyExceptionsCaughtCount());
    }
};

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}

}  // namespace AssemblyGui
