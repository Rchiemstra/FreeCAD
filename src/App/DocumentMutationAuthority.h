// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "MutationCapability.h"
#include "MutationKind.h"
#include <FCGlobal.h>

#include <fastsignals/signal.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace App
{

class Document;
struct MutationContext;

/**
 * Optional policy callbacks registered by FreeCADMCP (or tests).
 * Core still issues capabilities; the provider does not replace enforcement.
 */
class AppExport ExternalMutationAuthorityProvider
{
public:
    virtual ~ExternalMutationAuthorityProvider() = default;

    virtual bool mayIssue(Document& document, MutationKind kind, std::uint64_t generation) = 0;
    virtual void onDenied(Document& document,
                          const MutationContext& context,
                          MutationDecision decision) = 0;
    virtual void onTakeover(Document& document, std::uint64_t newGeneration) = 0;
};

/**
 * Abstract guard invoked before mutations are applied.
 */
class AppExport DocumentMutationGuard
{
public:
    virtual ~DocumentMutationGuard() = default;
    virtual MutationDecision authorize(Document& document,
                                       MutationKind kind,
                                       const MutationContext& context) = 0;
};

/**
 * Single source of truth for lease-gated document mutation authority.
 * Documents without MCP ownership remain unrestricted.
 */
class AppExport DocumentMutationAuthority: public DocumentMutationGuard
{
public:
    static DocumentMutationAuthority& instance();

    void setProvider(std::shared_ptr<ExternalMutationAuthorityProvider> provider);
    std::shared_ptr<ExternalMutationAuthorityProvider> provider() const;

    void setOwner(Document& document,
                  MutationOwner owner,
                  std::uint64_t fencingGeneration = 0,
                  const std::string& providerId = {});
    void clearOwner(const Document& document);
    MutationOwner owner(const Document& document) const;
    std::uint64_t fencingGeneration(const Document& document) const;
    std::string providerId(const Document& document) const;
    bool isRestricted(const Document& document) const;

    /** Issue a short-lived capability after lease auth (in-process only). */
    MutationCapabilityScope openCapability(Document& document,
                                           MutationKindMask kinds,
                                           std::uint64_t fencingGeneration,
                                           MutationOrigin origin = MutationOrigin::Mcp);

    /** Bump fencing generation, revoke MCP capabilities, switch to UserOwned. */
    std::uint64_t takeover(Document& document);

    MutationDecision authorize(Document& document,
                               MutationKind kind,
                               const MutationContext& context) override;

    /** Authorize and throw Base::MutationDeniedException on deny. */
    void enforce(Document& document, MutationKind kind, MutationContext context);

    void forgetDocument(const Document& document);

    using DeniedSignal =
        fastsignals::signal<void(Document&, const MutationContext&, MutationDecision)>;
    using TakeoverSignal = fastsignals::signal<void(Document&, std::uint64_t)>;
    DeniedSignal signalDenied;
    TakeoverSignal signalTakeover;

private:
    DocumentMutationAuthority() = default;

    struct DocumentState
    {
        MutationOwner owner {MutationOwner::Unrestricted};
        std::uint64_t fencingGeneration {0};
        std::string providerId;
        bool recoveryMode {false};
    };

    DocumentState* stateFor(const Document& document);
    const DocumentState* stateFor(const Document& document) const;
    MutationDecision authorizeLocked(Document& document,
                                     MutationKind kind,
                                     const MutationContext& context,
                                     const DocumentState& state) const;

    mutable std::mutex _mutex;
    std::unordered_map<const Document*, DocumentState> _states;
    std::shared_ptr<ExternalMutationAuthorityProvider> _provider;
    std::uint64_t _nextCapabilityId {1};
};

/** Resolve owning document from a property container, if any. */
AppExport Document* documentFromPropertyContainer(const class PropertyContainer* container);

/** Build a minimal context and enforce for the given document/kind. */
AppExport void enforceDocumentMutation(Document* document,
                                       MutationKind kind,
                                       MutationOrigin origin = MutationOrigin::Cpp,
                                       const char* objectName = nullptr,
                                       const char* propertyName = nullptr);

}  // namespace App
