// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ScopedDocumentMutation.h"

#include <Base/Exception.h>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace
{
std::atomic<bool> admissionActive {false};
std::mutex admissionMutex;
std::unordered_map<const App::Document*, App::ScopedDocumentMutation*> admissions;
}

namespace App
{
ScopedDocumentMutation::ScopedDocumentMutation(const Document& document)
    : document(document)
    , owner(std::this_thread::get_id())
{
    std::lock_guard lock(admissionMutex);
    if (!admissions.emplace(&document, this).second) {
        throw Base::RuntimeError("A temporary motion scope is already active in this document");
    }
    admissionActive.store(true, std::memory_order_release);
}

ScopedDocumentMutation::~ScopedDocumentMutation()
{
    std::lock_guard lock(admissionMutex);
    admissions.erase(&document);
    admissionActive.store(!admissions.empty(), std::memory_order_release);
}

void ScopedDocumentMutation::check(const Document& document)
{
    if (!admissionActive.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard lock(admissionMutex);
    const auto it = admissions.find(&document);
    if (it != admissions.end()
        && (!it->second->allowed || it->second->owner != std::this_thread::get_id())) {
        throw Base::RuntimeError(
            "Document edits, removal, transactions, undo and redo are unavailable during a temporary motion sweep or replay");
    }
}

ScopedDocumentMutation::Allow::Allow(const Document& document)
{
    std::lock_guard lock(admissionMutex);
    const auto it = admissions.find(&document);
    if (it == admissions.end() || it->second->owner != std::this_thread::get_id()) {
        throw Base::RuntimeError(
            "Controlled motion writes require an active temporary motion scope on this thread");
    }
    scope = it->second;
    previous = scope->allowed;
    scope->allowed = true;
}

ScopedDocumentMutation::Allow::~Allow()
{
    std::lock_guard lock(admissionMutex);
    scope->allowed = previous;
}
}  // namespace App
