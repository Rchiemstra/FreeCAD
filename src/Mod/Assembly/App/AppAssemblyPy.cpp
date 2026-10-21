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
#include <Base/Tools.h>

#include "InterferenceScanPy.h"


namespace Assembly
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("AssemblyApp")
    {
        add_keyword_method("scanSelectedPair", &Module::scanSelectedPair,
            "scanSelectedPair(host, first, second, measureDistances=False, "
            "maxFacePairCandidates=10000, cancel=None) -> dict\n"
            "Snapshot and scan two component occurrences using the interference checker.\n"
            "Call on the document thread. Endpoints are objects or (object, subname).\n"
            "Distances/points are in mm/world coordinates. Results retain witness geometry;\n"
            "solidDetection preserves the whole-solid result before face-rule aggregation.\n"
            "measureDistances evaluates distant clear pairs too. complete describes only\n"
            "this static selected scope. cancel is a newInterferenceScanCancellation token.");
        add_varargs_method("newInterferenceScanCancellation", &Module::newCancellation,
            "Create a thread-safe cooperative scan cancellation token.");
        add_varargs_method("cancelInterferenceScan", &Module::cancelScan,
            "Cancel scans using the supplied token; cancellation is cooperative.");
        add_varargs_method("_withMotionSweepState", &Module::withMotionSweepState,
            "Internal synchronous document-thread scope for the motion controller.");
        initialize("This module is the Assembly module.");  // register with Python
    }

private:
    Py::Object withMotionSweepState(const Py::Tuple& args)
    {
        return withMotionSweepStatePy(args);
    }
    Py::Object scanSelectedPair(const Py::Tuple& args, const Py::Dict& keywords)
    {
        return scanSelectedPairPy(args, keywords);
    }
    Py::Object newCancellation(const Py::Tuple& args)
    {
        return newInterferenceScanCancellationPy(args);
    }
    Py::Object cancelScan(const Py::Tuple& args)
    {
        return cancelInterferenceScanPy(args);
    }
};

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}

}  // namespace Assembly
