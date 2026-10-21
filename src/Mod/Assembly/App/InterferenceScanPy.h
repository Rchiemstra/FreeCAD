// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <CXX/Objects.hxx>
#include <Mod/Assembly/AssemblyGlobal.h>

namespace Assembly
{
struct InterferenceScanResult;

Py::Object scanSelectedPairPy(const Py::Tuple& args, const Py::Dict& keywords);
Py::Object newInterferenceScanCancellationPy(const Py::Tuple& args);
Py::Object cancelInterferenceScanPy(const Py::Tuple& args);

/** Owned immutable geometry snapshot, independent of the returned Python dictionaries. */
AssemblyExport const InterferenceScanResult& interferenceScanSnapshotFromPy(PyObject* capsule);
}
