// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include <string>
#include <string_view>

namespace App
{

class Document;

/** Result of checking a prepared operation's postcondition. */
struct AppExport CollaborativePostconditionResult
{
    bool satisfied {false};
    std::string message;
};

/**
 * Immutable interface for one FreeCAD-owned prepared operation.
 *
 * The operation is already a final mutation batch. Dependency derivation and
 * publication effects are frozen in PreparedEdit by the App-owned preparation
 * boundary, rather than self-attested through this interface. Implementations
 * may access a document only during these short-lived calls and must not retain
 * Document, DocumentObject, or other live-model pointers.
 */
class AppExport CollaborativeOperation
{
public:
    virtual ~CollaborativeOperation();

    [[nodiscard]] virtual std::string_view typeId() const noexcept = 0;
    virtual void apply(Document& document) const = 0;
    [[nodiscard]] virtual CollaborativePostconditionResult
    checkPostcondition(const Document& document) const = 0;

    /** Detached recompute adapters may commit an authoritative failure state. */
    /** Whether this feature counts towards the recompute's reported total.
     *
     * The legacy loop counted an object when its mustRecompute() was true --
     * not when execute() ran. The two differ: a plain App::Link is settled
     * without executing yet still counts, while an object whose only change
     * was a Prop_NoRecompute property never sets Enforce and counts for
     * nothing. Operations that execute always count; the bookkeeping settle
     * answers with the state it found.
     */
    [[nodiscard]] virtual bool recomputeCountedFeature() const noexcept
    {
        return true;
    }

    [[nodiscard]] virtual bool recomputeOutcomeSucceeded() const noexcept
    {
        return true;
    }
    [[nodiscard]] virtual std::string_view recomputeOutcomeDiagnostic() const noexcept
    {
        return {};
    }

protected:
    CollaborativeOperation() = default;
    CollaborativeOperation(const CollaborativeOperation&) = delete;
    CollaborativeOperation(CollaborativeOperation&&) = delete;
    CollaborativeOperation& operator=(const CollaborativeOperation&) = delete;
    CollaborativeOperation& operator=(CollaborativeOperation&&) = delete;
};

}  // namespace App
