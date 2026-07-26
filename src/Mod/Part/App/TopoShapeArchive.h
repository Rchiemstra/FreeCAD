// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Part/PartGlobal.h>
#include <App/GeometryJob.h>
#include <App/ElementMap.h>
#include <App/StringHasher.h>
#include <Mod/Part/App/TopoShape.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Part
{

using StringHasherSnapshot = App::StringHasherClosure;

struct PartExport HasherDeltaMergeResult
{
    bool success {false};
    std::string errorCode;
    std::string errorMessage;
    size_t appendedCount {0};
};

struct PartExport FrozenTopoShapeBundle
{
    TopoShape shape;
    Data::ElementMapPtr elementMap;
    App::StringHasherRef hasher;
    StringHasherSnapshot hasherSnapshot;
    long shapeTag {0};
    std::vector<std::string> mappedElements;
    /// False when createBundle could not clone map/hasher losslessly.
    bool valid {true};
    std::string errorCode;
};

class PartExport TopoShapeArchive
{
public:
    /// Reject archive sections larger than this (bytes).
    static constexpr uint32_t MaxSectionBytes = 256u * 1024u * 1024u;
    /// Reject hasher closures with more entries than this.
    static constexpr uint32_t MaxHasherEntries = 10u * 1000u * 1000u;
    /// Reject individual hasher data/postfix blobs larger than this.
    static constexpr uint32_t MaxHasherBlobBytes = 16u * 1024u * 1024u;

    TopoShapeArchive();
    ~TopoShapeArchive();

    /**
     * Capture an immutable frozen bundle from @p shape.
     * Copies ElementMap and the referenced hasher closure into private storage;
     * does not steal or null the source shape's map.
     */
    static FrozenTopoShapeBundle createBundle(const TopoShape& shape);

    static bool writeArchive(const FrozenTopoShapeBundle& bundle, const std::string& filePath);
    static bool readArchive(const std::string& filePath, FrozenTopoShapeBundle& outBundle);

    /// Stable fingerprint of a frozen bundle for join/coalesce identity digests.
    static std::string fingerprintBundle(const FrozenTopoShapeBundle& bundle);

    /**
     * Apply an exact-ID hasher delta onto @p hasher.
     * Existing IDs must match byte-for-byte; collisions or revision mismatches reject.
     * Missing IDs are appended with the same numeric ID.
     */
    /**
     * Apply an exact-ID hasher delta onto @p hasher using @p expectedRevision
     * (or the hasher's live revision when @p expectedRevision is 0).
     */
    static HasherDeltaMergeResult mergeHasherDelta(App::StringHasherRef hasher,
                                                   const StringHasherSnapshot& delta,
                                                   uint64_t expectedRevision);

    /**
     * Production commit helper: merge @p delta against the hasher's current revision,
     * then advance the revision on success so the snapshot cannot be re-applied.
     */
    static HasherDeltaMergeResult commitHasherDelta(App::StringHasherRef hasher,
                                                    const StringHasherSnapshot& delta);

    /**
     * Rebind @p bundle's ElementMap/SIDs onto @p hasher by save/restore clone.
     * Caller must already have materialized/merged @p bundle.hasherSnapshot into @p hasher.
     */
    static bool rebindBundleToHasher(FrozenTopoShapeBundle& bundle, App::StringHasherRef hasher);

    /**
     * Merge authenticated base/tool closures into one fresh worker hasher and rebind both
     * operands so every map/SID/TopoShape::Hasher refers to that same hasher.
     */
    static bool materializeOperandsOntoSharedHasher(const FrozenTopoShapeBundle& baseIn,
                                                    const FrozenTopoShapeBundle& toolIn,
                                                    App::StringHasherRef workerHasher,
                                                    FrozenTopoShapeBundle& baseOut,
                                                    FrozenTopoShapeBundle& toolOut,
                                                    std::string& errorCode,
                                                    std::string& errorMessage);

    static std::string calculateSha256(const std::vector<uint8_t>& data);
    static std::string calculateSha256File(const std::string& path);

    /// Checked int64 → long narrowing used by FCG1 hasher closure decode.
    static bool int64ToLongChecked(int64_t value, long& out);

#ifdef FC_TOPOSHape_ARCHIVE_TEST_SEAMS
    /// Test-only: force live closure capture failure inside writeArchive.
    static void setTestForceClosureCaptureFailure(bool enabled);
#endif
};

} // namespace Part
