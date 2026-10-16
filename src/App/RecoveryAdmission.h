// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <FCGlobal.h>

namespace App {
/** FIFO idle reservation for recovery IO and its GUI completion.
 * Register before dispatch. Pending reservations make new mutation admission
 * yield; nested work of an already admitted command remains allowed.
 * The worker may wait without the GIL; GUI callers only use tryAcquire().
 */
class AppExport RecoveryAdmission final
{
public:
    RecoveryAdmission();
    ~RecoveryAdmission() noexcept;
    RecoveryAdmission(const RecoveryAdmission&) = delete;
    RecoveryAdmission& operator=(const RecoveryAdmission&) = delete;
    bool tryAcquire();
    void acquire();
private:
    bool acquired {false};
};
}
