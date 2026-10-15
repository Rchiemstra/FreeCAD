// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Document.h"
#include "DocumentObject.h"

namespace App::Internal
{
// Owner-thread apply capture, independent of undo records and observer delivery.
// Record every request, including repeated touches and already-dirty cells.
using NativeRecomputeRequests = std::set<const DocumentObject*>;

class AppExport NativeRecomputeRequestCapture
{
public:
    NativeRecomputeRequestCapture(Document& document, NativeRecomputeRequests& requests);
    ~NativeRecomputeRequestCapture();
    NativeRecomputeRequestCapture(const NativeRecomputeRequestCapture&) = delete;
    NativeRecomputeRequestCapture& operator=(const NativeRecomputeRequestCapture&) = delete;

    static void record(const DocumentObject& object);

private:
    NativeRecomputeRequestCapture* const previous;
    Document& document;
    NativeRecomputeRequests& requests;
};

using NativeRecomputeErrors = std::map<const DocumentObject*, std::string>;

inline NativeRecomputeErrors captureNativeRecomputeErrors(const Document& document)
{
    NativeRecomputeErrors errors;
    for (const auto* object : document.getObjects()) {
        if (object->isError() && !object->getPropertyByName("Proxy")) {
            if (const char* error = document.getErrorDescription(object); error && *error) {
                errors.emplace(object, error);
            }
        }
    }
    return errors;
}

// Failed native features deliberately remain dirty so recompute retries them.
// That retry state is a terminal modelling result, not outstanding execution.
// Compatibility repair may retain the same error and blocked downstream work;
// unrelated dirty work, foreign work and Python Proxy failures still refuse.
inline bool pendingRecomputeMatchesNativeErrors(const Document& document,
                                                const NativeRecomputeErrors& errors,
                                                const NativeRecomputeRequests* fresh = nullptr)
{
    std::set<const DocumentObject*> visited;
    std::function<bool(const DocumentObject*)> explained = [&](const DocumentObject* object) {
        if (!object || object->getDocument() != &document
            || object->getPropertyByName("Proxy") || (fresh && fresh->contains(object))
            || !visited.insert(object).second) {
            return false;
        }
        if (const auto found = errors.find(object); found != errors.end() && object->isError()) {
            const char* error = document.getErrorDescription(object);
            if (error && found->second == error) {
                return true;
            }
        }
        for (const auto* source : object->getOutList()) {
            if (explained(source)) {
                return true;
            }
        }
        return false;
    };
    std::set<const DocumentObject*> closure;
    std::vector<const DocumentObject*> pending;
    for (auto* object : document.getObjects()) {
        pending.push_back(object);
    }
    while (!pending.empty()) {
        const auto* object = pending.back();
        pending.pop_back();
        if (!object || !closure.insert(object).second) {
            continue;
        }
        if (object->isTouched() || object->mustExecute()) {
            visited.clear();
            if (!explained(object)) {
                return false;
            }
        }
        for (const auto* source : object->getOutList()) {
            pending.push_back(source);
        }
    }
    return true;
}
}
