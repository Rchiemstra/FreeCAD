// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 Joao Matos
// SPDX-FileNotice: Part of the FreeCAD project.

/******************************************************************************
 *                                                                            *
 *   FreeCAD is free software: you can redistribute it and/or modify          *
 *   it under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1            *
 *   of the License, or (at your option) any later version.                   *
 *                                                                            *
 *   FreeCAD is distributed in the hope that it will be useful,               *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty              *
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                  *
 *   See the GNU Lesser General Public License for more details.              *
 *                                                                            *
 *   You should have received a copy of the GNU Lesser General Public         *
 *   License along with FreeCAD. If not, see https://www.gnu.org/licenses     *
 *                                                                            *
 ******************************************************************************/

#include "PreCompiled.h"

#include "MainThreadSignal.h"

using namespace App;

namespace
{
// One definition, in one module. The hooks are installed by Gui or by a test
// and read by App, so a per-module copy would silently disconnect the two.
MainThreadSignalConfig::IsMainThreadFn gIsMainThread {nullptr};
MainThreadSignalConfig::InvokeFn gInvoke {nullptr};
}  // namespace

void MainThreadSignalConfig::setHooks(const IsMainThreadFn isMainThread, const InvokeFn invoke)
{
    gIsMainThread = isMainThread;
    gInvoke = invoke;
}

bool MainThreadSignalConfig::isMainThread()
{
    return gIsMainThread != nullptr ? gIsMainThread() : true;
}

bool MainThreadSignalConfig::hasHooks()
{
    return gIsMainThread != nullptr && gInvoke != nullptr;
}

void MainThreadSignalConfig::invoke(std::function<void()>&& fn, const bool blocking)
{
    if (gInvoke != nullptr) {
        gInvoke(std::move(fn), blocking);
        return;
    }
    fn();
}
