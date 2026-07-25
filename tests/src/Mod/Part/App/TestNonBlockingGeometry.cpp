// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <Mod/Part/App/TopoShapeArchive.h>
#include <Mod/Part/App/BooleanGeometryOperation.h>
#include <Mod/Part/App/FilletGeometryOperation.h>
#include <Mod/Part/App/SweepGeometryOperation.h>
#include <Mod/Part/App/GeometryWorkerRegistry.h>
#include <App/ElementMap.h>
#include <App/GeometryJobManager.h>
#include <App/IndexedName.h>
#include <App/MappedName.h>
#include <App/StringHasher.h>
#include <Base/Exception.h>
#include <src/App/InitApplication.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <gp_Pnt.hxx>
#include <QDir>
#include <QTemporaryDir>
#include <QByteArray>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>

class NonBlockingGeometryTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(_tempDir->isValid());
    }

    std::unique_ptr<QTemporaryDir> _tempDir;
};

TEST_F(NonBlockingGeometryTest, TopoShapeArchiveWriteReadRoundTrip)
{
    BRepPrimAPI_MakeBox mkBox(10.0, 20.0, 30.0);
    Part::TopoShape boxShape(mkBox.Shape());
    boxShape.Tag = 12345;

    Part::FrozenTopoShapeBundle inBundle = Part::TopoShapeArchive::createBundle(boxShape);

    std::string archivePath = (_tempDir->path() + "/test_box.fcg").toStdString();

    bool writeSuccess = Part::TopoShapeArchive::writeArchive(inBundle, archivePath);
    EXPECT_TRUE(writeSuccess);

    Part::FrozenTopoShapeBundle outBundle;
    bool readSuccess = Part::TopoShapeArchive::readArchive(archivePath, outBundle);
    EXPECT_TRUE(readSuccess);

    EXPECT_EQ(outBundle.shapeTag, 12345);
    EXPECT_FALSE(outBundle.shape.isNull());
    EXPECT_EQ(outBundle.shape.getShape().ShapeType(), TopAbs_SOLID);
}

TEST_F(NonBlockingGeometryTest, RepeatWriteIsByteIdenticalAndDoesNotMutateBundle)
{
    App::StringHasherRef hasher(new App::StringHasher);
    hasher->advanceRevision();

    BRepPrimAPI_MakeBox mkBox(10.0, 20.0, 30.0);
    Part::TopoShape boxShape(mkBox.Shape(), /*tag=*/99, hasher);
    auto map = std::make_shared<Data::ElementMap>();
    map->hasher = hasher;
    boxShape.resetElementMap(map);

    Data::IndexedName face1("Face", 1);
    App::StringIDRef sid = hasher->getID(QByteArray("RepeatFace"));
    ASSERT_TRUE(sid);
    Data::MappedName mapped(sid);
    Data::ElementIDRefs sids;
    sids.push_back(sid);
    map->setElementName(face1, mapped, boxShape.Tag, &sids);
    sid.mark();

    Part::FrozenTopoShapeBundle bundle = Part::TopoShapeArchive::createBundle(boxShape);
    ASSERT_TRUE(bundle.valid);

    const std::string path1 = (_tempDir->path() + "/repeat1.fcg").toStdString();
    const std::string path2 = (_tempDir->path() + "/repeat2.fcg").toStdString();
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(bundle, path1));
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(bundle, path2));

    auto readAll = [](const std::string& path) {
        std::ifstream ifs(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    };
    EXPECT_EQ(readAll(path1), readAll(path2));
    EXPECT_TRUE(bundle.valid);
    EXPECT_NE(bundle.elementMap.get(), nullptr);
}

TEST_F(NonBlockingGeometryTest, BuiltinWorkerRegistryRegistersBooleanFilletSweep)
{
    Part::GeometryWorkerRegistry::instance().registerBuiltins();
    EXPECT_TRUE(Part::GeometryWorkerRegistry::instance().isOperationAllowed("Part::Boolean"));
    EXPECT_TRUE(Part::GeometryWorkerRegistry::instance().isOperationAllowed("Part::Fillet"));
    EXPECT_TRUE(Part::GeometryWorkerRegistry::instance().isOperationAllowed("Part::Sweep"));
    EXPECT_NE(Part::GeometryWorkerRegistry::instance().createTask("Part::Boolean"), nullptr);
    EXPECT_FALSE(Part::GeometryWorkerRegistry::instance().isOperationAllowed("Part::NotARealOp"));
}

TEST_F(NonBlockingGeometryTest, ChecksumMismatchRejection)
{
    BRepPrimAPI_MakeCylinder mkCyl(5.0, 15.0);
    Part::TopoShape cylShape(mkCyl.Shape());

    Part::FrozenTopoShapeBundle inBundle = Part::TopoShapeArchive::createBundle(cylShape);
    std::string archivePath = (_tempDir->path() + "/corrupt.fcg").toStdString();

    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(inBundle, archivePath));

    // Corrupt one byte in the middle of the archive
    std::fstream fs(archivePath, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(fs.is_open());
    fs.seekp(20);
    char corruptByte = 0xFF;
    fs.write(&corruptByte, 1);
    fs.close();

    Part::FrozenTopoShapeBundle outBundle;
    bool readSuccess = Part::TopoShapeArchive::readArchive(archivePath, outBundle);
    // Checksum verification must reject corrupted archive payload
    EXPECT_FALSE(readSuccess);
}

