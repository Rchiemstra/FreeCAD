// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PresentationCaptureRegistry.h"

#include <App/DocumentObject.h>

#include <mutex>
#include <vector>

namespace Gui
{

namespace
{

struct CaptureEntry
{
    Base::Type objectType;
    DocumentObjectPresentationCaptureFn capture {};
};

std::mutex& registryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<CaptureEntry>& registryEntries()
{
    static std::vector<CaptureEntry> entries;
    return entries;
}

int typeDepth(Base::Type type)
{
    int depth = 0;
    while (!type.isBad()) {
        type = type.getParent();
        ++depth;
    }
    return depth;
}

const CaptureEntry* findBestCapture(const App::DocumentObject& object)
{
    const CaptureEntry* best = nullptr;
    int bestDepth = -1;
    for (const CaptureEntry& entry : registryEntries()) {
        if (!object.isDerivedFrom(entry.objectType)) {
            continue;
        }
        const int depth = typeDepth(entry.objectType);
        if (depth > bestDepth) {
            bestDepth = depth;
            best = &entry;
        }
    }
    return best;
}

}  // namespace

void PresentationCaptureRegistry::registerCapture(Base::Type objectType,
                                                    DocumentObjectPresentationCaptureFn capture)
{
    if (objectType.isBad() || capture == nullptr) {
        return;
    }
    std::lock_guard lock(registryMutex());
    registryEntries().push_back({objectType, capture});
}

bool PresentationCaptureRegistry::hasCapture(const App::DocumentObject& object)
{
    std::lock_guard lock(registryMutex());
    return findBestCapture(object) != nullptr;
}

bool PresentationCaptureRegistry::tryCapture(const App::DocumentObject& object,
                                             const std::string& stableObjectIdentity,
                                             PresentationRenderBuffer& buffer)
{
    std::lock_guard lock(registryMutex());
    const CaptureEntry* entry = findBestCapture(object);
    if (!entry || entry->capture == nullptr) {
        return false;
    }
    return entry->capture(object, stableObjectIdentity, buffer);
}

DocumentPresentationCaptureRegistrar::DocumentPresentationCaptureRegistrar(
    Base::Type objectType,
    DocumentObjectPresentationCaptureFn capture)
{
    PresentationCaptureRegistry::registerCapture(objectType, capture);
}

}  // namespace Gui
