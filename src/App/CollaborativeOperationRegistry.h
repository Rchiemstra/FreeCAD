// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "CollaborativeOperation.h"
#include "DocumentRevisionIndex.h"
#include "GeometryArchive.h"
#include "GeometryJobManager.h"
#include "PreparationPolicy.h"

#include <FCGlobal.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace App
{

class Document;

/** Pointer-free client intent. Only registered native adapters interpret it. */
struct AppExport CollaborativeOperationIntent
{
    std::string operationType;
    std::map<std::string, std::string> arguments;
};

/** Native-adapter output consumed only inside DocumentCollaborationService. */
struct AppExport CollaborativeOperationPreparation
{
    using DetachedTask =
        std::function<std::unique_ptr<const CollaborativeOperation>(std::stop_token)>;
    using IsolatedResultDecoder =
        std::function<std::unique_ptr<const CollaborativeOperation>(const GeometryArchive&)>;

    struct IsolatedTask
    {
        GeometryJobRequest request;
        GeometryArchive inputArchive;
        IsolatedResultDecoder decodeResult;
    };

    CollaborativeOperationPreparation(
        std::vector<DocumentRevisionKey> readSet,
        std::vector<DocumentRevisionKey> writeSet,
        std::vector<DocumentRevisionPublicationRequest> publicationEffects,
        std::unique_ptr<const CollaborativeOperation> operation)
        : readSet(std::move(readSet))
        , writeSet(std::move(writeSet))
        , publicationEffects(std::move(publicationEffects))
        , operation(std::move(operation))
        , policy(PreparationPolicy::Inline)
    {}

    CollaborativeOperationPreparation(
        std::vector<DocumentRevisionKey> readSet,
        std::vector<DocumentRevisionKey> writeSet,
        std::vector<DocumentRevisionPublicationRequest> publicationEffects,
        IsolatedTask isolatedTask)
        : readSet(std::move(readSet))
        , writeSet(std::move(writeSet))
        , publicationEffects(std::move(publicationEffects))
        , isolatedTask(std::make_unique<IsolatedTask>(std::move(isolatedTask)))
        , policy(PreparationPolicy::IsolatedProcess)
    {}

    CollaborativeOperationPreparation(
        std::vector<DocumentRevisionKey> readSet,
        std::vector<DocumentRevisionKey> writeSet,
        std::vector<DocumentRevisionPublicationRequest> publicationEffects,
        DetachedTask detachedTask,
        PreparationPolicy policy = PreparationPolicy::DetachedInProcess)
        : readSet(std::move(readSet))
        , writeSet(std::move(writeSet))
        , publicationEffects(std::move(publicationEffects))
        , detachedTask(std::move(detachedTask))
        , policy(policy)
    {}

    [[nodiscard]] bool isDetached() const noexcept
    {
        return static_cast<bool>(detachedTask) || static_cast<bool>(isolatedTask);
    }

    std::vector<DocumentRevisionKey> readSet;
    std::vector<DocumentRevisionKey> writeSet;
    std::vector<DocumentRevisionPublicationRequest> publicationEffects;
    std::unique_ptr<const CollaborativeOperation> operation;
    DetachedTask detachedTask;
    std::unique_ptr<IsolatedTask> isolatedTask;
    PreparationPolicy policy {PreparationPolicy::Inline};
    std::uint64_t registrationId {0};
};

using CollaborativeOperationAdapter =
    std::function<CollaborativeOperationPreparation(const Document&,
                                                    const CollaborativeOperationIntent&)>;

namespace Internal
{
class CollaborativeOperationRegistrar;
}

/**
 * Process-wide registry of trusted native operation adapters.
 *
 * The caller can select a registered type but cannot supply an operation,
 * dependency set, or publication effect. Registration is a native FreeCAD
 * extension boundary and is not exposed through Python or remote bindings.
 */
class AppExport CollaborativeOperationRegistry
{
public:
    using RegistrationId = std::uint64_t;

    static CollaborativeOperationRegistry& instance();

    [[nodiscard]] CollaborativeOperationPreparation prepare(
        const Document& document,
        const CollaborativeOperationIntent& intent) const;
    [[nodiscard]] bool contains(const std::string& operationType) const;

private:
    friend class Internal::CollaborativeOperationRegistrar;
    friend class DocumentCollaborationService;

    RegistrationId registerAdapter(std::string operationType,
                                   CollaborativeOperationAdapter adapter);
    [[nodiscard]] bool matches(RegistrationId registrationId,
                               const std::string& operationType) const;
    struct Entry
    {
        RegistrationId id;
        CollaborativeOperationAdapter adapter;
    };

    mutable std::mutex _mutex;
    std::map<std::string, Entry> _adapters;
    RegistrationId _nextRegistrationId {1};
};

}  // namespace App