class TestWorkerContext : public App::GeometryWorkerContext
{
public:
    explicit TestWorkerContext(const std::string& tempDir)
        : _tempDir(tempDir)
    {
    }

    void reportProgress(double fraction, const std::string& phase = "") override
    {
        _lastFraction = fraction;
        _lastPhase = phase;
    }

    bool isCancelled() const override
    {
        return false;
    }

    std::chrono::steady_clock::time_point deadline() const override
    {
        return std::chrono::steady_clock::now() + std::chrono::minutes(1);
    }

    std::string tempDir() const override
    {
        return _tempDir;
    }

    double _lastFraction {0.0};
    std::string _lastPhase;
    std::string _tempDir;
};

TEST_F(NonBlockingGeometryTest, Sha256DigestIsHex64)
{
    std::vector<uint8_t> data = {'a', 'b', 'c'};
    std::string digest = Part::TopoShapeArchive::calculateSha256(data);
    EXPECT_EQ(digest.size(), 64u);
    EXPECT_TRUE(std::all_of(digest.begin(), digest.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }));
}

TEST_F(NonBlockingGeometryTest, HasherDeltaRevisionMismatchRejected)
{
    App::StringHasherRef hasher(new App::StringHasher);
    auto sid = hasher->getID(QByteArray("Face001"));
    ASSERT_TRUE(sid);
    hasher->advanceRevision();
    hasher->advanceRevision(); // revision = 2

    Part::StringHasherSnapshot delta = hasher->captureClosure(/*markedOnly=*/false);
    delta.revision = 2;

    auto merged = Part::TopoShapeArchive::mergeHasherDelta(hasher, delta, /*expectedRevision=*/1);
    EXPECT_FALSE(merged.success);
    EXPECT_EQ(merged.errorCode, "RevisionMismatch");
}

TEST_F(NonBlockingGeometryTest, StaleHasherRevisionRejectedAfterAdvance)
{
    App::StringHasherRef hasher(new App::StringHasher);
    auto sid = hasher->getID(QByteArray("StaleFace"));
    ASSERT_TRUE(sid);
    hasher->advanceRevision(); // revision = 1

    Part::StringHasherSnapshot delta = hasher->captureClosure(/*markedOnly=*/false);
    delta.revision = hasher->getRevision();
    ASSERT_EQ(delta.revision, 1u);

    // Document-side commit advances the hasher; a pre-commit snapshot must not merge.
    hasher->advanceRevision(); // revision = 2
    auto stale = Part::TopoShapeArchive::mergeHasherDelta(hasher, delta, /*expectedRevision=*/0);
    EXPECT_FALSE(stale.success) << "stale revision must not merge after hasher advance";
    EXPECT_EQ(stale.errorCode, "RevisionMismatch");

    // Matching current revision still merges, then commitHasherDelta advances again.
    Part::StringHasherSnapshot fresh = hasher->captureClosure(/*markedOnly=*/false);
    fresh.revision = hasher->getRevision();
    const uint64_t before = hasher->getRevision();
    auto committed = Part::TopoShapeArchive::commitHasherDelta(hasher, fresh);
    EXPECT_TRUE(committed.success);
    EXPECT_EQ(hasher->getRevision(), before + 1);

    // Re-applying the same snapshot against the advanced hasher must fail.
    auto again = Part::TopoShapeArchive::commitHasherDelta(hasher, fresh);
    EXPECT_FALSE(again.success);
    EXPECT_EQ(again.errorCode, "RevisionMismatch");
}

TEST_F(NonBlockingGeometryTest, ElementMapSurvivesArchiveRoundTrip)
{
    App::StringHasherRef hasher(new App::StringHasher);
    hasher->advanceRevision();

    BRepPrimAPI_MakeBox mkBox(10.0, 20.0, 30.0);
    Part::TopoShape boxShape(mkBox.Shape(), /*tag=*/42, hasher);
    auto map = std::make_shared<Data::ElementMap>();
    map->hasher = hasher;
    boxShape.resetElementMap(map);

    Data::IndexedName face1("Face", 1);
    App::StringIDRef sid = hasher->getID(QByteArray("MyFaceA"));
    ASSERT_TRUE(sid);
    Data::MappedName mapped(sid);
    Data::ElementIDRefs sids;
    sids.push_back(sid);
    map->setElementName(face1, mapped, boxShape.Tag, &sids);
    sid.mark();
    ASSERT_TRUE(sid.isMarked());

    Part::FrozenTopoShapeBundle inBundle = Part::TopoShapeArchive::createBundle(boxShape);
    EXPECT_TRUE(inBundle.valid);
    EXPECT_TRUE(inBundle.elementMap);
    EXPECT_TRUE(inBundle.hasher);
    EXPECT_FALSE(inBundle.hasherSnapshot.entries.empty());
    EXPECT_EQ(inBundle.hasherSnapshot.revision, 1u);
    EXPECT_FALSE(inBundle.mappedElements.empty());

    // Freeze must restore live hasher marks (clearMarks + beforeSave are temporary).
    EXPECT_TRUE(sid.isMarked());

    // Source map must not have been stolen/nulled by createBundle.
    EXPECT_GE(boxShape.getElementMapSize(false), 1u);

    std::string archivePath = (_tempDir->path() + "/mapped_box.fcg").toStdString();
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(inBundle, archivePath));

    Part::FrozenTopoShapeBundle outBundle;
    ASSERT_TRUE(Part::TopoShapeArchive::readArchive(archivePath, outBundle));
    EXPECT_EQ(outBundle.shapeTag, 42);
    ASSERT_TRUE(outBundle.elementMap);
    ASSERT_TRUE(outBundle.hasher);
    EXPECT_EQ(outBundle.shape.Hasher, outBundle.hasher);
    EXPECT_GE(outBundle.shape.getElementMapSize(false), 1u);
    EXPECT_GE(outBundle.elementMap->size(), 1u);

    auto found = outBundle.elementMap->find(face1);
    EXPECT_TRUE(found);
    EXPECT_EQ(found.toString(), mapped.toString());
}

