// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentRevisionIndex.h"
#include "MutationKind.h"

#include <FCGlobal.h>

#include <optional>
#include <string>
#include <vector>

namespace App
{

/** The post-change funnel that observed a compatibility mutation. */
enum class CollaborationMutationSource
{
    PropertyValue,
    PropertyStatus,
    DynamicPropertySchema,
    ObjectAddition,
    ObjectRemoval,
    Unknown
};

/** A deliberately coarse property family used to reject unsupported value semantics. */
enum class CollaborationPropertyFamily
{
    NotApplicable,
    ModelValue,
    Link,
    Unknown
};

/** The scope of the PropertyContainer at the mutation funnel. */
enum class CollaborationContainerKind
{
    DocumentObject,
    Document,
    Unknown
};

/**
 * Pointer-free description of one completed local mutation.
 *
 * Callers derive these values while they have access to the live mutation site. The classifier
 * neither retains nor resolves live objects. Object-scoped sites require both objectName and a
 * non-empty stableObjectIdentity; document-scoped sites require neither.
 */
struct AppExport MutationClassificationInput
{
    CollaborationMutationSource source {CollaborationMutationSource::Unknown};
    MutationKind mutationKind {MutationKind::PropertyWrite};
    CollaborationPropertyFamily propertyFamily {CollaborationPropertyFamily::Unknown};
    CollaborationContainerKind containerKind {CollaborationContainerKind::Unknown};
    std::string objectName;
    std::optional<std::string> stableObjectIdentity;
    /** Separate scalar; never concatenate it with objectName. */
    std::string propertyName;
};

using MutationRevisionEffects = std::vector<DocumentRevisionPublicationRequest>;

/**
 * Return the smallest revision-effect set proven by the supplied mutation site.
 *
 * An unrecognized or internally inconsistent description always returns the document wildcard.
 * This function is deterministic and has no document mutation, locking, or publication effects.
 */
[[nodiscard]] AppExport MutationRevisionEffects
classifyMutation(const MutationClassificationInput& input);

}  // namespace App
