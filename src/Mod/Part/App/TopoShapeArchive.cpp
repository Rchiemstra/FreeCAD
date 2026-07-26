// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TopoShapeArchive.h"
#include <Base/Console.h>
#include <QCryptographicHash>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Integer.hxx>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <atomic>
#include <stdexcept>

namespace Part
{

namespace
{

#ifdef FC_TOPOSHape_ARCHIVE_TEST_SEAMS
std::atomic<bool> g_testForceClosureCaptureFailure {false};
#endif

bool writeBytes(std::ostream& out, const void* data, size_t size)
{
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

bool readExact(std::istream& in, void* data, size_t size)
{
    in.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(in) && static_cast<size_t>(in.gcount()) == size;
}

bool writeQByteArray(std::ostream& out, const QByteArray& bytes)
{
    if (bytes.size() < 0
        || static_cast<uint32_t>(bytes.size()) > TopoShapeArchive::MaxHasherBlobBytes) {
        return false;
    }
    uint32_t len = static_cast<uint32_t>(bytes.size());
    if (!writeBytes(out, &len, sizeof(len))) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    return writeBytes(out, bytes.constData(), len);
}

bool readQByteArray(std::istream& in, QByteArray& bytes)
{
    uint32_t len = 0;
    if (!readExact(in, &len, sizeof(len))) {
        return false;
    }
    if (len > TopoShapeArchive::MaxHasherBlobBytes) {
        return false;
    }
    bytes.resize(static_cast<int>(len));
    if (len == 0) {
        return true;
    }
    return readExact(in, bytes.data(), len);
}

bool writeHasherClosure(std::ostream& out, const App::StringHasherClosure& closure)
{
    uint32_t count = static_cast<uint32_t>(closure.entries.size());
    if (count > TopoShapeArchive::MaxHasherEntries) {
        return false;
    }
    if (!writeBytes(out, &count, sizeof(count))) {
        return false;
    }
    for (const auto& entry : closure.entries) {
        int64_t id = entry.id;
        uint32_t flags = entry.flags;
        if (!writeBytes(out, &id, sizeof(id)) || !writeBytes(out, &flags, sizeof(flags))) {
            return false;
        }
        if (!writeQByteArray(out, entry.data) || !writeQByteArray(out, entry.postfix)) {
            return false;
        }
        uint32_t relatedCount = static_cast<uint32_t>(entry.relatedIds.size());
        if (relatedCount > 1024u) {
            return false;
        }
        if (!writeBytes(out, &relatedCount, sizeof(relatedCount))) {
            return false;
        }
        for (long relatedId : entry.relatedIds) {
            int64_t rid = relatedId;
            if (!writeBytes(out, &rid, sizeof(rid))) {
                return false;
            }
        }
    }
    return static_cast<bool>(out);
}

bool readHasherClosure(std::istream& in, App::StringHasherClosure& closure)
{
    uint32_t count = 0;
    if (!readExact(in, &count, sizeof(count))) {
        return false;
    }
    if (count > TopoShapeArchive::MaxHasherEntries) {
        return false;
    }
    closure.entries.clear();
    closure.entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        App::StringHasherClosureEntry entry;
        int64_t id = 0;
        uint32_t flags = 0;
        if (!readExact(in, &id, sizeof(id)) || !readExact(in, &flags, sizeof(flags))) {
            return false;
        }
        if (!TopoShapeArchive::int64ToLongChecked(id, entry.id)) {
            return false;
        }
        entry.flags = flags;
        if (!readQByteArray(in, entry.data) || !readQByteArray(in, entry.postfix)) {
            return false;
        }
        uint32_t relatedCount = 0;
        if (!readExact(in, &relatedCount, sizeof(relatedCount)) || relatedCount > 1024u) {
            return false;
        }
        entry.relatedIds.reserve(relatedCount);
        for (uint32_t r = 0; r < relatedCount; ++r) {
            int64_t rid = 0;
            if (!readExact(in, &rid, sizeof(rid))) {
                return false;
            }
            long related = 0;
            if (!TopoShapeArchive::int64ToLongChecked(rid, related)) {
                return false;
            }
            entry.relatedIds.push_back(related);
        }
        closure.entries.push_back(std::move(entry));
    }
    return true;
}

void appendBytes(std::vector<uint8_t>& dest, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    dest.insert(dest.end(), bytes, bytes + size);
}

void appendBytes(std::vector<uint8_t>& dest, const std::string& data)
{
    appendBytes(dest, data.data(), data.size());
}

} // namespace

bool TopoShapeArchive::int64ToLongChecked(int64_t value, long& out)
{
    if (value < static_cast<int64_t>(std::numeric_limits<long>::min())
        || value > static_cast<int64_t>(std::numeric_limits<long>::max())) {
        return false;
    }
    out = static_cast<long>(value);
    return true;
}

#ifdef FC_TOPOSHape_ARCHIVE_TEST_SEAMS
void TopoShapeArchive::setTestForceClosureCaptureFailure(bool enabled)
{
    g_testForceClosureCaptureFailure.store(enabled);
}
#endif

TopoShapeArchive::TopoShapeArchive() = default;
TopoShapeArchive::~TopoShapeArchive() = default;

FrozenTopoShapeBundle TopoShapeArchive::createBundle(const TopoShape& shape)
{
    FrozenTopoShapeBundle bundle;
    bundle.shapeTag = shape.Tag;
    bundle.valid = true;

    // Private OCC geometry copy — TopoShape assignment shares the underlying TopoDS_Shape.
    bundle.shape = TopoShape(shape.Tag);
    try {
        bundle.shape.makeElementCopy(shape, nullptr, /*copyGeom=*/true, /*copyMesh=*/true);
    }
    catch (...) {
        // Fall back to handle-shared assignment if copy fails; mark invalid if empty.
        bundle.shape = shape;
        bundle.shape.Tag = shape.Tag;
    }
    bundle.shape.Tag = shape.Tag;

    // Copy shares the ElementMap shared_ptr; stealing from the scratch copy leaves the
    // source shape's map intact.
    TopoShape scratch = shape;
    (void)scratch.getElementMapSize(/*flush=*/true);
    Data::ElementMapPtr sourceMap = scratch.resetElementMap();
    App::StringHasherRef sourceHasher = shape.Hasher;

    std::unordered_map<long, bool> savedMarks;
    std::unordered_map<const Data::ElementMap*, unsigned> savedMapIds;
    if (sourceHasher) {
        savedMarks = sourceHasher->snapshotMarks();
    }
    if (sourceMap) {
        sourceMap->snapshotArchiveIds(savedMapIds);
    }

    auto restoreLiveState = [&]() {
        if (sourceMap && !savedMapIds.empty()) {
            sourceMap->restoreArchiveIds(savedMapIds);
        }
        if (sourceHasher && !savedMarks.empty()) {
            sourceHasher->restoreMarks(savedMarks);
        }
    };

    if (sourceMap && sourceHasher) {
        sourceHasher->clearMarks();
        Data::ElementMapArchiveContext markCtx;
        markCtx.hasher = sourceHasher;
        sourceMap->beforeSave(markCtx);
        bundle.hasherSnapshot = sourceHasher->captureClosure(/*markedOnly=*/true);
    }
    else if (sourceHasher) {
        bundle.hasherSnapshot = sourceHasher->captureClosure(/*markedOnly=*/false);
    }
    bundle.hasherSnapshot.revision = sourceHasher ? sourceHasher->getRevision() : 0;
    bundle.hasherSnapshot.highWaterId =
        sourceHasher ? static_cast<uint64_t>(sourceHasher->getLastID()) : 0;
    if (sourceHasher) {
        bundle.hasherSnapshot.threshold = sourceHasher->getThreshold();
    }

    // Private hasher clone carrying the exact-ID closure.
    App::StringHasherRef privateHasher(new App::StringHasher);
    if (!bundle.hasherSnapshot.entries.empty()) {
        auto merged =
            privateHasher->materializeExactClosure(bundle.hasherSnapshot);
        if (!merged.success) {
            restoreLiveState();
            bundle.valid = false;
            bundle.errorCode = merged.errorCode.empty() ? "HasherCloneFailed" : merged.errorCode;
            bundle.elementMap.reset();
            bundle.hasher = App::StringHasherRef();
            bundle.shape.Hasher = App::StringHasherRef();
            bundle.shape.resetElementMap();
            return bundle;
        }
    }
    else if (bundle.hasherSnapshot.highWaterId != 0) {
        if (bundle.hasherSnapshot.highWaterId
            > static_cast<uint64_t>(std::numeric_limits<long>::max())) {
            bundle.valid = false;
            bundle.errorCode = "HighWaterOutOfRange";
            return bundle;
        }
        privateHasher->reserveHighWater(bundle.hasherSnapshot.highWaterId);
    }
    if (sourceHasher) {
        privateHasher->setRevision(sourceHasher->getRevision());
        privateHasher->setThreshold(sourceHasher->getThreshold());
    }
    bundle.hasher = privateHasher;

    if (sourceMap) {
        Data::ElementMapArchiveContext saveCtx;
        saveCtx.hasher = sourceHasher;
        // Re-run beforeSave into a fresh context so map IDs are contiguous for this archive.
        // Live marks/`_id` are restored below.
        if (sourceHasher) {
            sourceHasher->clearMarks();
        }
        sourceMap->beforeSave(saveCtx);

        std::ostringstream mapStream(std::ios::binary);
        sourceMap->save(mapStream);

        Data::ElementMapArchiveContext restoreCtx;
        restoreCtx.hasher = privateHasher;
        std::istringstream mapIn(mapStream.str(), std::ios::binary);
        try {
            auto restored = std::make_shared<Data::ElementMap>();
            bundle.elementMap = restored->restore(restoreCtx, mapIn);
            if (!bundle.elementMap) {
                restoreLiveState();
                bundle.valid = false;
                bundle.errorCode = "ElementMapCloneFailed";
                bundle.hasher = App::StringHasherRef();
                bundle.shape.Hasher = App::StringHasherRef();
                bundle.shape.resetElementMap();
                return bundle;
            }
        }
        catch (const Base::Exception& e) {
            Base::Console().log("TopoShapeArchive::createBundle: ElementMap clone failed: %s\n",
                                e.what());
            restoreLiveState();
            bundle.valid = false;
            bundle.errorCode = "ElementMapCloneFailed";
            bundle.elementMap.reset();
            bundle.hasher = App::StringHasherRef();
            bundle.shape.Hasher = App::StringHasherRef();
            bundle.shape.resetElementMap();
            return bundle;
        }
        catch (...) {
            Base::Console().log("TopoShapeArchive::createBundle: ElementMap clone failed\n");
            restoreLiveState();
            bundle.valid = false;
            bundle.errorCode = "ElementMapCloneFailed";
            bundle.elementMap.reset();
            bundle.hasher = App::StringHasherRef();
            bundle.shape.Hasher = App::StringHasherRef();
            bundle.shape.resetElementMap();
            return bundle;
        }
    }

    restoreLiveState();

    bundle.shape.Hasher = privateHasher;
    bundle.shape.resetElementMap(bundle.elementMap);

    if (bundle.elementMap) {
        for (const auto& mapped : shape.getElementMap()) {
            bundle.mappedElements.push_back(mapped.name.toString() + "->"
                                            + mapped.index.toString());
        }
    }

    return bundle;
}

bool TopoShapeArchive::writeArchive(const FrozenTopoShapeBundle& bundle, const std::string& filePath)
{
    if (!bundle.valid) {
        return false;
    }

    namespace fs = std::filesystem;

    // beforeSave/clearMarks mutate hasher marks and map archive IDs through the
    // const bundle's private clones. Snapshot and restore so repeat writes are
    // byte-identical and exception-safe.
    std::unordered_map<long, bool> savedMarks;
    std::unordered_map<const Data::ElementMap*, unsigned> savedMapIds;
    App::StringHasherRef writeHasher = bundle.hasher ? bundle.hasher : bundle.shape.Hasher;
    if (writeHasher) {
        savedMarks = writeHasher->snapshotMarks();
    }
    if (bundle.elementMap) {
        bundle.elementMap->snapshotArchiveIds(savedMapIds);
    }
    auto restoreWriteState = [&]() {
        if (bundle.elementMap && !savedMapIds.empty()) {
            bundle.elementMap->restoreArchiveIds(savedMapIds);
        }
        if (writeHasher && !savedMarks.empty()) {
            writeHasher->restoreMarks(savedMarks);
        }
    };

    std::ostringstream payloadStream(std::ios::binary);

    // 1. FCG1 Header & Version (v4: hasher threshold for hashed-name round-trip)
    payloadStream.write("FCG1", 4);
    uint32_t version = 4;
    payloadStream.write(reinterpret_cast<const char*>(&version), sizeof(version));

    // 2. Shape Tag Section
    long tag = bundle.shapeTag;
    payloadStream.write(reinterpret_cast<const char*>(&tag), sizeof(tag));

    // 3. BREP Geometry Section
    std::ostringstream shapeStream(std::ios::binary);
    bundle.shape.exportBinary(shapeStream);
    std::string shapeData = shapeStream.str();
    if (shapeData.size() > MaxSectionBytes) {
        restoreWriteState();
        return false;
    }
    uint32_t shapeLen = static_cast<uint32_t>(shapeData.size());
    payloadStream.write(reinterpret_cast<const char*>(&shapeLen), sizeof(shapeLen));
    if (shapeLen > 0) {
        payloadStream.write(shapeData.data(), shapeLen);
    }

    // 4. ElementMap Section (explicit per-archive context; no process globals)
    std::ostringstream mapStream(std::ios::binary);
    if (bundle.elementMap) {
        Data::ElementMapArchiveContext ctx;
        ctx.hasher = writeHasher;
        if (ctx.hasher) {
            ctx.hasher->clearMarks();
        }
        try {
            bundle.elementMap->beforeSave(ctx);
            bundle.elementMap->save(mapStream);
        }
        catch (...) {
            restoreWriteState();
            return false;
        }
    }
    std::string mapData = mapStream.str();
    if (mapData.size() > MaxSectionBytes) {
        restoreWriteState();
        return false;
    }
    uint32_t mapLen = static_cast<uint32_t>(mapData.size());
    payloadStream.write(reinterpret_cast<const char*>(&mapLen), sizeof(mapLen));
    if (mapLen > 0) {
        payloadStream.write(mapData.data(), mapLen);
    }

    // 5. StringHasher high-water + revision + threshold + exact-ID closure
    App::StringHasherClosure closureForWrite = bundle.hasherSnapshot;
    if (writeHasher) {
        try {
#ifdef FC_TOPOSHape_ARCHIVE_TEST_SEAMS
            if (g_testForceClosureCaptureFailure.load()) {
                throw std::runtime_error("test forced closure capture failure");
            }
#endif
            closureForWrite = writeHasher->captureClosure(/*markedOnly=*/true);
            closureForWrite.revision = writeHasher->getRevision();
            closureForWrite.highWaterId = static_cast<uint64_t>(writeHasher->getLastID());
            closureForWrite.threshold = writeHasher->getThreshold();
        }
        catch (...) {
            restoreWriteState();
            return false;
        }
    }
    uint64_t highWaterId = closureForWrite.highWaterId;
    uint64_t revision = closureForWrite.revision;
    int32_t threshold = closureForWrite.threshold;
    payloadStream.write(reinterpret_cast<const char*>(&highWaterId), sizeof(highWaterId));
    payloadStream.write(reinterpret_cast<const char*>(&revision), sizeof(revision));
    payloadStream.write(reinterpret_cast<const char*>(&threshold), sizeof(threshold));

    std::ostringstream hasherStream(std::ios::binary);
    if (!writeHasherClosure(hasherStream, closureForWrite)) {
        restoreWriteState();
        return false;
    }
    std::string hasherData = hasherStream.str();
    if (hasherData.size() > MaxSectionBytes) {
        restoreWriteState();
        return false;
    }
    uint32_t hasherLen = static_cast<uint32_t>(hasherData.size());
    payloadStream.write(reinterpret_cast<const char*>(&hasherLen), sizeof(hasherLen));
    if (hasherLen > 0) {
        payloadStream.write(hasherData.data(), hasherLen);
    }

    // 6. SHA-256 over authenticated metadata + payloads
    std::vector<uint8_t> digestInput;
    appendBytes(digestInput, &version, sizeof(version));
    appendBytes(digestInput, &tag, sizeof(tag));
    appendBytes(digestInput, shapeData);
    appendBytes(digestInput, mapData);
    appendBytes(digestInput, &highWaterId, sizeof(highWaterId));
    appendBytes(digestInput, &revision, sizeof(revision));
    appendBytes(digestInput, &threshold, sizeof(threshold));
    appendBytes(digestInput, hasherData);
    std::string checksum = calculateSha256(digestInput);

    uint32_t checksumLen = static_cast<uint32_t>(checksum.size());
    payloadStream.write(reinterpret_cast<const char*>(&checksumLen), sizeof(checksumLen));
    payloadStream.write(checksum.data(), static_cast<std::streamsize>(checksumLen));

    const std::string body = payloadStream.str();
    const fs::path finalPath(filePath);
    const fs::path tmpPath = finalPath.string() + ".tmp";

    {
        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            restoreWriteState();
            return false;
        }
        ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
        ofs.close();
        if (!ofs) {
            fs::remove(tmpPath);
            restoreWriteState();
            return false;
        }
    }