TEST_F(NonBlockingGeometryTest, NestedAndSharedChildMapsSurviveArchiveRoundTrip)
{
    // Child ElementMaps (count >= 5) stay as nested maps instead of being expanded.
    // Two parent entries share one leaf pointer; a third entry uses a distinct leaf.
    App::StringHasherRef hasher(new App::StringHasher);
    hasher->advanceRevision();

    auto makeLeaf = [&](const char* label, long tag) -> Data::ElementMapPtr {
        auto leaf = std::make_shared<Data::ElementMap>();
        leaf->hasher = hasher;
        for (int i = 1; i <= 6; ++i) {
            Data::IndexedName edge("Edge", i);
            App::StringIDRef sid =
                hasher->getID(QByteArray(label) + QByteArray::number(i));
            EXPECT_TRUE(sid);
            if (!sid) {
                return {};
            }
            Data::MappedName mapped(sid);
            Data::ElementIDRefs sids;
            sids.push_back(sid);
            leaf->setElementName(edge, mapped, tag, &sids);
            sid.mark();
        }
        return leaf;
    };

    auto sharedLeaf = makeLeaf("SharedEdge", /*tag=*/11);
    auto otherLeaf = makeLeaf("OtherEdge", /*tag=*/12);
    ASSERT_TRUE(sharedLeaf);
    ASSERT_TRUE(otherLeaf);

    auto parent = std::make_shared<Data::ElementMap>();
    parent->hasher = hasher;

    Data::ElementIDRefs emptySids;
    std::vector<Data::ElementMap::MappedChildElements> children = {
        {Data::IndexedName("Face", 1),
         /*count=*/6,
         /*offset=*/0,
         /*tag=*/11,
         sharedLeaf,
         QByteArray("sharedAxxxx"),
         emptySids},
        {Data::IndexedName("Face", 7),
         6,
         0,
         11,
         sharedLeaf,
         QByteArray("sharedBxxxx"),
         emptySids},
        {Data::IndexedName("Face", 13),
         6,
         0,
         12,
         otherLeaf,
         QByteArray("otherCxxxxx"),
         emptySids},
    };
    parent->addChildElements(/*masterTag=*/30, children);
    ASSERT_TRUE(parent->hasChildElementMap());

    auto beforeChildren = parent->getChildElements();
    ASSERT_GE(beforeChildren.size(), 2u);
    int sharedRefsBefore = 0;
    for (const auto& child : beforeChildren) {
        if (child.elementMap && child.elementMap.get() == sharedLeaf.get()) {
            ++sharedRefsBefore;
        }
    }
    ASSERT_GE(sharedRefsBefore, 2)
        << "test setup must share one child ElementMap across parent entries";

    BRepPrimAPI_MakeBox mkBox(10.0, 20.0, 30.0);
    Part::TopoShape boxShape(mkBox.Shape(), /*tag=*/30, hasher);
    boxShape.resetElementMap(parent);

    Part::FrozenTopoShapeBundle inBundle = Part::TopoShapeArchive::createBundle(boxShape);
    ASSERT_TRUE(inBundle.valid) << inBundle.errorCode;
    ASSERT_TRUE(inBundle.elementMap);
    EXPECT_TRUE(inBundle.elementMap->hasChildElementMap())
        << "createBundle must retain nested child ElementMaps";

    auto midChildren = inBundle.elementMap->getChildElements();
    ASSERT_GE(midChildren.size(), 2u);

    Data::ElementMap* firstShared = nullptr;
    int sharedCloneRefs = 0;
    int otherCloneRefs = 0;
    for (const auto& child : midChildren) {
        ASSERT_TRUE(child.elementMap);
        Data::ElementIDRefs sids;
        auto name = child.elementMap->find(Data::IndexedName("Edge", 1), &sids);
        ASSERT_TRUE(name);
        ASSERT_FALSE(sids.empty());
        const std::string text = sids.front().dataToText();
        if (text.find("SharedEdge") != std::string::npos) {
            if (!firstShared) {
                firstShared = child.elementMap.get();
            }
            if (child.elementMap.get() == firstShared) {
                ++sharedCloneRefs;
            }
            else {
                ADD_FAILURE() << "shared leaf was duplicated during createBundle clone";
            }
        }
        else if (text.find("OtherEdge") != std::string::npos) {
            ++otherCloneRefs;
        }
    }
    EXPECT_GE(sharedCloneRefs, 2)
        << "createBundle must preserve shared child ElementMap identity";
    EXPECT_GE(otherCloneRefs, 1);

    const std::string archivePath = (_tempDir->path() + "/child_maps.fcg").toStdString();
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(inBundle, archivePath));

    Part::FrozenTopoShapeBundle outBundle;
    ASSERT_TRUE(Part::TopoShapeArchive::readArchive(archivePath, outBundle));
    ASSERT_TRUE(outBundle.elementMap);
    EXPECT_TRUE(outBundle.elementMap->hasChildElementMap())
        << "readArchive must not drop child ElementMaps";

    auto outChildren = outBundle.elementMap->getChildElements();
    EXPECT_EQ(outChildren.size(), midChildren.size());

    Data::ElementMap* outShared = nullptr;
    int outSharedRefs = 0;
    int outOtherRefs = 0;
    for (const auto& child : outChildren) {
        ASSERT_TRUE(child.elementMap) << "child map pointer must be restored";
        Data::ElementIDRefs sids;
        auto name = child.elementMap->find(Data::IndexedName("Edge", 1), &sids);
        ASSERT_TRUE(name);
        ASSERT_FALSE(sids.empty());
        const std::string text = sids.front().dataToText();
        if (text.find("SharedEdge") != std::string::npos) {
            if (!outShared) {
                outShared = child.elementMap.get();
            }
            EXPECT_EQ(child.elementMap.get(), outShared)
                << "shared child map must remain a single instance after reopen";
            if (child.elementMap.get() == outShared) {
                ++outSharedRefs;
            }
            EXPECT_EQ(child.elementMap->find(Data::IndexedName("Edge", 6), &sids).toString().empty(),
                      false);
            EXPECT_NE(sids.front().dataToText().find("SharedEdge"), std::string::npos);
        }
        else if (text.find("OtherEdge") != std::string::npos) {
            ++outOtherRefs;
        }
    }
    EXPECT_GE(outSharedRefs, 2);
    EXPECT_GE(outOtherRefs, 1);
}

