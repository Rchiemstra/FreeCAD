// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentMutationAuthority.h"

#include "Document.h"
#include "DocumentObject.h"
#include "Property.h"
#include "PropertyContainer.h"

#include <Base/Exception.h>

#include <sstream>
#include <utility>

using namespace App;

DocumentMutationAuthority& DocumentMutationAuthority::instance()
{
    static DocumentMutationAuthority auth;
    return auth;
}

void DocumentMutationAuthority::setProvider(
    std::shared_ptr<ExternalMutationAuthorityProvider> provider)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _provider = std::move(provider);
}

std::shared_ptr<ExternalMutationAuthorityProvider> DocumentMutationAuthority::provider() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _provider;
}

DocumentMutationAuthority::DocumentState*
DocumentMutationAuthority::stateFor(const Document& document)
{
    auto it = _states.find(&document);
    if (it == _states.end()) {
        return nullptr;
    }
    return &it->second;
}

const DocumentMutationAuthority::DocumentState*
DocumentMutationAuthority::stateFor(const Document& document) const
{
    auto it = _states.find(&document);
    if (it == _states.end()) {
        return nullptr;
    }
    return &it->second;
}

void DocumentMutationAuthority::setOwner(Document& document,
                                        MutationOwner owner,
                                        std::uint64_t fencingGeneration,
                                        const std::string& providerId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto& state = _states[&document];
    state.owner = owner;
    if (fencingGeneration != 0) {
        state.fencingGeneration = fencingGeneration;
    }
    else if (owner == MutationOwner::McpOwned && state.fencingGeneration == 0) {
        state.fencingGeneration = 1;
    }
    state.providerId = providerId;
}

void DocumentMutationAuthority::clearOwner(const Document& document)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _states.erase(&document);
}

MutationOwner DocumentMutationAuthority::owner(const Document& document) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto* state = stateFor(document);
    return state ? state->owner : MutationOwner::Unrestricted;
}

std::uint64_t DocumentMutationAuthority::fencingGeneration(const Document& document) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto* state = stateFor(document);
    return state ? state->fencingGeneration : 0;
}

std::string DocumentMutationAuthority::providerId(const Document& document) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto* state = stateFor(document);
    return state ? state->providerId : std::string {};
}

bool DocumentMutationAuthority::isRestricted(const Document& document) const
{
    return owner(document) == MutationOwner::McpOwned;
}

MutationCapabilityScope DocumentMutationAuthority::openCapability(Document& document,
                                                                  MutationKindMask kinds,
                                                                  std::uint64_t fencingGeneration,
                                                                  MutationOrigin origin)
{
    std::shared_ptr<ExternalMutationAuthorityProvider> providerCopy;
    MutationCapability cap;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto* state = stateFor(document);
        if (!state || state->owner != MutationOwner::McpOwned) {
            return MutationCapabilityScope({});
        }
        if (fencingGeneration != state->fencingGeneration) {
            return MutationCapabilityScope({});
        }
        providerCopy = _provider;
        cap.id = _nextCapabilityId++;
        cap.document = &document;
        cap.fencingGeneration = fencingGeneration;
        cap.allowedKinds = kinds ? kinds : MutationKindAll;
        cap.origin = origin;
    }

    if (providerCopy) {
        // Policy gate: if any requested kind is rejected, refuse issuance.
        for (MutationKindMask bit = 1; bit != 0 && bit <= MutationKindAll; bit <<= 1) {
            if ((cap.allowedKinds & bit) == 0) {
                continue;
            }
            if (!providerCopy->mayIssue(document, static_cast<MutationKind>(bit), fencingGeneration)) {
                return MutationCapabilityScope({});
            }
        }
    }

    return MutationCapabilityScope(std::move(cap));
}

std::uint64_t DocumentMutationAuthority::takeover(Document& document)
{
    std::uint64_t newGeneration = 0;
    std::shared_ptr<ExternalMutationAuthorityProvider> providerCopy;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto& state = _states[&document];
        state.fencingGeneration += 1;
        if (state.fencingGeneration == 0) {
            state.fencingGeneration = 1;
        }
        state.owner = MutationOwner::UserOwned;
        newGeneration = state.fencingGeneration;
        providerCopy = _provider;
    }

    signalTakeover(document, newGeneration);
    if (providerCopy) {
        providerCopy->onTakeover(document, newGeneration);
    }
    return newGeneration;
}