    std::error_code ec;
    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
        fs::remove(finalPath, ec);
        fs::rename(tmpPath, finalPath, ec);
        if (ec) {
            fs::remove(tmpPath);
            restoreWriteState();
            return false;
        }
    }
    restoreWriteState();
    return true;
}

bool TopoShapeArchive::readArchive(const std::string& filePath, FrozenTopoShapeBundle& outBundle)
{
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }

    FrozenTopoShapeBundle candidate;
    candidate.valid = false;

    char magic[4];
    if (!readExact(ifs, magic, 4) || std::string(magic, 4) != "FCG1") {
        return false;
    }

    uint32_t version = 0;
    if (!readExact(ifs, &version, sizeof(version)) || version < 1 || version > 4) {
        return false;
    }

    long tag = 0;
    if (!readExact(ifs, &tag, sizeof(tag))) {
        return false;
    }

    uint32_t shapeLen = 0;
    if (!readExact(ifs, &shapeLen, sizeof(shapeLen)) || shapeLen > MaxSectionBytes) {
        return false;
    }
    std::string shapeData(shapeLen, '\0');
    if (shapeLen > 0 && !readExact(ifs, shapeData.data(), shapeLen)) {
        return false;
    }

    uint32_t mapLen = 0;
    if (!readExact(ifs, &mapLen, sizeof(mapLen)) || mapLen > MaxSectionBytes) {
        return false;
    }
    std::string mapData(mapLen, '\0');
    if (mapLen > 0 && !readExact(ifs, mapData.data(), mapLen)) {
        return false;
    }

    uint64_t highWaterId = 0;
    if (!readExact(ifs, &highWaterId, sizeof(highWaterId))) {
        return false;
    }
    if (highWaterId > static_cast<uint64_t>(std::numeric_limits<long>::max())) {
        Base::Console().log("TopoShapeArchive: highWaterId exceeds long range\n");
        return false;
    }

    uint64_t revision = 0;
    if (version >= 2) {
        if (!readExact(ifs, &revision, sizeof(revision))) {
            return false;
        }
    }

    int32_t threshold = 0;
    if (version >= 4) {
        if (!readExact(ifs, &threshold, sizeof(threshold))) {
            return false;
        }
        if (threshold < 0) {
            return false;
        }
    }

    std::string hasherData;
    if (version >= 3) {
        uint32_t hasherLen = 0;
        if (!readExact(ifs, &hasherLen, sizeof(hasherLen)) || hasherLen > MaxSectionBytes) {
            return false;
        }
        hasherData.assign(hasherLen, '\0');
        if (hasherLen > 0 && !readExact(ifs, hasherData.data(), hasherLen)) {
            return false;
        }
    }

    uint32_t checksumLen = 0;
    if (!readExact(ifs, &checksumLen, sizeof(checksumLen)) || checksumLen > 128u) {
        return false;
    }
    std::string checksum(checksumLen, '\0');
    if (checksumLen > 0 && !readExact(ifs, checksum.data(), checksumLen)) {
        return false;
    }

    // Reject trailing data after the authenticated archive.
    if (ifs.peek() != std::char_traits<char>::eof()) {
        char junk = 0;
        if (ifs.read(&junk, 1) && ifs.gcount() > 0) {
            Base::Console().log("TopoShapeArchive: trailing data after checksum\n");
            return false;
        }
    }

    std::vector<uint8_t> digestInput;
    if (version >= 3) {
        appendBytes(digestInput, &version, sizeof(version));
        appendBytes(digestInput, &tag, sizeof(tag));
        appendBytes(digestInput, shapeData);
        appendBytes(digestInput, mapData);
        appendBytes(digestInput, &highWaterId, sizeof(highWaterId));
        appendBytes(digestInput, &revision, sizeof(revision));
        if (version >= 4) {
            appendBytes(digestInput, &threshold, sizeof(threshold));
        }
        appendBytes(digestInput, hasherData);
    }
    else {
        appendBytes(digestInput, shapeData);
        appendBytes(digestInput, mapData);
    }
    if (checksum != calculateSha256(digestInput)) {
        Base::Console().log("TopoShapeArchive checksum mismatch!\n");
        return false;
    }

    candidate.shapeTag = tag;
    candidate.hasherSnapshot.highWaterId = highWaterId;
    candidate.hasherSnapshot.revision = revision;
    candidate.hasherSnapshot.threshold = threshold;

    if (version >= 3 && !hasherData.empty()) {
        std::istringstream hasherStream(hasherData, std::ios::binary);
        if (!readHasherClosure(hasherStream, candidate.hasherSnapshot)) {
            Base::Console().log("TopoShapeArchive: hasher closure decode failed\n");
            return false;
        }
        candidate.hasherSnapshot.highWaterId = highWaterId;
        candidate.hasherSnapshot.revision = revision;
        candidate.hasherSnapshot.threshold = threshold;
    }

    App::StringHasherRef privateHasher(new App::StringHasher);
    if (!candidate.hasherSnapshot.entries.empty()) {
        auto merged = privateHasher->materializeExactClosure(candidate.hasherSnapshot);
        if (!merged.success) {
            Base::Console().log("TopoShapeArchive: hasher materialize failed (%s)\n",
                                merged.errorCode.c_str());
            return false;
        }
    }
    else if (highWaterId != 0) {
        if (highWaterId > static_cast<uint64_t>(std::numeric_limits<long>::max())) {
            Base::Console().log("TopoShapeArchive: highWaterId exceeds long range\n");
            return false;
        }
        privateHasher->reserveHighWater(highWaterId);
        privateHasher->setThreshold(threshold);
        privateHasher->setRevision(revision);
    }
    else {
        privateHasher->setThreshold(threshold);
        privateHasher->setRevision(revision);
    }
    candidate.hasher = privateHasher;

    if (shapeLen > 0) {
        try {
            std::istringstream shapeStream(shapeData, std::ios::binary);
            candidate.shape.importBinary(shapeStream);
            candidate.shape.Tag = tag;
        }
        catch (...) {
            Base::Console().log("TopoShapeArchive: BREP import failed after checksum OK\n");
            return false;
        }
    }
    candidate.shape.Hasher = privateHasher;

    if (mapLen > 0) {
        try {
            Data::ElementMapArchiveContext ctx;
            ctx.hasher = privateHasher;
            std::istringstream mapStream(mapData, std::ios::binary);
            auto restored = std::make_shared<Data::ElementMap>();
            candidate.elementMap = restored->restore(ctx, mapStream);
            if (!candidate.elementMap) {
                Base::Console().log("TopoShapeArchive: ElementMap restore returned null\n");
                return false;
            }
            candidate.shape.resetElementMap(candidate.elementMap);
        }
        catch (...) {
            Base::Console().log("TopoShapeArchive: ElementMap restore failed\n");
            return false;
        }
    }

    candidate.valid = true;
    outBundle = std::move(candidate);
    return true;
}