TEST_F(NonBlockingGeometryTest, LongHashedNameSurvivesArchiveRoundTrip)
{
    App::StringHasherRef hasher(new App::StringHasher);
    hasher->setThreshold(8);
    hasher->advanceRevision();

    const QByteArray longName(
        "FaceWithAVeryLongTopologicalNameThatExceedsTheHasherThresholdAndMustRoundTrip");
    ASSERT_GT(longName.size(), hasher->getThreshold());

    BRepPrimAPI_MakeBox mkBox(5.0, 5.0, 5.0);
    Part::TopoShape boxShape(mkBox.Shape(), /*tag=*/77, hasher);
    auto map = std::make_shared<Data::ElementMap>();
    map->hasher = hasher;
    boxShape.resetElementMap(map);

    Data::IndexedName face1("Face", 1);
    App::StringIDRef sid =
        hasher->getID(longName, App::StringHasher::Option::Hashable);
    ASSERT_TRUE(sid);
    ASSERT_TRUE(sid.isHashed());
    const std::string hashedDigest = sid.dataToText();  // base64(SHA-1)
    // MappedName must carry the #id token, not raw hash bytes (those are illegal in names).
    Data::MappedName mapped(QByteArray::fromStdString(sid.toString()));
    Data::ElementIDRefs sids;
    sids.push_back(sid);
    map->setElementName(face1, mapped, boxShape.Tag, &sids);
    sid.mark();

    Part::FrozenTopoShapeBundle inBundle = Part::TopoShapeArchive::createBundle(boxShape);
    ASSERT_TRUE(inBundle.valid) << inBundle.errorCode;
    ASSERT_TRUE(inBundle.elementMap);
    ASSERT_FALSE(inBundle.hasherSnapshot.entries.empty());

    const std::string archivePath = (_tempDir->path() + "/long_name.fcg").toStdString();
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(inBundle, archivePath));

    Part::FrozenTopoShapeBundle outBundle;
    ASSERT_TRUE(Part::TopoShapeArchive::readArchive(archivePath, outBundle));
    ASSERT_TRUE(outBundle.elementMap);
    ASSERT_TRUE(outBundle.hasher);

    Data::ElementIDRefs outSids;
    auto found = outBundle.elementMap->find(face1, &outSids);
    ASSERT_TRUE(found);
    ASSERT_FALSE(outSids.empty());
    EXPECT_TRUE(outSids.front().isHashed());
    EXPECT_EQ(outSids.front().dataToText(), hashedDigest);

    // Re-hashing the same plaintext against the restored hasher must resolve the same id.
    auto again = outBundle.hasher->getID(longName, App::StringHasher::Option::Hashable);
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value(), outSids.front().value());
    EXPECT_EQ(again.dataToText(), hashedDigest);
}

