// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <Mod/Part/PartGlobal.h>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

class TopoDS_Shape;

namespace App
{
class Document;
struct CollaborativeOperationIntent;
struct CollaborativeOperationPreparation;
}

namespace Part
{

inline constexpr std::string_view CollaborativeBooleanOperationType =
    "Part.CollaborativeBoolean";

/**
 * Register the native Part Boolean collaboration adapter exactly once.
 *
 * Its pointer-free intent accepts exactly the string arguments `base`, `tool`,
 * `result`, and `kind`; kind is one of `cut`, `fuse`, or `common`. All three
 * object arguments name pre-existing Part::Feature objects; result must be an
 * exact Part::Feature so recompute cannot replace the committed shape through
 * a subclass execute implementation. Preparation captures independent input
 * shapes on the document owner thread and returns a detached Boolean task.
 */
PartExport void ensureCollaborativeBooleanOperationRegistered();

namespace Internal
{

inline constexpr std::size_t CollaborativeBooleanDigestSize = 32;
using CollaborativeBooleanDigest =
    std::array<std::uint8_t, CollaborativeBooleanDigestSize>;

/** Task-local native probe used only by the detached Boolean tests. */
struct PartExport CollaborativeBooleanTaskProbe
{
    std::atomic<bool> baseSnapshotIndependent {false};
    std::atomic<bool> toolSnapshotIndependent {false};
    std::atomic<bool> buildEntered {false};
    std::atomic<unsigned int> progressCallbacks {0};
    std::atomic<bool> cancellationObserved {false};
    std::atomic<bool> buildCompletedNaturally {false};
    std::atomic<std::size_t> expectedCanonicalBytes {0};
};

PartExport App::CollaborativeOperationPreparation prepareCollaborativeBooleanForTests(
    const App::Document& document,
    const App::CollaborativeOperationIntent& intent,
    std::shared_ptr<CollaborativeBooleanTaskProbe> probe);

PartExport CollaborativeBooleanDigest
collaborativeBooleanShapeDigestForTests(const TopoDS_Shape& shape);

}  // namespace Internal

}  // namespace Part