std::string TopoShapeArchive::fingerprintBundle(const FrozenTopoShapeBundle& bundle)
{
    std::ostringstream os;
    os << "tag=" << bundle.shapeTag
       << ";hw=" << bundle.hasherSnapshot.highWaterId
       << ";rev=" << bundle.hasherSnapshot.revision
       << ";entries=" << bundle.hasherSnapshot.entries.size()
       << ";map=" << (bundle.elementMap ? bundle.elementMap->size() : 0u);
    if (!bundle.shape.isNull()) {
        const TopoDS_Shape& s = bundle.shape.getShape();
        os << ";hash=" << s.HashCode(IntegerLast());
        Bnd_Box box;
        try {
            BRepBndLib::Add(s, box);
            if (!box.IsVoid()) {
                double xmin = 0, ymin = 0, zmin = 0, xmax = 0, ymax = 0, zmax = 0;
                box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
                os << ";bb=" << xmin << ',' << ymin << ',' << zmin << ',' << xmax << ',' << ymax
                   << ',' << zmax;
            }
        }
        catch (...) {
            os << ";bb=err";
        }
    }
    else {
        os << ";null";
    }
    return os.str();
}

std::string TopoShapeArchive::calculateSha256(const std::vector<uint8_t>& data)
{
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    if (!data.empty()) {
#if QT_VERSION < QT_VERSION_CHECK(6, 3, 0)
        hasher.addData(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
#else
        hasher.addData(QByteArrayView(reinterpret_cast<const char*>(data.data()),
                                      static_cast<qsizetype>(data.size())));
#endif
    }
    return hasher.result().toHex().toStdString();
}

std::string TopoShapeArchive::calculateSha256File(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return {};
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
    return calculateSha256(data);
}

bool TopoShapeArchive::rebindBundleToHasher(FrozenTopoShapeBundle& bundle,
                                            App::StringHasherRef hasher)
{
    if (!hasher || !bundle.valid) {
        return false;
    }
    if (!bundle.elementMap) {
        bundle.hasher = hasher;
        bundle.shape.Hasher = hasher;
        return true;
    }

    // Snapshot source marks/map IDs so force-mark + save cannot leak into inputs.
    const App::StringHasherRef sourceHasher = bundle.hasher;
    std::unordered_map<long, bool> savedMarks;
    std::unordered_map<const Data::ElementMap*, unsigned> savedMapIds;
    if (sourceHasher) {
        savedMarks = sourceHasher->snapshotMarks();
    }
    bundle.elementMap->snapshotArchiveIds(savedMapIds);

    auto restoreSourceState = [&]() {
        if (bundle.elementMap && !savedMapIds.empty()) {
            bundle.elementMap->restoreArchiveIds(savedMapIds);
        }
        if (sourceHasher && !savedMarks.empty()) {
            sourceHasher->restoreMarks(savedMarks);
        }
    };

    auto forceMark = [](const Data::ElementMapPtr& map, auto&& self) -> void {
        if (!map) {
            return;
        }
        for (const auto& child : map->getChildElements()) {
            for (auto& sid : child.sids) {
                sid.mark();
            }
            self(child.elementMap, self);
        }
        for (const auto& el : map->getAll()) {
            Data::ElementIDRefs sids;
            (void)map->find(el.index, &sids);
            for (auto& sid : sids) {
                sid.mark();
            }
        }
    };

    auto collectSidValues = [](const Data::ElementMapPtr& map, auto&& self, std::vector<long>& out) {
        if (!map) {
            return;
        }
        for (const auto& child : map->getChildElements()) {
            for (const auto& sid : child.sids) {
                if (sid) {
                    out.push_back(sid.value());
                }
            }
            self(child.elementMap, self, out);
        }
        for (const auto& el : map->getAll()) {
            Data::ElementIDRefs sids;
            (void)map->find(el.index, &sids);
            for (const auto& sid : sids) {
                if (sid) {
                    out.push_back(sid.value());
                }
            }
        }
    };

    auto allSidsResolve = [](const Data::ElementMapPtr& map,
                             const App::StringHasherRef& target,
                             auto&& self) -> bool {
        if (!map) {
            return true;
        }
        if (static_cast<App::StringHasher*>(map->hasher)
            != static_cast<App::StringHasher*>(target)) {
            return false;
        }
        for (const auto& child : map->getChildElements()) {
            for (const auto& sid : child.sids) {
                if (!sid || !sid.isFromSameHasher(target) || !target->getID(sid.value())) {
                    return false;
                }
            }
            if (!self(child.elementMap, target, self)) {
                return false;
            }
        }
        for (const auto& el : map->getAll()) {
            Data::ElementIDRefs sids;
            (void)map->find(el.index, &sids);
            for (const auto& sid : sids) {
                if (!sid || !sid.isFromSameHasher(target) || !target->getID(sid.value())) {
                    return false;
                }
            }
        }
        return true;
    };

    try {
        // Match writeArchive: assign archive IDs then force-mark every referenced SID.
        if (sourceHasher) {
            sourceHasher->clearMarks();
        }
        Data::ElementMapArchiveContext saveCtx;
        saveCtx.hasher = sourceHasher;
        bundle.elementMap->beforeSave(saveCtx);
        forceMark(bundle.elementMap, forceMark);

        std::vector<long> requiredSids;
        collectSidValues(bundle.elementMap, collectSidValues, requiredSids);
        for (long id : requiredSids) {
            if (!hasher->getID(id)) {
                restoreSourceState();
                return false;
            }
        }

        std::ostringstream mapStream(std::ios::binary);
        bundle.elementMap->save(mapStream);
        restoreSourceState();

        Data::ElementMapArchiveContext restoreCtx;
        restoreCtx.hasher = hasher;
        std::istringstream mapIn(mapStream.str(), std::ios::binary);
        auto restored = std::make_shared<Data::ElementMap>();
        auto rebound = restored->restore(restoreCtx, mapIn);
        if (!rebound || !allSidsResolve(rebound, hasher, allSidsResolve)) {
            restoreSourceState();
            return false;
        }
        // Publish only after validation succeeds.
        bundle.elementMap = rebound;
        bundle.hasher = hasher;
        bundle.shape.Hasher = hasher;
        bundle.shape.resetElementMap(bundle.elementMap);
        return true;
    }
    catch (...) {
        restoreSourceState();
        return false;
    }
}

bool TopoShapeArchive::materializeOperandsOntoSharedHasher(const FrozenTopoShapeBundle& baseIn,
                                                           const FrozenTopoShapeBundle& toolIn,
                                                           App::StringHasherRef workerHasher,
                                                           FrozenTopoShapeBundle& baseOut,
                                                           FrozenTopoShapeBundle& toolOut,
                                                           std::string& errorCode,
                                                           std::string& errorMessage)
{
    errorCode.clear();
    errorMessage.clear();
    if (!workerHasher) {
        errorCode = "NullHasher";
        errorMessage = "Shared worker hasher is null";
        return false;
    }
    if (!baseIn.valid || !toolIn.valid) {
        errorCode = "InvalidOperand";
        errorMessage = "Boolean operand bundle is invalid";
        return false;
    }

    auto matBase = workerHasher->materializeExactClosure(baseIn.hasherSnapshot);
    if (!matBase.success) {
        errorCode = matBase.errorCode.empty() ? "BaseHasherMaterializeFailed" : matBase.errorCode;
        errorMessage = matBase.errorMessage.empty() ? "Failed to materialize base hasher closure"
                                                    : matBase.errorMessage;
        return false;
    }
    auto mergeTool = workerHasher->mergeExactClosure(toolIn.hasherSnapshot,
                                                     /*expectedRevision=*/0);
    if (!mergeTool.success) {
        errorCode = mergeTool.errorCode.empty() ? "ToolHasherMergeFailed" : mergeTool.errorCode;
        errorMessage = mergeTool.errorMessage.empty()
            ? "Failed to merge tool hasher closure onto shared worker hasher"
            : mergeTool.errorMessage;
        return false;
    }

    baseOut = baseIn;
    toolOut = toolIn;
    if (!rebindBundleToHasher(baseOut, workerHasher)) {
        errorCode = "BaseRebindFailed";
        errorMessage = "Failed to rebind base ElementMap onto shared worker hasher";
        return false;
    }
    if (!rebindBundleToHasher(toolOut, workerHasher)) {
        errorCode = "ToolRebindFailed";
        errorMessage = "Failed to rebind tool ElementMap onto shared worker hasher";
        return false;
    }
    return true;
}

HasherDeltaMergeResult TopoShapeArchive::mergeHasherDelta(App::StringHasherRef hasher,
                                                          const StringHasherSnapshot& delta,
                                                          uint64_t expectedRevision)
{
    HasherDeltaMergeResult result;
    if (!hasher) {
        result.errorCode = "NullHasher";
        result.errorMessage = "Canonical document hasher is null";
        return result;
    }

    auto merged = hasher->mergeExactClosure(delta, expectedRevision);
    result.success = merged.success;
    result.errorCode = merged.errorCode;
    result.errorMessage = merged.errorMessage;
    result.appendedCount = merged.appendedCount;
    return result;
}

HasherDeltaMergeResult TopoShapeArchive::commitHasherDelta(App::StringHasherRef hasher,
                                                           const StringHasherSnapshot& delta)
{
    HasherDeltaMergeResult result = mergeHasherDelta(hasher, delta, /*expectedRevision=*/0);
    if (result.success) {
        hasher->advanceRevision();
    }
    return result;
}

} // namespace Part
