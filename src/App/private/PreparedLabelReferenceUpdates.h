// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>
#include <memory>
#include <list>
#include <atomic>

namespace App
{
class Document;
class DocumentCommitCoordinator;
class Property;
class PropertyString;
class Transaction;
namespace Internal
{
class LabelReferenceUpdatesTestAccess;
// Native Label reference effects join the prepared operation's rollback and
// publication boundary. Only the coordinator can establish this context.
class AppExport PreparedLabelReferenceUpdates final
{
public:
    ~PreparedLabelReferenceUpdates();
    bool hasLocalUpdates() const noexcept;
    void prepare();
    void commit();
    bool rollback() noexcept;
    void finish(bool committed) noexcept;
    static void admit(Document& source, Property& property);
    static void activate(Document& source);
    static void preserveRedo(Document& document);
    static void noteMutation(Document& document);
    static bool hasUnrestoredChanges(const Document& document) noexcept;
    static bool presentationRefreshOnCallingThread() noexcept;
    static void invalidateRedos(Document& document);
    static void clearRedos(Document& document,
                           std::list<Transaction*>* deferred = nullptr) noexcept;
    static void failed(Document& source) noexcept;
    bool failed() const noexcept;
    class ReplayMode final
    {
    public:
        explicit ReplayMode(bool grouped) noexcept;
        ~ReplayMode();
        ReplayMode(const ReplayMode&) = delete;
        ReplayMode& operator=(const ReplayMode&) = delete;
    private:
        bool previous;
    };
    // Shared native replay entry for the collaboration coordinator.
    static bool replay(Document& source, bool undo, int transactionId = 0,
                       bool grouped = true);
    static bool apply(Document& source, Property& property, const Property& value);

private:
    friend class App::DocumentCommitCoordinator;
    friend class App::Document;
    friend class LabelReferenceUpdatesTestAccess;
    static std::atomic<void (*)(Document&)> finalizationTestHook;
    static std::atomic<void (*)()> rewriteTestHook;
    PreparedLabelReferenceUpdates(Document& source, bool retainUndoHistory);
    struct State;
    std::unique_ptr<State> state;
};

// A private, synchronous native effect may temporarily target an admitted
// dependent. The surrounding prepared target and read-only fence stay intact.
class LabelReferenceMutationTarget final
{
public:
    ~LabelReferenceMutationTarget();
private:
    friend class PreparedLabelReferenceUpdates;
    LabelReferenceMutationTarget(Document& source, Document& dependent);
    Document* previous;
};
}
}
