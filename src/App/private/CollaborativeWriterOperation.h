// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "../CollaborativeOperation.h"

namespace App
{
class Document;
class CollaborationWriterCapability;

namespace Internal
{
// Optional interface for trusted operations that explicitly borrow the writer.
// The capability is valid only during the call and must not reach observers.
class AppExport CollaborativeWriterOperation
{
public:
    virtual ~CollaborativeWriterOperation();
    virtual void applyWithWriterCapability(
        Document&, const CollaborationWriterCapability&) const = 0;
};

inline void applyCollaborativeOperation(const CollaborativeOperation& operation,
                                        Document& document,
                                        const CollaborationWriterCapability* capability)
{
    const auto* scoped = capability
        ? dynamic_cast<const CollaborativeWriterOperation*>(&operation)
        : nullptr;
    if (scoped) {
        scoped->applyWithWriterCapability(document, *capability);
    }
    else {
        operation.apply(document);
    }
}
}  // namespace Internal
}  // namespace App
