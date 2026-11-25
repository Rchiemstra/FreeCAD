// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <App/MainThreadSignal.h>

// Test-only interface. The implementation is exported by FreeCADApp only
// when developer tests are enabled; the production class is unchanged.
namespace App::Test
{
struct MainThreadSignalHooks
{
    MainThreadSignalConfig::IsMainThreadFn isMainThread;
    MainThreadSignalConfig::InvokeFn invoke;
    MainThreadSignalConfig::ServiceFn service;
};

// Install before test senders start; restore only after they have stopped.
[[nodiscard]] AppExport MainThreadSignalHooks
exchangeMainThreadSignalHooks(MainThreadSignalHooks hooks) noexcept;
}  // namespace App::Test
