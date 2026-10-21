// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>
#include <thread>

namespace App
{
class Document;

/** Admission boundary for a temporary document state.
 *
 * Ordinary property, structure and history mutations are refused on every
 * thread until the scope ends. Only short synchronous operations on the scope's
 * calling thread may borrow Allow; they must never deliver events or call user
 * progress callbacks. This also blocks native history calls dispatched from
 * the GUI to a document execution lane.
 */
class AppExport ScopedDocumentMutation
{
public:
    explicit ScopedDocumentMutation(const Document& document);
    ~ScopedDocumentMutation();
    ScopedDocumentMutation(const ScopedDocumentMutation&) = delete;
    ScopedDocumentMutation& operator=(const ScopedDocumentMutation&) = delete;

    static void check(const Document& document);

    class AppExport Allow
    {
    public:
        explicit Allow(const Document& document);
        ~Allow();
        Allow(const Allow&) = delete;
        Allow& operator=(const Allow&) = delete;

    private:
        ScopedDocumentMutation* scope;
        bool previous;
    };

private:
    const Document& document;
    const std::thread::id owner;
    bool allowed = false;
};
}  // namespace App
