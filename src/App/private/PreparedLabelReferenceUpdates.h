// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>
#include <memory>

namespace App
{
class Document;
class DocumentCommitCoordinator;
class Property;
class PropertyString;
namespace Internal
{
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
    static bool apply(Document& source, Property& property, const Property& value);

private:
    friend class App::DocumentCommitCoordinator;
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