TEST_F(NonBlockingGeometryTest, HasherDeltaAppendsMissingIds)
{
    App::StringHasherRef worker(new App::StringHasher);
    auto sidA = worker->getID(QByteArray("EdgeAlpha"));
    auto sidB = worker->getID(QByteArray("EdgeBeta"));
    ASSERT_TRUE(sidA);
    ASSERT_TRUE(sidB);
    worker->setRevision(1);

    Part::StringHasherSnapshot delta = worker->captureClosure(/*markedOnly=*/false);
    delta.revision = 1;
    ASSERT_GE(delta.entries.size(), 2u);

    App::StringHasherRef canonical(new App::StringHasher);
    // Seed only the first entry so the second must be appended.
    App::StringHasherClosure seed;
    seed.revision = 1;
    seed.entries.push_back(delta.entries.front());
    ASSERT_TRUE(canonical->mergeExactClosure(seed, 1).success);
    EXPECT_TRUE(canonical->getID(delta.entries.front().id));
    EXPECT_FALSE(canonical->getID(delta.entries.back().id));

    auto merged = Part::TopoShapeArchive::mergeHasherDelta(canonical, delta, /*expectedRevision=*/1);
    EXPECT_TRUE(merged.success);
    EXPECT_GE(merged.appendedCount, 1u);
    EXPECT_TRUE(canonical->getID(sidA.value()));
    EXPECT_TRUE(canonical->getID(sidB.value()));
    EXPECT_EQ(canonical->getID(sidB.value()).dataToText(), std::string("EdgeBeta"));

    auto idempotent = Part::TopoShapeArchive::mergeHasherDelta(canonical, delta, 1);
    EXPECT_TRUE(idempotent.success);
    EXPECT_EQ(idempotent.appendedCount, 0u);
}

TEST_F(NonBlockingGeometryTest, HasherDeltaCollisionRejected)
{
    App::StringHasherRef canonical(new App::StringHasher);
    auto existing = canonical->getID(QByteArray("SameIdDifferentText"));
    ASSERT_TRUE(existing);

    Part::StringHasherSnapshot delta;
    delta.revision = 0;
    delta.entries.push_back(
        {existing.value(), 0u, QByteArray("OtherText"), QByteArray(), {}});

    auto merged = Part::TopoShapeArchive::mergeHasherDelta(canonical, delta, 0);
    EXPECT_FALSE(merged.success);
    EXPECT_EQ(merged.errorCode, "IdCollision");
}

TEST_F(NonBlockingGeometryTest, HasherDeltaCollisionLeavesPriorAppendsUncommitted)
{
    App::StringHasherRef canonical(new App::StringHasher);
    auto keep = canonical->getID(QByteArray("KeepMe"));
    ASSERT_TRUE(keep);
    const size_t sizeBefore = canonical->size();

    // First entry is new and would append; second collides with KeepMe's id.
    Part::StringHasherSnapshot delta;
    delta.revision = 0;
    delta.entries.push_back(
        {keep.value() + 10, 0u, QByteArray("WouldAppend"), QByteArray(), {}});
    delta.entries.push_back(
        {keep.value(), 0u, QByteArray("CollisionText"), QByteArray(), {}});

    auto merged = Part::TopoShapeArchive::mergeHasherDelta(canonical, delta, 0);
    EXPECT_FALSE(merged.success);
    EXPECT_EQ(merged.errorCode, "IdCollision");
    EXPECT_EQ(merged.appendedCount, 0u);
    EXPECT_EQ(canonical->size(), sizeBefore);
    EXPECT_FALSE(canonical->getID(keep.value() + 10));
}

TEST_F(NonBlockingGeometryTest, HasherHighWaterGapReservesFutureIds)
{
    App::StringHasherRef hasher(new App::StringHasher);
    App::StringHasherClosure closure;
    closure.revision = 0;
    closure.highWaterId = 100;
    // Transport only a sparse ID; authenticated high-water is far above it.
    closure.entries.push_back({5, 0u, QByteArray("SparseFive"), QByteArray(), {}});

    auto merged = hasher->mergeExactClosure(closure, 0);
    ASSERT_TRUE(merged.success);
    EXPECT_EQ(merged.appendedCount, 1u);
    EXPECT_EQ(hasher->getLastID(), 100);

    auto next = hasher->getID(QByteArray("AfterHighWater"));
    ASSERT_TRUE(next);
    EXPECT_GT(next.value(), 100);
    EXPECT_FALSE(hasher->getID(50));
}

TEST_F(NonBlockingGeometryTest, HasherRejectsIdAboveHighWater)
{
    App::StringHasherRef hasher(new App::StringHasher);
    const size_t sizeBefore = hasher->size();

    App::StringHasherClosure closure;
    closure.highWaterId = 10;
    closure.entries.push_back({11, 0u, QByteArray("TooHigh"), QByteArray(), {}});

    auto merged = hasher->mergeExactClosure(closure, 0);
    EXPECT_FALSE(merged.success);
    EXPECT_EQ(merged.errorCode, "IdAboveHighWater");
    EXPECT_EQ(merged.appendedCount, 0u);
    EXPECT_EQ(hasher->size(), sizeBefore);
    EXPECT_EQ(hasher->getLastID(), 0);
}

