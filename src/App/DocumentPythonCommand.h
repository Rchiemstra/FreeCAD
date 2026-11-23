// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentCommandHandle.h"

#include <FCGlobal.h>

struct _object;
using PyObject = _object;

namespace App
{

AppExport PyObject* makeDocumentCommandHandlePy(const DocumentCommandHandle& handle);

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
namespace Internal {
enum class CompatibilityReceiptFault {
    None, CompatibilityFutureAllocation, CompatibilityStateAllocation,
    CompatibilityCapsuleAllocation, CompatibilityCallableAllocation,
    CompatibilityDictionaryAllocation, CompatibilityResultConversion
};
using CompatibilityWaitHook = void (*)() noexcept;
AppExport void setCompatibilityWaitHookForTesting(CompatibilityWaitHook hook) noexcept;
AppExport void setCompatibilityReceiptFaultForTesting(CompatibilityReceiptFault fault) noexcept;
AppExport bool consumeCompatibilityReceiptFaultForTesting(CompatibilityReceiptFault fault) noexcept;
}
#endif

}  // namespace App
