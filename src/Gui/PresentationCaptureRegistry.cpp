// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PresentationCaptureRegistry.h"

#include <App/DocumentObject.h>

#include <mutex>
#include <string>
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

struct PendingCaptureEntry
{
    std::string objectTypeName;
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

std::vector<PendingCaptureEntry>& pendingCaptureEntries()
{
    static std::vector<PendingCaptureEntry> entries;
    return entries;
}

void flushPendingCaptureEntries()
{
    for (auto it = pendingCaptureEntries().begin(); it != pendingCaptureEntries().end();) {
        if (it->capture == nullptr || it->objectTypeName.empty()) {
            it = pendingCaptureEntries().erase(it);
            continue;
        }
        const Base::Type objectType = Base::Type::fromName(it->objectTypeName);
        if (objectType.isBad()) {
            ++it;
            continue;
        }
        registryEntries().push_back({objectType, it->capture});
        it = pendingCaptureEntries().erase(it);
    }
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

void PresentationCaptureRegistry::registerCapture(const char* objectTypeName,
                                                    DocumentObjectPresentationCaptureFn capture)
{
    if (objectTypeName == nullptr || objectTypeName[0] == '\0' || capture == nullptr) {
        return;
    }
    std::lock_guard lock(registryMutex());
    const Base::Type objectType = Base::Type::fromName(objectTypeName);
    if (objectType.isBad()) {
        pendingCaptureEntries().push_back({objectTypeName, capture});
        return;
    }
    registryEntries().push_back({objectType, capture});
    flushPendingCaptureEntries();
}

void PresentationCaptureRegistry::registerCapture(Base::Type objectType,
                                                    DocumentObjectPresentationCaptureFn capture)
{
    if (capture == nullptr) {
        return;
    }
    if (objectType.isBad()) {
        return;
    }
    std::lock_guard lock(registryMutex());
    registryEntries().push_back({objectType, capture});
    flushPendingCaptureEntries();
}

bool PresentationCaptureRegistry::hasCapture(const App::DocumentObject& object)
{
    std::lock_guard lock(registryMutex());
    flushPendingCaptureEntries();
    return findBestCapture(object) != nullptr;
}

bool PresentationCaptureRegistry::tryCapture(const App::DocumentObject& object,
                                             const std::string& stableObjectIdentity,
                                             PresentationRenderBuffer& buffer)
{
    DocumentObjectPresentationCaptureFn captureFn {};
    {
        std::lock_guard lock(registryMutex());
        flushPendingCaptureEntries();
        const CaptureEntry* entry = findBestCapture(object);
        if (!entry || entry->capture == nullptr) {
            return false;
        }
        captureFn = entry->capture;
    }
    return captureFn(object, stableObjectIdentity, buffer);
}

DocumentPresentationCaptureRegistrar::DocumentPresentationCaptureRegistrar(
    const char* objectTypeName,
    DocumentObjectPresentationCaptureFn capture)
{
    PresentationCaptureRegistry::registerCapture(objectTypeName, capture);
}

}  // namespace Gui
