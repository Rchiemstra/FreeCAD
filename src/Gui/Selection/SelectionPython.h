// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <App/MainThreadSignal.h>
#include <Base/Interpreter.h>

namespace Gui
{
inline bool requirePythonSelectionMainThread(const char* api)
{
    if (App::MainThreadSignalConfig::isMainThread()) {
        return true;
    }
    // Selection changes notify view providers and widgets synchronously. Reject
    // before publishing any selection: an owner-thread callback may roll back
    // and delete its objects when it receives this error. Throwing from notify()
    // instead leaves selected pointers to those deleted objects, and unwinds
    // through CPython frames without translating the C++ exception.
    PyErr_Format(PyExc_RuntimeError, "GUI API '%s' may only be used from the main thread", api);
    return false;
}
}  // namespace Gui