MutationDecision DocumentMutationAuthority::authorizeLocked(Document& document,
                                                            MutationKind kind,
                                                            const MutationContext& context,
                                                            const DocumentState& state) const
{
    if (state.recoveryMode) {
        return MutationDecision::DenyRecoveryMode;
    }

    if (state.owner == MutationOwner::Unrestricted
        || state.owner == MutationOwner::UserOwned) {
        return MutationDecision::Allow;
    }

    // McpOwned
    if (MutationAuthorityTLS::hasInternalGrant(&document)
        || context.origin == MutationOrigin::Internal) {
        return MutationDecision::Allow;
    }

    if (context.fencingGeneration != 0
        && context.fencingGeneration != state.fencingGeneration) {
        return MutationDecision::DenyStaleGeneration;
    }

    const auto& caps = MutationAuthorityTLS::activeCapabilities();
    for (const auto& cap : caps) {
        if (cap.document != &document) {
            continue;
        }
        if (cap.fencingGeneration != state.fencingGeneration) {
            continue;
        }
        if ((cap.allowedKinds & mutationKindBit(kind)) == 0) {
            continue;
        }
        return MutationDecision::Allow;
    }

    if (context.origin == MutationOrigin::Gui) {
        return MutationDecision::RequireTakeover;
    }
    return MutationDecision::DenyNoCapability;
}

MutationDecision DocumentMutationAuthority::authorize(Document& document,
                                                      MutationKind kind,
                                                      const MutationContext& context)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto* state = stateFor(document);
    if (!state || state->owner == MutationOwner::Unrestricted) {
        return MutationDecision::Allow;
    }
    return authorizeLocked(document, kind, context, *state);
}

void DocumentMutationAuthority::enforce(Document& document,
                                        MutationKind kind,
                                        MutationContext context)
{
    context.kind = kind;
    if (context.documentName.empty()) {
        context.documentName = document.getName();
    }
    if (context.fencingGeneration == 0) {
        context.fencingGeneration = fencingGeneration(document);
    }

    const MutationDecision decision = authorize(document, kind, context);
    if (decision == MutationDecision::Allow) {
        return;
    }

    signalDenied(document, context, decision);
    auto providerCopy = provider();
    if (providerCopy) {
        providerCopy->onDenied(document, context, decision);
    }

    std::ostringstream message;
    message << "Mutation denied (" << mutationDecisionName(decision) << "): "
            << mutationKindName(kind) << " on document '" << context.documentName << "'";
    if (!context.objectName.empty()) {
        message << " object '" << context.objectName << "'";
    }
    if (!context.propertyName.empty()) {
        message << " property '" << context.propertyName << "'";
    }
    throw Base::MutationDeniedException(message.str(), static_cast<int>(decision));
}

void DocumentMutationAuthority::forgetDocument(const Document& document)
{
    clearOwner(document);
}

Document* App::documentFromPropertyContainer(const PropertyContainer* container)
{
    if (!container) {
        return nullptr;
    }
    if (const auto* doc = dynamic_cast<const Document*>(container)) {
        return const_cast<Document*>(doc);
    }
    if (const auto* obj = dynamic_cast<const DocumentObject*>(container)) {
        return obj->getDocument();
    }
    return nullptr;
}

void App::enforceDocumentMutation(Document* document,
                                  MutationKind kind,
                                  MutationOrigin origin,
                                  const char* objectName,
                                  const char* propertyName)
{
    if (!document) {
        return;
    }
    MutationContext context;
    context.kind = kind;
    context.origin = origin;
    context.documentName = document->getName();
    if (objectName) {
        context.objectName = objectName;
    }
    if (propertyName) {
        context.propertyName = propertyName;
    }
    DocumentMutationAuthority::instance().enforce(*document, kind, std::move(context));
}