TEST_F(NonBlockingGeometryTest, HasherDuplicateIdsLeaveCanonicalUnchanged)
{
    App::StringHasherRef canonical(new App::StringHasher);
    auto keep = canonical->getID(QByteArray("KeepOriginal"));
    ASSERT_TRUE(keep);
    const size_t sizeBefore = canonical->size();
    const long lastBefore = canonical->getLastID();

    App::StringHasherClosure closure;
    closure.revision = 0;
    // Duplicate IDs in one closure, even with identical payload, must not mutate.
    closure.entries.push_back(
        {keep.value() + 20, 0u, QByteArray("WouldAppend"), QByteArray(), {}});
    closure.entries.push_back(
        {keep.value() + 20, 0u, QByteArray("WouldAppend"), QByteArray(), {}});

    auto merged = canonical->mergeExactClosure(closure, 0);
    EXPECT_FALSE(merged.success);
    EXPECT_EQ(merged.errorCode, "DuplicateId");
    EXPECT_EQ(merged.appendedCount, 0u);
    EXPECT_EQ(canonical->size(), sizeBefore);
    EXPECT_EQ(canonical->getLastID(), lastBefore);
    EXPECT_FALSE(canonical->getID(keep.value() + 20));
}

TEST_F(NonBlockingGeometryTest, OversizedSectionRejected)
{
    // Craft a minimal corrupt archive claiming an enormous shape section.
    std::string archivePath = (_tempDir->path() + "/oversized.fcg").toStdString();
    {
        std::ofstream ofs(archivePath, std::ios::binary);
        ASSERT_TRUE(ofs.is_open());
        ofs.write("FCG1", 4);
        uint32_t version = 3;
        ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));
        long tag = 1;
        ofs.write(reinterpret_cast<const char*>(&tag), sizeof(tag));
        uint32_t huge = Part::TopoShapeArchive::MaxSectionBytes + 1;
        ofs.write(reinterpret_cast<const char*>(&huge), sizeof(huge));
    }

    Part::FrozenTopoShapeBundle outBundle;
    EXPECT_FALSE(Part::TopoShapeArchive::readArchive(archivePath, outBundle));
}

TEST_F(NonBlockingGeometryTest, BooleanDifferentOperandsHaveDistinctDigests)
{
    BRepPrimAPI_MakeBox boxA(10.0, 10.0, 10.0);
    BRepPrimAPI_MakeBox boxB(gp_Pnt(5, 5, 5), 10.0, 10.0, 10.0);
    BRepPrimAPI_MakeBox boxC(20.0, 20.0, 20.0);

    Part::FrozenTopoShapeBundle a = Part::TopoShapeArchive::createBundle(Part::TopoShape(boxA.Shape()));
    Part::FrozenTopoShapeBundle b = Part::TopoShapeArchive::createBundle(Part::TopoShape(boxB.Shape()));
    Part::FrozenTopoShapeBundle c = Part::TopoShapeArchive::createBundle(Part::TopoShape(boxC.Shape()));

    Part::BooleanGeometryOperation fuseAB(Part::BooleanType::Fuse, a, b);
    Part::BooleanGeometryOperation fuseAC(Part::BooleanType::Fuse, a, c);

    EXPECT_FALSE(fuseAB.parameterDigest().empty());
    EXPECT_FALSE(fuseAC.parameterDigest().empty());
    EXPECT_NE(fuseAB.parameterDigest(), fuseAC.parameterDigest());
    EXPECT_EQ(fuseAB.parameterDigest(), fuseAB.parameterDigest());
}

TEST_F(NonBlockingGeometryTest, BooleanDifferentOperandsDoNotJoinUnderSingleInstance)
{
    BRepPrimAPI_MakeBox boxA(10.0, 10.0, 10.0);
    BRepPrimAPI_MakeBox boxB(gp_Pnt(5, 5, 5), 10.0, 10.0, 10.0);
    BRepPrimAPI_MakeBox boxC(20.0, 20.0, 20.0);

    Part::FrozenTopoShapeBundle a = Part::TopoShapeArchive::createBundle(Part::TopoShape(boxA.Shape()));
    Part::FrozenTopoShapeBundle b = Part::TopoShapeArchive::createBundle(Part::TopoShape(boxB.Shape()));
    Part::FrozenTopoShapeBundle c = Part::TopoShapeArchive::createBundle(Part::TopoShape(boxC.Shape()));

    auto taskAB = std::make_shared<Part::BooleanGeometryOperation>(Part::BooleanType::Fuse, a, b);
    auto taskAC = std::make_shared<Part::BooleanGeometryOperation>(Part::BooleanType::Fuse, a, c);

    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 960;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 42;
    spec.key.documentIncarnation = 960;
    spec.key.targetObjectId = 42;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    spec.coalescing = App::CoalesceMode::SingleInstance;
    spec.task = taskAB;

    auto handleAB = App::GeometryJobManager::instance().submit(spec);
    int cancelled = 0;
    App::GeometryJobManager::instance().registerCallback(
        handleAB.id(),
        [&](App::GeometryJobId, App::GeometryJobState state, const App::DetachedGeometryResult&) {
            if (state == App::GeometryJobState::Cancelled) {
                ++cancelled;
            }
        });

    spec.task = taskAC;
    auto handleAC = App::GeometryJobManager::instance().submit(spec);
    EXPECT_NE(handleAB.id(), handleAC.id());
    EXPECT_EQ(cancelled, 1);
    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handleAB.id()),
              App::GeometryJobState::Cancelled);
}

