// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <App/MainThreadSignal.h>

#include <Base/Exception.h>

#include <FCGlobal.h>

namespace App
{

/**
 * Raised when a synchronous document API is invoked on the GUI thread while the
 * execution lane owns model work. Callers should use async APIs instead.
 */
class AppExport DocumentWouldBlock : public Base::RuntimeError
{
public:
    explicit DocumentWouldBlock(const char* message = nullptr);
    explicit DocumentWouldBlock(const std::string& message);

    PyObject* getPyExceptionType() const override;

    [[nodiscard]] static bool isGuiThread() noexcept;
    [[nodiscard]] static void throwIfGuiThread(const char* syncApi, const char* asyncApi);
};

}  // namespace App
