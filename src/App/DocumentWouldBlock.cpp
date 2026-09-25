// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentWouldBlock.h"

#include <format>

namespace App
{

DocumentWouldBlock::DocumentWouldBlock(const char* message)
    : Base::RuntimeError(message ? message : "document operation would block the GUI thread")
{}

DocumentWouldBlock::DocumentWouldBlock(const std::string& message)
    : Base::RuntimeError(message.c_str())
{}

bool DocumentWouldBlock::isGuiThread() noexcept
{
    return MainThreadSignalConfig::hasHooks() && MainThreadSignalConfig::isMainThread();
}

void DocumentWouldBlock::throwIfGuiThread(const char* syncApi, const char* asyncApi)
{
    if (!isGuiThread()) {
        return;
    }
    throw DocumentWouldBlock(
        std::format("{} would block the GUI thread; use {} instead", syncApi, asyncApi));
}

}  // namespace App