TEST_F(NonBlockingGeometryTest, FilletDifferentRadiiHaveDistinctDigests)
{
    BRepPrimAPI_MakeBox box(10.0, 10.0, 10.0);
    Part::FrozenTopoShapeBundle base = Part::TopoShapeArchive::createBundle(Part::TopoShape(box.Shape()));

    Part::FilletGeometryOperation fillet1(base, {{0, 1.0, 1.0}});
    Part::FilletGeometryOperation fillet2(base, {{0, 2.0, 2.0}});

    EXPECT_FALSE(fillet1.parameterDigest().empty());
    EXPECT_NE(fillet1.parameterDigest(), fillet2.parameterDigest());
}

TEST_F(NonBlockingGeometryTest, BooleanFusePreservesMappedElementHistory)
{
    // Tagged operands are required for unambiguous OCC history encoding.
    BRepPrimAPI_MakeBox box1(10.0, 10.0, 10.0);
    BRepPrimAPI_MakeBox box2(gp_Pnt(5, 5, 5), 10.0, 10.0, 10.0);

    App::StringHasherRef hasher(new App::StringHasher);
    Part::TopoShape shape1(box1.Shape(), /*tag=*/1, hasher);
    Part::TopoShape shape2(box2.Shape(), /*tag=*/2, hasher);

    // Seed an explicit mapped name so we can prove history is not dropped to raw BREP.
    {
        auto map = std::make_shared<Data::ElementMap>();
        map->hasher = hasher;
        shape1.resetElementMap(map);
        Data::IndexedName face1("Face", 1);
        App::StringIDRef sid = hasher->getID(QByteArray("SeedFaceA"));
        ASSERT_TRUE(sid);
        Data::MappedName mapped(sid);
        Data::ElementIDRefs sids;
        sids.push_back(sid);
        map->setElementName(face1, mapped, shape1.Tag, &sids);
    }

    Part::FrozenTopoShapeBundle b1 = Part::TopoShapeArchive::createBundle(shape1);
    Part::FrozenTopoShapeBundle b2 = Part::TopoShapeArchive::createBundle(shape2);
    ASSERT_TRUE(b1.valid);
    ASSERT_TRUE(b1.elementMap);
    EXPECT_GE(b1.elementMap->size(), 1u);

    Part::BooleanGeometryOperation fuseOp(Part::BooleanType::Fuse, b1, b2);
    TestWorkerContext ctx(_tempDir->path().toStdString());
    App::DetachedGeometryResult result = fuseOp.run(ctx);

    ASSERT_TRUE(result.success) << result.errorCode << ": " << result.errorMessage;
    ASSERT_FALSE(result.resultArchivePath.empty());

    Part::FrozenTopoShapeBundle outBundle;
    ASSERT_TRUE(Part::TopoShapeArchive::readArchive(result.resultArchivePath, outBundle));
    EXPECT_FALSE(outBundle.shape.isNull());
    ASSERT_TRUE(outBundle.elementMap) << "mapped boolean must archive a non-null ElementMap";
    EXPECT_GT(outBundle.elementMap->size(), 0u);
    EXPECT_GE(outBundle.shape.getElementMapSize(false), 1u);
    // Fuse of overlapping boxes produces many named faces/edges/vertices when history is mapped.
    EXPECT_GE(outBundle.elementMap->size(), 10u);
    EXPECT_FALSE(outBundle.hasherSnapshot.entries.empty());
}

TEST_F(NonBlockingGeometryTest, FilletPreservesMappedElementHistory)
{
    BRepPrimAPI_MakeBox box(10.0, 10.0, 10.0);
    App::StringHasherRef hasher(new App::StringHasher);
    Part::TopoShape shape(box.Shape(), /*tag=*/1, hasher);

    Part::FrozenTopoShapeBundle base = Part::TopoShapeArchive::createBundle(shape);
    ASSERT_TRUE(base.valid);

    // Fillet a single edge with a modest radius (matches existing FilletOperation happy path).
    Part::FilletGeometryOperation filletOp(base, {{0, 1.0, 1.0}});
    TestWorkerContext ctx(_tempDir->path().toStdString());
    App::DetachedGeometryResult result = filletOp.run(ctx);

    ASSERT_TRUE(result.success) << result.errorCode << ": " << result.errorMessage;
    ASSERT_FALSE(result.resultArchivePath.empty());

    Part::FrozenTopoShapeBundle outBundle;
    ASSERT_TRUE(Part::TopoShapeArchive::readArchive(result.resultArchivePath, outBundle));
    EXPECT_FALSE(outBundle.shape.isNull());
    ASSERT_TRUE(outBundle.elementMap) << "mapped fillet must archive a non-null ElementMap";
    EXPECT_GT(outBundle.elementMap->size(), 0u);
    EXPECT_GE(outBundle.shape.getElementMapSize(false), 1u);
    EXPECT_GE(outBundle.elementMap->size(), 10u);
    EXPECT_FALSE(outBundle.hasherSnapshot.entries.empty());
}

