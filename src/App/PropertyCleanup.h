// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

namespace App
{
struct PropertyCleaner;

// Only for a complete managed job whose retired properties cannot escape.
// Ordinary PropertyCleaner guards retain their existing self-skip semantics.
class AppExport PropertyCleanupBoundary final
{
public:
    PropertyCleanupBoundary();
    ~PropertyCleanupBoundary() noexcept;
    PropertyCleanupBoundary(const PropertyCleanupBoundary&) = delete;
    PropertyCleanupBoundary& operator=(const PropertyCleanupBoundary&) = delete;

private:
    friend struct PropertyCleaner;
    friend bool propertyCleanupIsQuiescent() noexcept;
    PropertyCleanupBoundary* previous;
    static thread_local PropertyCleanupBoundary* current;
};

// Call as the first application work on a worker. Only workers whose lifetime
// is owned and joined by Application shutdown may pass true.
AppExport void initializeManagedPropertyCleanup(bool applicationOwned);
[[nodiscard]] AppExport bool propertyCleanupIsQuiescent() noexcept;
// Refuses before changing runtime state while any registered foreign thread
// remains alive. No callback runs while the registry lock is held.
[[nodiscard]] AppExport bool preparePropertyCleanupFinalization();
AppExport void waitForPropertyCleanupDrains();
AppExport void cancelPropertyCleanupFinalization() noexcept;
[[nodiscard]] AppExport bool finishPropertyCleanupFinalization();
AppExport void stopPropertyCleanupReaper() noexcept;
// Only at an actual quiescent boundary, never from a property callback.
AppExport void drainRetiredPropertiesAtQuiescence() noexcept;
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
namespace Internal {
AppExport void setCleanupDrainWaitHookForTesting(void (*hook)()) noexcept;
AppExport void failNextFinalizationAfterSealForTesting() noexcept;
AppExport void setPostInitialFinalizationCloseHookForTesting(void (*hook)()) noexcept;
}
#endif
}  // namespace App
