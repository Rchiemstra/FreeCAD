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

#ifdef ASSEMBLY_ENABLE_TEST_HOOKS
# include "ViewProviderReviewNoteTestHarness.h"
#endif


namespace AssemblyGui
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("AssemblyGui")
    {
#ifdef ASSEMBLY_ENABLE_TEST_HOOKS
        // Private test harness bindings — only compiled for developer/test builds.
        // Production AssemblyGui does not expose deliberate fault-injection APIs.
        add_varargs_method(
            "resetReviewNoteTestHooks",
            &Module::resetReviewNoteTestHooks,
            "resetReviewNoteTestHooks() — clear ViewProviderReviewNote test injectors"
        );
        add_varargs_method(
            "setReviewNoteTestInjectThrowAfterCoords",
            &Module::setReviewNoteTestInjectThrowAfterCoords,
            "setReviewNoteTestInjectThrowAfterCoords(count) — test harness only"
        );
        add_varargs_method(
            "setReviewNoteTestInjectNestedCamera",
            &Module::setReviewNoteTestInjectNestedCamera,
            "setReviewNoteTestInjectNestedCamera(count) — test harness only"
        );
        add_varargs_method(
            "reviewNoteTestNestedDirtyMarkedCount",
            &Module::reviewNoteTestNestedDirtyMarkedCount,
            "reviewNoteTestNestedDirtyMarkedCount() -> int — test harness only"
        );
        add_varargs_method(
            "reviewNoteTestApplyExceptionsCaughtCount",
            &Module::reviewNoteTestApplyExceptionsCaughtCount,
            "reviewNoteTestApplyExceptionsCaughtCount() -> int — test harness only"
        );
#endif
        initialize("This module is the Assembly module.");  // register with Python
    }

private:
#ifdef ASSEMBLY_ENABLE_TEST_HOOKS
    Py::Object resetReviewNoteTestHooks(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        ReviewNoteTestHarness::reset();
        return Py::None();
    }

    Py::Object setReviewNoteTestInjectThrowAfterCoords(const Py::Tuple& args)
    {
        int count = 0;
        if (!PyArg_ParseTuple(args.ptr(), "i", &count)) {
            throw Py::Exception();
        }
        ReviewNoteTestHarness::setInjectThrowAfterCoords(count);
        return Py::None();
    }

    Py::Object setReviewNoteTestInjectNestedCamera(const Py::Tuple& args)
    {
        int count = 0;
        if (!PyArg_ParseTuple(args.ptr(), "i", &count)) {
            throw Py::Exception();
        }
        ReviewNoteTestHarness::setInjectNestedCamera(count);
        return Py::None();
    }

    Py::Object reviewNoteTestNestedDirtyMarkedCount(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        return Py::Long(ReviewNoteTestHarness::nestedDirtyMarkedCount());
    }

    Py::Object reviewNoteTestApplyExceptionsCaughtCount(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        return Py::Long(ReviewNoteTestHarness::applyExceptionsCaughtCount());
    }
#endif
};

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}

}  // namespace AssemblyGui