TEST_F(NonBlockingGeometryTest, BooleanFuseOperation)
{
    BRepPrimAPI_MakeBox box1(10.0, 10.0, 10.0);
    BRepPrimAPI_MakeBox box2(gp_Pnt(5, 5, 5), 10.0, 10.0, 10.0);

    Part::TopoShape shape1(box1.Shape());
    Part::TopoShape shape2(box2.Shape());

    Part::FrozenTopoShapeBundle b1 = Part::TopoShapeArchive::createBundle(shape1);
    Part::FrozenTopoShapeBundle b2 = Part::TopoShapeArchive::createBundle(shape2);

    Part::BooleanGeometryOperation fuseOp(Part::BooleanType::Fuse, b1, b2);

    TestWorkerContext ctx(_tempDir->path().toStdString());
    App::DetachedGeometryResult result = fuseOp.run(ctx);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.resultArchivePath.empty());

    Part::FrozenTopoShapeBundle outBundle;
    EXPECT_TRUE(Part::TopoShapeArchive::readArchive(result.resultArchivePath, outBundle));
    EXPECT_FALSE(outBundle.shape.isNull());
}

TEST_F(NonBlockingGeometryTest, FilletOperation)
{
    BRepPrimAPI_MakeBox box(10.0, 10.0, 10.0);
    Part::TopoShape shape(box.Shape());
    Part::FrozenTopoShapeBundle b = Part::TopoShapeArchive::createBundle(shape);

    std::vector<Part::FilletEdgeSpec> edges;
    edges.push_back({0, 1.0, 1.0}); // Fillet edge 0 with radius 1.0

    Part::FilletGeometryOperation filletOp(b, edges);

    TestWorkerContext ctx(_tempDir->path().toStdString());
    App::DetachedGeometryResult result = filletOp.run(ctx);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.resultArchivePath.empty());

    Part::FrozenTopoShapeBundle outBundle;
    EXPECT_TRUE(Part::TopoShapeArchive::readArchive(result.resultArchivePath, outBundle));
    EXPECT_FALSE(outBundle.shape.isNull());
}

TEST_F(NonBlockingGeometryTest, SweepPreservesMappedElementHistory)
{
    // Vertical spine wire + rectangular face profile (same arrangement as Part::Sweep /
    // makeElementPipeShell production path: spine first, then profiles).
    auto spineEdge = BRepBuilderAPI_MakeEdge(gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(0.0, 0.0, 8.0)).Edge();
    auto spineWire = BRepBuilderAPI_MakeWire(spineEdge).Wire();

    auto e1 = BRepBuilderAPI_MakeEdge(gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(5.0, 0.0, 0.0)).Edge();
    auto e2 = BRepBuilderAPI_MakeEdge(gp_Pnt(5.0, 0.0, 0.0), gp_Pnt(5.0, 5.0, 0.0)).Edge();
    auto e3 = BRepBuilderAPI_MakeEdge(gp_Pnt(5.0, 5.0, 0.0), gp_Pnt(0.0, 5.0, 0.0)).Edge();
    auto e4 = BRepBuilderAPI_MakeEdge(gp_Pnt(0.0, 5.0, 0.0), gp_Pnt(0.0, 0.0, 0.0)).Edge();
    auto profileWire = BRepBuilderAPI_MakeWire(e1, e2, e3, e4).Wire();

    App::StringHasherRef hasher(new App::StringHasher);
    Part::TopoShape spineShape(spineWire, /*tag=*/1, hasher);
    Part::TopoShape profileShape(profileWire, /*tag=*/2, hasher);

    Part::FrozenTopoShapeBundle spineBundle = Part::TopoShapeArchive::createBundle(spineShape);
    Part::FrozenTopoShapeBundle profileBundle = Part::TopoShapeArchive::createBundle(profileShape);
    ASSERT_TRUE(spineBundle.valid);
    ASSERT_TRUE(profileBundle.valid);

    Part::SweepGeometryOperation sweepOp(spineBundle, {profileBundle}, /*isSolid=*/false);
    TestWorkerContext ctx(_tempDir->path().toStdString());
    App::DetachedGeometryResult result = sweepOp.run(ctx);

    ASSERT_TRUE(result.success) << result.errorCode << ": " << result.errorMessage;
    ASSERT_FALSE(result.resultArchivePath.empty());

    Part::FrozenTopoShapeBundle outBundle;
    ASSERT_TRUE(Part::TopoShapeArchive::readArchive(result.resultArchivePath, outBundle));
    EXPECT_FALSE(outBundle.shape.isNull());
    ASSERT_TRUE(outBundle.elementMap) << "mapped sweep must archive a non-null ElementMap";
    EXPECT_GT(outBundle.elementMap->size(), 0u);
    EXPECT_GE(outBundle.shape.getElementMapSize(false), 1u);
    EXPECT_GE(outBundle.elementMap->size(), 10u);
    EXPECT_FALSE(outBundle.hasherSnapshot.entries.empty());
}
