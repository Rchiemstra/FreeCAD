// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <Mod/Part/App/TopoShapeArchive.h>
#include <Mod/Part/App/BooleanGeometryOperation.h>
#include <Mod/Part/App/FilletGeometryOperation.h>
#include <Mod/Part/App/SweepGeometryOperation.h>
#include <Mod/Part/App/GeometryWorkerRegistry.h>
#include <Mod/Part/App/GeometryWorker.h>
#include <App/GeometryJob.h>
#include <App/ElementMap.h>
#include <App/GeometryJobManager.h>
#include <App/GeometryRequestWorkspace.h>
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
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QByteArray>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <unordered_map>

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

    // Rebind onto a fresh worker hasher must not mutate the source/bundle snapshots.
    const auto sourceMarksBefore = hasher->snapshotMarks();
    const auto bundleMarksBefore = inBundle.hasher->snapshotMarks();
    std::unordered_map<const Data::ElementMap*, unsigned> archiveIdsBefore;
    inBundle.elementMap->snapshotArchiveIds(archiveIdsBefore);
    const size_t sourceSizeBefore = hasher->size();
    const uint64_t sourceRevisionBefore = hasher->getRevision();
    const int sourceThresholdBefore = hasher->getThreshold();
    const long sourceHighWaterBefore = hasher->getLastID();

    App::StringHasherRef worker(new App::StringHasher);
    ASSERT_TRUE(worker->materializeExactClosure(inBundle.hasherSnapshot).success);
    Part::FrozenTopoShapeBundle rebound = inBundle;
    ASSERT_TRUE(Part::TopoShapeArchive::rebindBundleToHasher(rebound, worker));
    EXPECT_EQ(static_cast<App::StringHasher*>(rebound.hasher), static_cast<App::StringHasher*>(worker));

    auto reboundChildren = rebound.elementMap->getChildElements();
    EXPECT_EQ(reboundChildren.size(), midChildren.size());

    Data::ElementMap* reboundShared = nullptr;
    Data::ElementMap* reboundOther = nullptr;
    int reboundSharedRefs = 0;
    int reboundOtherRefs = 0;
    for (const auto& child : reboundChildren) {
        ASSERT_TRUE(child.elementMap) << "rebound child map pointer must be restored";
        Data::ElementIDRefs sids;
        auto name = child.elementMap->find(Data::IndexedName("Edge", 1), &sids);
        ASSERT_TRUE(name);
        ASSERT_FALSE(sids.empty());
        for (const auto& sidRef : sids) {
            EXPECT_TRUE(sidRef.isFromSameHasher(worker))
                << "rebound SID must reference the worker hasher";
            const App::StringIDRef resolved = worker->getID(sidRef.value());
            ASSERT_TRUE(resolved) << "rebound SID must resolve from worker hasher";
            EXPECT_EQ(resolved.value(), sidRef.value());
        }
        const std::string text = sids.front().dataToText();
        if (text.find("SharedEdge") != std::string::npos) {
            if (!reboundShared) {
                reboundShared = child.elementMap.get();
            }
            EXPECT_EQ(child.elementMap.get(), reboundShared)
                << "shared child map must remain a single instance after rebind";
            if (child.elementMap.get() == reboundShared) {
                ++reboundSharedRefs;
            }
            EXPECT_EQ(child.elementMap->find(Data::IndexedName("Edge", 6), &sids).toString().empty(),
                      false);
            EXPECT_NE(sids.front().dataToText().find("SharedEdge"), std::string::npos);
        }
        else if (text.find("OtherEdge") != std::string::npos) {
            if (!reboundOther) {
                reboundOther = child.elementMap.get();
            }
            ++reboundOtherRefs;
            EXPECT_NE(child.elementMap.get(), reboundShared)
                << "OtherEdge child must not alias SharedEdge map";
        }
    }
    ASSERT_NE(reboundShared, nullptr);
    ASSERT_NE(reboundOther, nullptr);
    EXPECT_NE(reboundShared, reboundOther);
    EXPECT_GE(reboundSharedRefs, 2);
    EXPECT_GE(reboundOtherRefs, 1);

    const std::string rebindArchive = (_tempDir->path() + "/child_maps_rebound.fcg").toStdString();
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(rebound, rebindArchive));
    ASSERT_TRUE(Part::TopoShapeArchive::rebindBundleToHasher(rebound, worker));
    const std::string rebindArchive2 = (_tempDir->path() + "/child_maps_rebound2.fcg").toStdString();
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(rebound, rebindArchive2));

    QFile reboundFile(QString::fromStdString(rebindArchive));
    ASSERT_TRUE(reboundFile.open(QIODevice::ReadOnly));
    const QByteArray reboundBytes = reboundFile.readAll();
    reboundFile.close();
    QFile reboundFile2(QString::fromStdString(rebindArchive2));
    ASSERT_TRUE(reboundFile2.open(QIODevice::ReadOnly));
    const QByteArray reboundBytes2 = reboundFile2.readAll();
    reboundFile2.close();
    EXPECT_EQ(reboundBytes, reboundBytes2)
        << "repeat archive write after rebind must be byte-identical";

    EXPECT_EQ(inBundle.hasher->snapshotMarks(), bundleMarksBefore);
    EXPECT_EQ(hasher->snapshotMarks(), sourceMarksBefore);
    std::unordered_map<const Data::ElementMap*, unsigned> archiveIdsAfter;
    inBundle.elementMap->snapshotArchiveIds(archiveIdsAfter);
    EXPECT_EQ(archiveIdsAfter, archiveIdsBefore);
    EXPECT_EQ(hasher->size(), sourceSizeBefore);
    EXPECT_EQ(hasher->getRevision(), sourceRevisionBefore);
    EXPECT_EQ(hasher->getThreshold(), sourceThresholdBefore);
    EXPECT_EQ(hasher->getLastID(), sourceHighWaterBefore);
}

TEST_F(NonBlockingGeometryTest, DuplicatedSharedChildLeavesFailIdentityAssertion)
{
    App::StringHasherRef hasher(new App::StringHasher);
    auto makeLeaf = [&](const char* label, long tag) {
        auto leaf = std::make_shared<Data::ElementMap>();
        leaf->hasher = hasher;
        for (int i = 1; i <= 6; ++i) {
            Data::IndexedName edge("Edge", i);
            App::StringIDRef sid =
                hasher->getID(QByteArray(label) + QByteArray::number(i));
            if (!sid) {
                return Data::ElementMapPtr {};
            }
            Data::MappedName mapped(sid);
            Data::ElementIDRefs sids;
            sids.push_back(sid);
            leaf->setElementName(edge, mapped, tag, &sids);
            sid.mark();
        }
        return leaf;
    };

    auto sharedLeafA = makeLeaf("SharedEdge", 11);
    auto sharedLeafB = makeLeaf("SharedEdge", 11);
    ASSERT_TRUE(sharedLeafA);
    ASSERT_TRUE(sharedLeafB);
    EXPECT_NE(sharedLeafA.get(), sharedLeafB.get())
        << "bad-path setup must use distinct ElementMap instances";

    auto parent = std::make_shared<Data::ElementMap>();
    parent->hasher = hasher;
    Data::ElementIDRefs emptySids;
    parent->addChildElements(
        30,
        {{Data::IndexedName("Face", 1), 6, 0, 11, sharedLeafA, QByteArray("sharedAxxxx"), emptySids},
         {Data::IndexedName("Face", 7), 6, 0, 11, sharedLeafB, QByteArray("sharedBxxxx"), emptySids}});

    int sharedRefs = 0;
    Data::ElementMap* firstShared = nullptr;
    for (const auto& child : parent->getChildElements()) {
        ASSERT_TRUE(child.elementMap);
        Data::ElementIDRefs sids;
        ASSERT_TRUE(child.elementMap->find(Data::IndexedName("Edge", 1), &sids));
        ASSERT_FALSE(sids.empty());
        if (sids.front().dataToText().find("SharedEdge") == std::string::npos) {
            continue;
        }
        if (!firstShared) {
            firstShared = child.elementMap.get();
        }
        if (child.elementMap.get() == firstShared) {
            ++sharedRefs;
        }
    }
    EXPECT_LT(sharedRefs, 2)
        << "duplicated SharedEdge leaves must not satisfy shared identity (>=2 same pointer)";
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
    outBundle.valid = true;
    outBundle.shapeTag = 42;
    EXPECT_FALSE(Part::TopoShapeArchive::readArchive(archivePath, outBundle));
    // Failure must leave the caller's bundle untouched (candidate-only decode).
    EXPECT_TRUE(outBundle.valid);
    EXPECT_EQ(outBundle.shapeTag, 42);
}

TEST_F(NonBlockingGeometryTest, HighWaterOutOfRangeRejected)
{
    std::string archivePath = (_tempDir->path() + "/hiwater.fcg").toStdString();
    {
        std::ofstream ofs(archivePath, std::ios::binary);
        ASSERT_TRUE(ofs.is_open());
        ofs.write("FCG1", 4);
        uint32_t version = 4;
        ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));
        long tag = 1;
        ofs.write(reinterpret_cast<const char*>(&tag), sizeof(tag));
        uint32_t zero = 0;
        ofs.write(reinterpret_cast<const char*>(&zero), sizeof(zero));  // shapeLen
        ofs.write(reinterpret_cast<const char*>(&zero), sizeof(zero));  // mapLen
        uint64_t highWater =
            static_cast<uint64_t>(std::numeric_limits<long>::max()) + 1ull;
        ofs.write(reinterpret_cast<const char*>(&highWater), sizeof(highWater));
        uint64_t revision = 0;
        ofs.write(reinterpret_cast<const char*>(&revision), sizeof(revision));
        int32_t threshold = 0;
        ofs.write(reinterpret_cast<const char*>(&threshold), sizeof(threshold));
        ofs.write(reinterpret_cast<const char*>(&zero), sizeof(zero));  // hasherLen
        // checksum length + empty checksum — decode fails earlier on highWater.
        ofs.write(reinterpret_cast<const char*>(&zero), sizeof(zero));
    }
    Part::FrozenTopoShapeBundle outBundle;
    outBundle.shapeTag = 7;
    EXPECT_FALSE(Part::TopoShapeArchive::readArchive(archivePath, outBundle));
    EXPECT_EQ(outBundle.shapeTag, 7);
}

TEST_F(NonBlockingGeometryTest, HasherClosureEntryIdOutOfRangeUsesNarrowingGuard)
{
    if (sizeof(long) >= 8) {
        GTEST_SKIP() << "LP64: FCG1 entry-id decode narrowing is only observable when long is 32-bit";
    }
    const int64_t badId =
        static_cast<int64_t>(static_cast<uint64_t>(std::numeric_limits<long>::max()) + 1ull);
    long ignored = 0;
    ASSERT_FALSE(Part::TopoShapeArchive::int64ToLongChecked(badId, ignored));
    std::ostringstream hasherBlob;
    const uint32_t count = 1;
    const uint32_t flags = 0;
    const uint32_t zero = 0;
    hasherBlob.write(reinterpret_cast<const char*>(&count), sizeof(count));
    hasherBlob.write(reinterpret_cast<const char*>(&badId), sizeof(badId));
    hasherBlob.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
    hasherBlob.write(reinterpret_cast<const char*>(&zero), sizeof(zero));
    hasherBlob.write(reinterpret_cast<const char*>(&zero), sizeof(zero));
    hasherBlob.write(reinterpret_cast<const char*>(&zero), sizeof(zero));

    const uint32_t version = 4;
    const long tag = 1;
    const std::string shapeData;
    const std::string mapData;
    const uint64_t highWater = 0;
    const uint64_t revision = 0;
    const int32_t threshold = 0;
    const std::string& hasherData = hasherBlob.str();

    std::vector<uint8_t> digestInput;
    const auto appendBytes = [&digestInput](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        digestInput.insert(digestInput.end(), bytes, bytes + size);
    };
    appendBytes(&version, sizeof(version));
    appendBytes(&tag, sizeof(tag));
    appendBytes(shapeData.data(), shapeData.size());
    appendBytes(mapData.data(), mapData.size());
    appendBytes(&highWater, sizeof(highWater));
    appendBytes(&revision, sizeof(revision));
    appendBytes(&threshold, sizeof(threshold));
    appendBytes(hasherData.data(), hasherData.size());
    const std::string checksum = Part::TopoShapeArchive::calculateSha256(digestInput);

    const std::string archivePath = (_tempDir->path() + "/bad_entry_id.fcg").toStdString();
    {
        std::ofstream ofs(archivePath, std::ios::binary);
        ASSERT_TRUE(ofs.is_open());
        const uint32_t shapeLen = 0;
        const uint32_t mapLen = 0;
        const uint32_t hasherLen = static_cast<uint32_t>(hasherData.size());
        const uint32_t checksumLen = static_cast<uint32_t>(checksum.size());
        ofs.write("FCG1", 4);
        ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));
        ofs.write(reinterpret_cast<const char*>(&tag), sizeof(tag));
        ofs.write(reinterpret_cast<const char*>(&shapeLen), sizeof(shapeLen));
        ofs.write(reinterpret_cast<const char*>(&mapLen), sizeof(mapLen));
        ofs.write(reinterpret_cast<const char*>(&highWater), sizeof(highWater));
        ofs.write(reinterpret_cast<const char*>(&revision), sizeof(revision));
        ofs.write(reinterpret_cast<const char*>(&threshold), sizeof(threshold));
        ofs.write(reinterpret_cast<const char*>(&hasherLen), sizeof(hasherLen));
        ofs.write(hasherData.data(), static_cast<std::streamsize>(hasherLen));
        ofs.write(reinterpret_cast<const char*>(&checksumLen), sizeof(checksumLen));
        ofs.write(checksum.data(), static_cast<std::streamsize>(checksumLen));
    }

    App::StringHasherRef canonical(new App::StringHasher);
    canonical->getID(QByteArray("KeepMe"));
    const size_t sizeBefore = canonical->size();

    Part::FrozenTopoShapeBundle outBundle;
    outBundle.valid = true;
    outBundle.shapeTag = 42;
    outBundle.hasher = canonical;
    outBundle.shape.Hasher = canonical;
    EXPECT_FALSE(Part::TopoShapeArchive::readArchive(archivePath, outBundle));
    EXPECT_TRUE(outBundle.valid);
    EXPECT_EQ(outBundle.shapeTag, 42);
    EXPECT_EQ(static_cast<App::StringHasher*>(outBundle.hasher), static_cast<App::StringHasher*>(canonical));
    EXPECT_EQ(canonical->size(), sizeBefore);
}

TEST_F(NonBlockingGeometryTest, HasherClosureRelatedIdOutOfRangeUsesNarrowingGuard)
{
    if (sizeof(long) >= 8) {
        GTEST_SKIP() << "LP64: related-id decode narrowing is only observable when long is 32-bit";
    }
    const int64_t entryId = 1;
    const int64_t badRelated =
        static_cast<int64_t>(static_cast<uint64_t>(std::numeric_limits<long>::max()) + 1ull);
    long ignored = 0;
    ASSERT_FALSE(Part::TopoShapeArchive::int64ToLongChecked(badRelated, ignored));
    std::ostringstream hasherBlob;
    const uint32_t count = 1;
    const uint32_t flags = 0;
    const uint32_t zero = 0;
    const uint32_t relatedCount = 1;
    hasherBlob.write(reinterpret_cast<const char*>(&count), sizeof(count));
    hasherBlob.write(reinterpret_cast<const char*>(&entryId), sizeof(entryId));
    hasherBlob.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
    hasherBlob.write(reinterpret_cast<const char*>(&zero), sizeof(zero));
    hasherBlob.write(reinterpret_cast<const char*>(&zero), sizeof(zero));
    hasherBlob.write(reinterpret_cast<const char*>(&relatedCount), sizeof(relatedCount));
    hasherBlob.write(reinterpret_cast<const char*>(&badRelated), sizeof(badRelated));

    const uint32_t version = 4;
    const long tag = 1;
    const std::string shapeData;
    const std::string mapData;
    const uint64_t highWater = 0;
    const uint64_t revision = 0;
    const int32_t threshold = 0;
    const std::string& hasherData = hasherBlob.str();

    std::vector<uint8_t> digestInput;
    const auto appendBytes = [&digestInput](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        digestInput.insert(digestInput.end(), bytes, bytes + size);
    };
    appendBytes(&version, sizeof(version));
    appendBytes(&tag, sizeof(tag));
    appendBytes(shapeData.data(), shapeData.size());
    appendBytes(mapData.data(), mapData.size());
    appendBytes(&highWater, sizeof(highWater));
    appendBytes(&revision, sizeof(revision));
    appendBytes(&threshold, sizeof(threshold));
    appendBytes(hasherData.data(), hasherData.size());
    const std::string checksum = Part::TopoShapeArchive::calculateSha256(digestInput);

    const std::string archivePath = (_tempDir->path() + "/bad_related_id.fcg").toStdString();
    {
        std::ofstream ofs(archivePath, std::ios::binary);
        ASSERT_TRUE(ofs.is_open());
        const uint32_t shapeLen = 0;
        const uint32_t mapLen = 0;
        const uint32_t hasherLen = static_cast<uint32_t>(hasherData.size());
        const uint32_t checksumLen = static_cast<uint32_t>(checksum.size());
        ofs.write("FCG1", 4);
        ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));
        ofs.write(reinterpret_cast<const char*>(&tag), sizeof(tag));
        ofs.write(reinterpret_cast<const char*>(&shapeLen), sizeof(shapeLen));
        ofs.write(reinterpret_cast<const char*>(&mapLen), sizeof(mapLen));
        ofs.write(reinterpret_cast<const char*>(&highWater), sizeof(highWater));
        ofs.write(reinterpret_cast<const char*>(&revision), sizeof(revision));
        ofs.write(reinterpret_cast<const char*>(&threshold), sizeof(threshold));
        ofs.write(reinterpret_cast<const char*>(&hasherLen), sizeof(hasherLen));
        ofs.write(hasherData.data(), static_cast<std::streamsize>(hasherLen));
        ofs.write(reinterpret_cast<const char*>(&checksumLen), sizeof(checksumLen));
        ofs.write(checksum.data(), static_cast<std::streamsize>(checksumLen));
    }

    Part::FrozenTopoShapeBundle outBundle;
    outBundle.valid = true;
    outBundle.shapeTag = 9;
    EXPECT_FALSE(Part::TopoShapeArchive::readArchive(archivePath, outBundle));
    EXPECT_EQ(outBundle.shapeTag, 9);
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

namespace
{

void captureHasherSnapshot(const App::StringHasherRef& hasher,
                           size_t& size,
                           long& lastId,
                           uint64_t& revision,
                           int& threshold,
                           uint64_t& highWater,
                           std::unordered_map<long, bool>& marks)
{
    size = hasher->size();
    lastId = hasher->getLastID();
    revision = hasher->getRevision();
    threshold = hasher->getThreshold();
    highWater = static_cast<uint64_t>(hasher->getLastID());
    marks = hasher->snapshotMarks();
}

} // namespace

TEST_F(NonBlockingGeometryTest, ExactHasherMaterializeFromV4Closure)
{
    App::StringHasherRef source(new App::StringHasher);
    source->setThreshold(12);
    auto sidA = source->getID(QByteArray("ShortName"));
    auto sidB = source->getID(QByteArray("ThisNameIsLongEnoughToHash"),
                              App::StringHasher::Option::Hashable);
    ASSERT_TRUE(sidA);
    ASSERT_TRUE(sidB);
    ASSERT_TRUE(sidB.isHashed());
    sidA.mark();
    sidB.mark();
    source->advanceRevision();
    source->advanceRevision();

    App::StringHasherClosure closure = source->captureClosure(/*markedOnly=*/true);
    EXPECT_EQ(closure.threshold, 12);
    EXPECT_EQ(closure.revision, 2u);
    ASSERT_GE(closure.entries.size(), 2u);

    App::StringHasherRef fresh(new App::StringHasher);
    auto mat = fresh->materializeExactClosure(closure);
    ASSERT_TRUE(mat.success) << mat.errorCode << ": " << mat.errorMessage;
    EXPECT_EQ(fresh->getThreshold(), 12);
    EXPECT_EQ(fresh->getRevision(), 2u);
    EXPECT_EQ(fresh->getLastID(), source->getLastID());
    EXPECT_TRUE(fresh->getID(sidA.value()));
    EXPECT_TRUE(fresh->getID(sidB.value()));
    EXPECT_EQ(fresh->getID(sidA.value()).dataToText(), std::string("ShortName"));
    auto again = fresh->getID(QByteArray("ThisNameIsLongEnoughToHash"),
                              App::StringHasher::Option::Hashable);
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value(), sidB.value());
    EXPECT_TRUE(again.isHashed());
}

TEST_F(NonBlockingGeometryTest, ExactHasherCompatibleDeltaAppend)
{
    App::StringHasherRef canonical(new App::StringHasher);
    canonical->setThreshold(8);
    auto keep = canonical->getID(QByteArray("Keep"));
    ASSERT_TRUE(keep);
    keep.mark();
    canonical->advanceRevision();

    App::StringHasherRef worker(new App::StringHasher);
    ASSERT_TRUE(worker->materializeExactClosure(canonical->captureClosure(false)).success);
    auto extra = worker->getID(QByteArray("AppendOK"));
    ASSERT_TRUE(extra);
    EXPECT_FALSE(extra.isHashed());
    extra.mark();

    App::StringHasherClosure delta = worker->captureClosure(false);
    delta.revision = canonical->getRevision();
    const size_t sizeBefore = canonical->size();
    auto merged = canonical->mergeExactClosure(delta, canonical->getRevision());
    ASSERT_TRUE(merged.success) << merged.errorCode;
    EXPECT_GE(merged.appendedCount, 1u);
    EXPECT_GT(canonical->size(), sizeBefore);
    EXPECT_EQ(canonical->getThreshold(), 8);
    EXPECT_TRUE(canonical->getID(extra.value()));
    EXPECT_EQ(canonical->getID(extra.value()).dataToText(), std::string("AppendOK"));
}

TEST_F(NonBlockingGeometryTest, HasherThresholdMismatchLeavesCanonicalUnchanged)
{
    App::StringHasherRef canonical(new App::StringHasher);
    canonical->setThreshold(10);
    auto keep = canonical->getID(QByteArray("Stable"));
    ASSERT_TRUE(keep);
    size_t size = 0;
    long lastId = 0;
    uint64_t revision = 0;
    int threshold = 0;
    uint64_t highWater = 0;
    std::unordered_map<long, bool> marks;
    captureHasherSnapshot(canonical, size, lastId, revision, threshold, highWater, marks);

    App::StringHasherClosure delta;
    delta.threshold = 20;
    delta.revision = revision;
    delta.entries.push_back({keep.value() + 1, 0u, QByteArray("WouldAppend"), QByteArray(), {}});
    delta.entries.push_back({keep.value(), 0u, QByteArray("Collision"), QByteArray(), {}});

    auto merged = canonical->mergeExactClosure(delta, revision);
    EXPECT_FALSE(merged.success);
    EXPECT_EQ(merged.errorCode, "ThresholdMismatch");
    EXPECT_EQ(canonical->size(), size);
    EXPECT_EQ(canonical->getLastID(), lastId);
    EXPECT_EQ(canonical->getRevision(), revision);
    EXPECT_EQ(canonical->getThreshold(), 10);
    EXPECT_FALSE(canonical->getID(keep.value() + 1));
}

TEST_F(NonBlockingGeometryTest, HasherValueReassignedUnderDifferentIdRejected)
{
    App::StringHasherRef canonical(new App::StringHasher);
    auto existing = canonical->getID(QByteArray("SameValue"));
    ASSERT_TRUE(existing);
    const size_t sizeBefore = canonical->size();
    const int thresholdBefore = canonical->getThreshold();
    const long lastBefore = canonical->getLastID();
    const uint64_t revBefore = canonical->getRevision();

    App::StringHasherClosure delta;
    delta.threshold = canonical->getThreshold();
    delta.entries.push_back(
        {existing.value() + 5, 0u, QByteArray("SameValue"), QByteArray(), {}});

    auto merged = canonical->mergeExactClosure(delta, 0);
    EXPECT_FALSE(merged.success);
    EXPECT_EQ(merged.errorCode, "ValueCollision");
    EXPECT_EQ(canonical->size(), sizeBefore);
    EXPECT_EQ(canonical->getThreshold(), thresholdBefore);
    EXPECT_EQ(canonical->getLastID(), lastBefore);
    EXPECT_EQ(canonical->getRevision(), revBefore);
    EXPECT_FALSE(canonical->getID(existing.value() + 5));
}

TEST_F(NonBlockingGeometryTest, HasherClosureInternalValueCollisionRejectedAtomically)
{
    App::StringHasherRef canonical(new App::StringHasher);
    auto keep = canonical->getID(QByteArray("KeepOriginal"));
    ASSERT_TRUE(keep);
    const size_t sizeBefore = canonical->size();
    const long lastBefore = canonical->getLastID();

    App::StringHasherClosure delta;
    delta.threshold = 0;
    delta.entries.push_back(
        {keep.value() + 10, 0u, QByteArray("WouldAppend"), QByteArray(), {}});
    delta.entries.push_back(
        {keep.value() + 11, 0u, QByteArray("DupValue"), QByteArray(), {}});
    delta.entries.push_back(
        {keep.value() + 12, 0u, QByteArray("DupValue"), QByteArray(), {}});

    auto merged = canonical->mergeExactClosure(delta, 0);
    EXPECT_FALSE(merged.success);
    EXPECT_EQ(merged.errorCode, "ValueCollision");
    EXPECT_EQ(merged.appendedCount, 0u);
    EXPECT_EQ(canonical->size(), sizeBefore);
    EXPECT_EQ(canonical->getLastID(), lastBefore);
    EXPECT_FALSE(canonical->getID(keep.value() + 10));
}

TEST_F(NonBlockingGeometryTest, HasherRejectsMissingRelatedAndForwardRelation)
{
    App::StringHasherRef canonical(new App::StringHasher);
    const size_t sizeBefore = canonical->size();

    App::StringHasherClosure missingRelated;
    missingRelated.threshold = 0;
    missingRelated.entries.push_back(
        {5, 0u, QByteArray("NeedsRelated"), QByteArray(), {99}});
    auto m1 = canonical->mergeExactClosure(missingRelated, 0);
    EXPECT_FALSE(m1.success);
    EXPECT_EQ(m1.errorCode, "MissingRelatedId");
    EXPECT_EQ(canonical->size(), sizeBefore);

    App::StringHasherClosure forward;
    forward.threshold = 0;
    forward.entries.push_back({3, 0u, QByteArray("Owner"), QByteArray(), {8}});
    forward.entries.push_back({8, 0u, QByteArray("LaterRelated"), QByteArray(), {}});
    auto m2 = canonical->mergeExactClosure(forward, 0);
    EXPECT_FALSE(m2.success);
    EXPECT_EQ(m2.errorCode, "MissingRelatedId");
    EXPECT_EQ(canonical->size(), sizeBefore);
}

TEST_F(NonBlockingGeometryTest, HasherRejectsNegativeThresholdAndStaleRevision)
{
    App::StringHasherRef canonical(new App::StringHasher);
    canonical->advanceRevision();
    const int thresholdBefore = canonical->getThreshold();
    const uint64_t revBefore = canonical->getRevision();
    const size_t sizeBefore = canonical->size();

    App::StringHasherClosure badThreshold;
    badThreshold.threshold = -1;
    badThreshold.entries.push_back({1, 0u, QByteArray("X"), QByteArray(), {}});
    auto m1 = canonical->mergeExactClosure(badThreshold, 0);
    EXPECT_FALSE(m1.success);
    EXPECT_EQ(m1.errorCode, "InvalidThreshold");
    EXPECT_EQ(canonical->getThreshold(), thresholdBefore);
    EXPECT_EQ(canonical->size(), sizeBefore);

    App::StringHasherClosure stale;
    stale.threshold = thresholdBefore;
    stale.revision = revBefore + 10;
    stale.entries.push_back({1, 0u, QByteArray("X"), QByteArray(), {}});
    auto m2 = canonical->mergeExactClosure(stale, revBefore);
    EXPECT_FALSE(m2.success);
    EXPECT_EQ(m2.errorCode, "RevisionMismatch");
    EXPECT_EQ(canonical->getRevision(), revBefore);
    EXPECT_EQ(canonical->size(), sizeBefore);
}

TEST_F(NonBlockingGeometryTest, BooleanCodecRejectsBadRequestBeforeOcc)
{
    App::StringHasherRef hasher(new App::StringHasher);
    BRepPrimAPI_MakeBox box1(10.0, 10.0, 10.0);
    BRepPrimAPI_MakeBox box2(gp_Pnt(5, 5, 5), 10.0, 10.0, 10.0);
    Part::TopoShape s1(box1.Shape(), 1, hasher);
    Part::TopoShape s2(box2.Shape(), 2, hasher);
    auto b1 = Part::TopoShapeArchive::createBundle(s1);
    auto b2 = Part::TopoShapeArchive::createBundle(s2);
    ASSERT_TRUE(b1.valid && b2.valid);

    App::GeometryRequestWorkspace workspace(_tempDir->path());
    Part::BooleanGeometryOperation op(Part::BooleanType::Fuse, b1, b2);
    ASSERT_TRUE(op.writeArchive(workspace).success);
    ASSERT_TRUE(workspace.publishRequestJson());

    QFile reqFile(_tempDir->path() + QStringLiteral("/request.json"));
    ASSERT_TRUE(reqFile.open(QIODevice::ReadOnly));
    QJsonObject good = QJsonDocument::fromJson(reqFile.readAll()).object();
    reqFile.close();

    auto expectReject = [&](QJsonObject bad, const char* expectedCode) {
        std::string errorCode;
        std::string errorMessage;
        auto decoded = Part::BooleanGeometryOperation::decodeFromRequest(
            bad, _tempDir->path(), errorCode, errorMessage);
        EXPECT_FALSE(decoded) << expectedCode;
        EXPECT_EQ(errorCode, expectedCode) << errorMessage;
        EXPECT_FALSE(QFileInfo::exists(_tempDir->path() + QStringLiteral("/result.fcg")));
    };

    {
        QJsonObject bad = good;
        bad.remove(QStringLiteral("basePath"));
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("booleanType"), QStringLiteral("NotAType"));
        expectReject(bad, "UnknownBooleanType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("operationType"), QStringLiteral("Part::Fillet"));
        expectReject(bad, "UnsupportedOperation");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), 99);
        expectReject(bad, "UnsupportedCodecVersion");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), QStringLiteral("1"));
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("basePath"), QStringLiteral("/tmp/escape.fcg"));
        expectReject(bad, "UntrustedOperandPath");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("basePath"), QStringLiteral("../escape.fcg"));
        expectReject(bad, "UntrustedOperandPath");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("toolPath"), bad.value(QStringLiteral("basePath")));
        expectReject(bad, "DuplicateOperandPath");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("baseSize"),
                   bad.value(QStringLiteral("baseSize")).toVariant().toLongLong() + 1);
        expectReject(bad, "OperandSizeMismatch");
    }
}

TEST_F(NonBlockingGeometryTest, FilletCodecRoundTripPreservesOperandsAndEdges)
{
    BRepPrimAPI_MakeBox box(10.0, 10.0, 10.0);
    App::StringHasherRef hasher(new App::StringHasher);
    Part::TopoShape shape(box.Shape(), /*tag=*/1, hasher);

    {
        auto map = std::make_shared<Data::ElementMap>();
        map->hasher = hasher;
        shape.resetElementMap(map);
        Data::IndexedName edge1("Edge", 1);
        App::StringIDRef sid = hasher->getID(QByteArray("SeedEdgeA"));
        ASSERT_TRUE(sid);
        Data::MappedName mapped(sid);
        Data::ElementIDRefs sids;
        sids.push_back(sid);
        map->setElementName(edge1, mapped, shape.Tag, &sids);
    }

    Part::FrozenTopoShapeBundle base = Part::TopoShapeArchive::createBundle(shape);
    ASSERT_TRUE(base.valid);
    ASSERT_TRUE(base.hasher);
    ASSERT_TRUE(base.elementMap);
    EXPECT_GE(base.elementMap->size(), 1u);

    std::vector<Part::FilletEdgeSpec> edges;
    edges.push_back({0, 1.0, 2.5});
    edges.push_back({1, 0.75, 1.25});

    const QString workDir = _tempDir->path() + QStringLiteral("/fillet_codec");
    App::GeometryRequestWorkspace workspace(workDir);
    Part::FilletGeometryOperation op(base, edges);
    ASSERT_TRUE(op.writeArchive(workspace).success);
    ASSERT_TRUE(workspace.publishRequestJson());
    ASSERT_TRUE(QFileInfo::exists(workDir + QStringLiteral("/base.fcg")));
    ASSERT_TRUE(QFileInfo::exists(workDir + QStringLiteral("/request.json")));

    QFile reqFile(workDir + QStringLiteral("/request.json"));
    ASSERT_TRUE(reqFile.open(QIODevice::ReadOnly));
    const QJsonObject published = QJsonDocument::fromJson(reqFile.readAll()).object();
    reqFile.close();

    EXPECT_EQ(published.value(QStringLiteral("operationType")).toString(), QStringLiteral("Part::Fillet"));
    EXPECT_EQ(published.value(QStringLiteral("codecVersion")).toInt(), 1);
    EXPECT_EQ(published.value(QStringLiteral("basePath")).toString(), QStringLiteral("base.fcg"));
    const QString baseAbs = workDir + QStringLiteral("/base.fcg");
    const qint64 stagedSize = QFileInfo(baseAbs).size();
    EXPECT_EQ(published.value(QStringLiteral("baseSize")).toVariant().toLongLong(), stagedSize);
    QFile baseFile(baseAbs);
    ASSERT_TRUE(baseFile.open(QIODevice::ReadOnly));
    const QByteArray stagedDigest =
        QCryptographicHash::hash(baseFile.readAll(), QCryptographicHash::Sha256).toHex();
    baseFile.close();
    EXPECT_EQ(published.value(QStringLiteral("baseSha256")).toString().toLower(),
              QString::fromLatin1(stagedDigest));
    const QJsonArray publishedEdges = published.value(QStringLiteral("edges")).toArray();
    ASSERT_EQ(publishedEdges.size(), 2);
    EXPECT_EQ(publishedEdges.at(0).toObject().value(QStringLiteral("edgeIndex")).toInt(), 0);
    EXPECT_DOUBLE_EQ(publishedEdges.at(0).toObject().value(QStringLiteral("startRadius")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(publishedEdges.at(0).toObject().value(QStringLiteral("endRadius")).toDouble(), 2.5);
    EXPECT_EQ(publishedEdges.at(1).toObject().value(QStringLiteral("edgeIndex")).toInt(), 1);
    EXPECT_DOUBLE_EQ(publishedEdges.at(1).toObject().value(QStringLiteral("startRadius")).toDouble(), 0.75);
    EXPECT_DOUBLE_EQ(publishedEdges.at(1).toObject().value(QStringLiteral("endRadius")).toDouble(), 1.25);

    std::string errorCode;
    std::string errorMessage;
    auto decoded = Part::FilletGeometryOperation::decodeFromRequest(
        published, workDir, errorCode, errorMessage);
    ASSERT_TRUE(decoded) << errorCode << ": " << errorMessage;
    const Part::FrozenTopoShapeBundle& decodedBase = decoded->baseBundle();
    EXPECT_TRUE(decodedBase.valid);
    EXPECT_FALSE(decodedBase.shape.isNull());
    ASSERT_TRUE(decodedBase.hasher);
    ASSERT_TRUE(decodedBase.elementMap);
    EXPECT_GT(decodedBase.elementMap->size(), 0u);
    EXPECT_EQ(static_cast<App::StringHasher*>(decodedBase.shape.Hasher),
              static_cast<App::StringHasher*>(decodedBase.hasher));
    ASSERT_EQ(decoded->edgeSpecs().size(), 2u);
    EXPECT_EQ(decoded->edgeSpecs()[0].edgeIndex, 0u);
    EXPECT_DOUBLE_EQ(decoded->edgeSpecs()[0].startRadius, 1.0);
    EXPECT_DOUBLE_EQ(decoded->edgeSpecs()[0].endRadius, 2.5);
    EXPECT_EQ(decoded->edgeSpecs()[1].edgeIndex, 1u);
    EXPECT_DOUBLE_EQ(decoded->edgeSpecs()[1].startRadius, 0.75);
    EXPECT_DOUBLE_EQ(decoded->edgeSpecs()[1].endRadius, 1.25);
    EXPECT_FALSE(QFileInfo::exists(workDir + QStringLiteral("/result.fcg")));
}

TEST_F(NonBlockingGeometryTest, FilletCodecRejectsBadRequestBeforeOcc)
{
    BRepPrimAPI_MakeBox box(10.0, 10.0, 10.0);
    App::StringHasherRef hasher(new App::StringHasher);
    Part::TopoShape shape(box.Shape(), /*tag=*/1, hasher);
    auto map = std::make_shared<Data::ElementMap>();
    map->hasher = hasher;
    shape.resetElementMap(map);
    Part::FrozenTopoShapeBundle base = Part::TopoShapeArchive::createBundle(shape);
    ASSERT_TRUE(base.valid);

    const QString workDir = _tempDir->path() + QStringLiteral("/fillet_reject");
    App::GeometryRequestWorkspace workspace(workDir);
    Part::FilletGeometryOperation op(base, {{0, 1.0, 1.0}});
    ASSERT_TRUE(op.writeArchive(workspace).success);
    ASSERT_TRUE(workspace.publishRequestJson());

    QFile reqFile(workDir + QStringLiteral("/request.json"));
    ASSERT_TRUE(reqFile.open(QIODevice::ReadOnly));
    QJsonObject good = QJsonDocument::fromJson(reqFile.readAll()).object();
    reqFile.close();

    const auto makeValidEdge = []() {
        QJsonObject edge;
        edge.insert(QStringLiteral("edgeIndex"), 0);
        edge.insert(QStringLiteral("startRadius"), 1.0);
        edge.insert(QStringLiteral("endRadius"), 1.0);
        return edge;
    };

    auto expectReject = [&](const QJsonObject& bad,
                            const char* expectedCode,
                            const QString& decodeDir = QString()) {
        const QString dir = decodeDir.isEmpty() ? workDir : decodeDir;
        std::string errorCode;
        std::string errorMessage;
        auto decoded = Part::FilletGeometryOperation::decodeFromRequest(bad, dir, errorCode, errorMessage);
        EXPECT_FALSE(decoded) << expectedCode;
        EXPECT_EQ(errorCode, expectedCode) << errorMessage;
        EXPECT_FALSE(QFileInfo::exists(dir + QStringLiteral("/result.fcg")));
    };

    {
        QJsonObject bad = good;
        bad.remove(QStringLiteral("basePath"));
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("operationType"), QStringLiteral("Part::Boolean"));
        expectReject(bad, "UnsupportedOperation");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), 99);
        expectReject(bad, "UnsupportedCodecVersion");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), QStringLiteral("1"));
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), 1.5);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), -1);
        expectReject(bad, "OutOfRangeJsonNumber");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), -2147483649.0);
        expectReject(bad, "OutOfRangeJsonNumber");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), 4294967296.0);
        expectReject(bad, "OutOfRangeJsonNumber");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), 4294967295.0);
        expectReject(bad, "UnsupportedCodecVersion");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("basePath"), QStringLiteral("/tmp/escape.fcg"));
        expectReject(bad, "UntrustedOperandPath");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("basePath"), QStringLiteral("../escape.fcg"));
        expectReject(bad, "UntrustedOperandPath");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("basePath"), QStringLiteral("missing.fcg"));
        expectReject(bad, "MissingOperandArchive");
    }
    {
        QJsonObject bad = good;
        bad.remove(QStringLiteral("baseSize"));
        expectReject(bad, "MissingJsonField");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("baseSize"), QStringLiteral("100"));
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("baseSize"), 1.5);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("baseSize"), -1);
        expectReject(bad, "OperandSizeMismatch");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("baseSize"), 9223372036854775808.0);
        expectReject(bad, "OutOfRangeJsonNumber");
    }
    {
        QJsonObject bad = good;
        const qint64 maxSection =
            static_cast<qint64>(App::GeometryRequestWorkspace::maxWorkspaceSectionBytes());
        bad.insert(QStringLiteral("baseSize"), static_cast<double>(maxSection) + 1.0);
        expectReject(bad, "OutOfRangeJsonNumber");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("baseSize"),
                   bad.value(QStringLiteral("baseSize")).toVariant().toLongLong() + 1);
        expectReject(bad, "OperandSizeMismatch");
    }
    {
        QJsonObject bad = good;
        QString sha = bad.value(QStringLiteral("baseSha256")).toString();
        sha[0] = (sha[0] == QLatin1Char('a')) ? QLatin1Char('b') : QLatin1Char('a');
        bad.insert(QStringLiteral("baseSha256"), sha);
        expectReject(bad, "OperandDigestMismatch");
    }
    {
        const QString corruptDir = _tempDir->path() + QStringLiteral("/fillet_corrupt");
        QDir().mkpath(corruptDir);
        const QString corruptPath = corruptDir + QStringLiteral("/base.fcg");
        ASSERT_TRUE(QFile::copy(workDir + QStringLiteral("/base.fcg"), corruptPath));
        {
            std::ofstream ofs(corruptPath.toStdString(), std::ios::binary | std::ios::app);
            ofs << "TRAIL";
        }
        QJsonObject bad = good;
        bad.insert(QStringLiteral("basePath"), QStringLiteral("base.fcg"));
        bad.insert(QStringLiteral("baseSize"), static_cast<qint64>(QFileInfo(corruptPath).size()));
        bad.insert(QStringLiteral("baseSha256"),
                   QString::fromStdString(
                       Part::TopoShapeArchive::calculateSha256File(corruptPath.toStdString())));
        expectReject(bad, "OperandDecodeFailed", corruptDir);
    }
    {
        QJsonObject bad = good;
        bad.remove(QStringLiteral("edges"));
        expectReject(bad, "MissingJsonField");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("edges"), QStringLiteral("not-an-array"));
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("edges"), QJsonArray());
        expectReject(bad, "EmptyEdges");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        edges.append(QStringLiteral("not-an-object"));
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.remove(QStringLiteral("edgeIndex"));
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "MissingJsonField");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("edgeIndex"), QStringLiteral("0"));
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("edgeIndex"), 1.5);
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("edgeIndex"), -1);
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "InvalidEdgeIndex");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("edgeIndex"), 4294967296.0);
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "OutOfRangeJsonNumber");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("edgeIndex"), 999);
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "InvalidEdgeIndex");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        edges.append(makeValidEdge());
        edges.append(makeValidEdge());
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "DuplicateEdgeIndex");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.remove(QStringLiteral("startRadius"));
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "MissingJsonField");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("startRadius"), QStringLiteral("1"));
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("startRadius"), QJsonValue::Null);
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("startRadius"), 0.0);
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "InvalidRadius");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("startRadius"), -1.0);
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "InvalidRadius");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.remove(QStringLiteral("endRadius"));
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "MissingJsonField");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("endRadius"), QStringLiteral("1"));
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("endRadius"), QJsonValue::Null);
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("endRadius"), 0.0);
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "InvalidRadius");
    }
    {
        QJsonObject bad = good;
        QJsonArray edges;
        QJsonObject edge = makeValidEdge();
        edge.insert(QStringLiteral("endRadius"), -2.0);
        edges.append(edge);
        bad.insert(QStringLiteral("edges"), edges);
        expectReject(bad, "InvalidRadius");
    }
}

namespace
{

TopoDS_Wire makeRectWire(double width, double height)
{
    const auto e1 =
        BRepBuilderAPI_MakeEdge(gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(width, 0.0, 0.0)).Edge();
    const auto e2 =
        BRepBuilderAPI_MakeEdge(gp_Pnt(width, 0.0, 0.0), gp_Pnt(width, height, 0.0)).Edge();
    const auto e3 =
        BRepBuilderAPI_MakeEdge(gp_Pnt(width, height, 0.0), gp_Pnt(0.0, height, 0.0)).Edge();
    const auto e4 =
        BRepBuilderAPI_MakeEdge(gp_Pnt(0.0, height, 0.0), gp_Pnt(0.0, 0.0, 0.0)).Edge();
    return BRepBuilderAPI_MakeWire(e1, e2, e3, e4).Wire();
}

TopoDS_Shape makeProfileBoxShape(double width, double height, double depth)
{
    return BRepPrimAPI_MakeBox(width, height, depth).Shape();
}

void expectMatchingGeometryFingerprint(const Part::FrozenTopoShapeBundle& expected,
                                       const Part::FrozenTopoShapeBundle& actual)
{
    const std::string expectedFp = Part::TopoShapeArchive::fingerprintBundle(expected);
    const std::string actualFp = Part::TopoShapeArchive::fingerprintBundle(actual);
    const auto bbPos = expectedFp.find(";bb=");
    ASSERT_NE(bbPos, std::string::npos);
    EXPECT_EQ(actualFp.substr(bbPos), expectedFp.substr(bbPos));
}

void assertElementMapUsesHasher(const Data::ElementMapPtr& map, const App::StringHasherRef& hasher)
{
    ASSERT_TRUE(map);
    EXPECT_EQ(static_cast<App::StringHasher*>(map->hasher),
              static_cast<App::StringHasher*>(hasher));
    for (const auto& child : map->getChildElements()) {
        for (const auto& sid : child.sids) {
            EXPECT_TRUE(sid.isFromSameHasher(hasher));
            const App::StringIDRef resolved = hasher->getID(sid.value());
            ASSERT_TRUE(resolved) << "child SID must resolve from shared hasher";
            EXPECT_EQ(resolved.value(), sid.value());
        }
        if (child.elementMap) {
            assertElementMapUsesHasher(child.elementMap, hasher);
        }
    }
    for (const auto& el : map->getAll()) {
        Data::ElementIDRefs sids;
        (void)map->find(el.index, &sids);
        for (const auto& sid : sids) {
            EXPECT_TRUE(sid.isFromSameHasher(hasher));
            const App::StringIDRef resolved = hasher->getID(sid.value());
            ASSERT_TRUE(resolved) << "mapped SID must resolve from shared hasher";
            EXPECT_EQ(resolved.value(), sid.value());
        }
    }
}

void assertSweepBundleMapsAndSids(const Part::FrozenTopoShapeBundle& bundle,
                                  const App::StringHasherRef& hasher)
{
    ASSERT_TRUE(bundle.hasher);
    EXPECT_EQ(static_cast<App::StringHasher*>(bundle.hasher),
              static_cast<App::StringHasher*>(hasher));
    EXPECT_EQ(static_cast<App::StringHasher*>(bundle.shape.Hasher),
              static_cast<App::StringHasher*>(hasher));
    assertElementMapUsesHasher(bundle.elementMap, hasher);
}

bool makeSweepCodecOperandBundles(Part::FrozenTopoShapeBundle& spineOut,
                                  Part::FrozenTopoShapeBundle& profile0Out,
                                  Part::FrozenTopoShapeBundle& profile1Out,
                                  long* originalLongSidValueOut = nullptr)
{
    const auto spineEdge =
        BRepBuilderAPI_MakeEdge(gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(0.0, 0.0, 8.0)).Edge();
    const TopoDS_Wire spineWire = BRepBuilderAPI_MakeWire(spineEdge).Wire();
    const TopoDS_Shape profileShape0Solid = makeProfileBoxShape(5.0, 5.0, 1.0);
    const TopoDS_Shape profileShape1Solid = makeProfileBoxShape(3.0, 4.0, 1.0);

    App::StringHasherRef hasher(new App::StringHasher);
    hasher->setThreshold(8);
    hasher->advanceRevision();

    Part::TopoShape spineShape(spineWire, /*tag=*/1, hasher);
    Part::TopoShape profileShape0(profileShape0Solid, /*tag=*/2, hasher);
    Part::TopoShape profileShape1(profileShape1Solid, /*tag=*/3, hasher);

    auto spineMap = std::make_shared<Data::ElementMap>();
    spineMap->hasher = hasher;
    spineShape.resetElementMap(spineMap);

    auto profileMap0 = std::make_shared<Data::ElementMap>();
    profileMap0->hasher = hasher;
    profileShape0.resetElementMap(profileMap0);
    auto profileMap1 = std::make_shared<Data::ElementMap>();
    profileMap1->hasher = hasher;
    profileShape1.resetElementMap(profileMap1);

    Data::IndexedName spineEdgeName("Edge", 1);
    App::StringIDRef spineSeed = hasher->getID(QByteArray("SpineEd"));
    if (!spineSeed) {
        return false;
    }
    Data::MappedName spineMapped(QByteArray::fromStdString(spineSeed.toString()));
    Data::ElementIDRefs spineSids;
    spineSids.push_back(spineSeed);
    spineMap->setElementName(spineEdgeName, spineMapped, spineShape.Tag, &spineSids);
    spineSeed.mark();

    Data::IndexedName profileEdge0("Edge", 1);
    if (originalLongSidValueOut) {
        const QByteArray longName(
            "SweepProfileWithAVeryLongTopologicalNameThatExceedsTheHasherThreshold");
        if (static_cast<size_t>(longName.size())
            <= static_cast<size_t>(hasher->getThreshold())) {
            return false;
        }
        App::StringIDRef longSid = hasher->getID(longName, App::StringHasher::Option::Hashable);
        if (!longSid || !longSid.isHashed()) {
            return false;
        }
        *originalLongSidValueOut = longSid.value();
        Data::MappedName longMapped(QByteArray::fromStdString(longSid.toString()));
        Data::ElementIDRefs longSids;
        longSids.push_back(longSid);
        profileMap0->setElementName(profileEdge0, longMapped, profileShape0.Tag, &longSids);
        longSid.mark();
    }
    else {
        App::StringIDRef profileSeed0 = hasher->getID(QByteArray("Prof0Ed"));
        if (!profileSeed0) {
            return false;
        }
        Data::MappedName profileMapped0(QByteArray::fromStdString(profileSeed0.toString()));
        Data::ElementIDRefs profileSids0;
        profileSids0.push_back(profileSeed0);
        profileMap0->setElementName(profileEdge0, profileMapped0, profileShape0.Tag, &profileSids0);
        profileSeed0.mark();
    }

    Data::IndexedName profileEdge1("Edge", 1);
    App::StringIDRef profileSeed1 = hasher->getID(QByteArray("Prof1Ed"));
    if (!profileSeed1) {
        return false;
    }
    Data::MappedName profileMapped1(QByteArray::fromStdString(profileSeed1.toString()));
    Data::ElementIDRefs profileSids1;
    profileSids1.push_back(profileSeed1);
    profileMap1->setElementName(profileEdge1, profileMapped1, profileShape1.Tag, &profileSids1);
    profileSeed1.mark();

    spineOut = Part::TopoShapeArchive::createBundle(spineShape);
    profile0Out = Part::TopoShapeArchive::createBundle(profileShape0);
    profile1Out = Part::TopoShapeArchive::createBundle(profileShape1);
    return spineOut.valid && profile0Out.valid && profile1Out.valid;
}

} // namespace

TEST_F(NonBlockingGeometryTest, SweepCodecRoundTripPreservesOperandsProfilesAndFlags)
{
    Part::FrozenTopoShapeBundle spineBundle;
    Part::FrozenTopoShapeBundle profileBundle0;
    Part::FrozenTopoShapeBundle profileBundle1;
    long originalLongSidValue = 0;
    ASSERT_TRUE(makeSweepCodecOperandBundles(
        spineBundle, profileBundle0, profileBundle1, &originalLongSidValue));

    const QByteArray longName(
        "SweepProfileWithAVeryLongTopologicalNameThatExceedsTheHasherThreshold");

    const QString workDir = _tempDir->path() + QStringLiteral("/sweep_codec");
    App::GeometryRequestWorkspace workspace(workDir);
    Part::SweepGeometryOperation op(spineBundle, {profileBundle0, profileBundle1}, /*isSolid=*/true);
    ASSERT_TRUE(op.writeArchive(workspace).success);
    ASSERT_TRUE(workspace.publishRequestJson());
    ASSERT_TRUE(QFileInfo::exists(workDir + QStringLiteral("/spine.fcg")));
    ASSERT_TRUE(QFileInfo::exists(workDir + QStringLiteral("/profile-0.fcg")));
    ASSERT_TRUE(QFileInfo::exists(workDir + QStringLiteral("/profile-1.fcg")));
    ASSERT_TRUE(QFileInfo::exists(workDir + QStringLiteral("/request.json")));

    QFile reqFile(workDir + QStringLiteral("/request.json"));
    ASSERT_TRUE(reqFile.open(QIODevice::ReadOnly));
    const QJsonObject published = QJsonDocument::fromJson(reqFile.readAll()).object();
    reqFile.close();

    EXPECT_EQ(published.value(QStringLiteral("operationType")).toString(), QStringLiteral("Part::Sweep"));
    EXPECT_EQ(published.value(QStringLiteral("codecVersion")).toInt(), 1);
    EXPECT_TRUE(published.value(QStringLiteral("isSolid")).toBool());
    EXPECT_EQ(published.value(QStringLiteral("spinePath")).toString(), QStringLiteral("spine.fcg"));
    const QJsonArray publishedProfiles = published.value(QStringLiteral("profiles")).toArray();
    ASSERT_EQ(publishedProfiles.size(), 2);
    EXPECT_EQ(publishedProfiles.at(0).toObject().value(QStringLiteral("path")).toString(),
              QStringLiteral("profile-0.fcg"));
    EXPECT_EQ(publishedProfiles.at(1).toObject().value(QStringLiteral("path")).toString(),
              QStringLiteral("profile-1.fcg"));

    std::string errorCode;
    std::string errorMessage;
    auto decoded = Part::SweepGeometryOperation::decodeFromRequest(
        published, workDir, errorCode, errorMessage);
    ASSERT_TRUE(decoded) << errorCode << ": " << errorMessage;

    expectMatchingGeometryFingerprint(spineBundle, decoded->spineBundle());
    ASSERT_EQ(decoded->profileBundles().size(), 2u);
    expectMatchingGeometryFingerprint(profileBundle0, decoded->profileBundles()[0]);
    expectMatchingGeometryFingerprint(profileBundle1, decoded->profileBundles()[1]);
    EXPECT_TRUE(decoded->isSolid());

    App::StringHasherRef sharedHasher(decoded->spineBundle().hasher);
    assertSweepBundleMapsAndSids(decoded->spineBundle(), sharedHasher);
    for (const Part::FrozenTopoShapeBundle& profile : decoded->profileBundles()) {
        assertSweepBundleMapsAndSids(profile, sharedHasher);
    }

    Data::ElementIDRefs profile0Sids;
    const Data::IndexedName profile0EdgeName("Edge", 1);
    ASSERT_TRUE(decoded->profileBundles()[0].elementMap->find(profile0EdgeName, &profile0Sids));
    ASSERT_FALSE(profile0Sids.empty());
    EXPECT_TRUE(profile0Sids.front().isHashed());
    auto again = sharedHasher->getID(longName, App::StringHasher::Option::Hashable);
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value(), originalLongSidValue);
    EXPECT_EQ(profile0Sids.front().value(), originalLongSidValue);

    EXPECT_FALSE(QFileInfo::exists(workDir + QStringLiteral("/result.fcg")));
}

TEST_F(NonBlockingGeometryTest, SweepCodecRejectsBadRequestBeforeOcc)
{
    Part::FrozenTopoShapeBundle spineBundle;
    Part::FrozenTopoShapeBundle profileBundle0;
    Part::FrozenTopoShapeBundle profileBundle1;
    ASSERT_TRUE(makeSweepCodecOperandBundles(spineBundle, profileBundle0, profileBundle1));

    const QString workDir = _tempDir->path() + QStringLiteral("/sweep_reject");
    App::GeometryRequestWorkspace workspace(workDir);
    Part::SweepGeometryOperation op(spineBundle, {profileBundle0, profileBundle1}, /*isSolid=*/false);
    ASSERT_TRUE(op.writeArchive(workspace).success);
    ASSERT_TRUE(workspace.publishRequestJson());

    QFile reqFile(workDir + QStringLiteral("/request.json"));
    ASSERT_TRUE(reqFile.open(QIODevice::ReadOnly));
    QJsonObject good = QJsonDocument::fromJson(reqFile.readAll()).object();
    reqFile.close();

    const auto makeValidProfile = [&](const QString& path) {
        QJsonObject profile;
        profile.insert(QStringLiteral("path"), path);
        profile.insert(QStringLiteral("size"),
                       static_cast<qint64>(QFileInfo(workDir + QStringLiteral("/") + path).size()));
        profile.insert(QStringLiteral("sha256"),
                       QString::fromStdString(
                           Part::TopoShapeArchive::calculateSha256File(
                               (workDir + QStringLiteral("/") + path).toStdString())));
        return profile;
    };

    auto expectReject = [&](const QJsonObject& bad,
                            const char* expectedCode,
                            const QString& decodeDir = QString()) {
        const QString dir = decodeDir.isEmpty() ? workDir : decodeDir;
        std::string errorCode;
        std::string errorMessage;
        auto decoded = Part::SweepGeometryOperation::decodeFromRequest(bad, dir, errorCode, errorMessage);
        EXPECT_FALSE(decoded) << expectedCode;
        EXPECT_EQ(errorCode, expectedCode) << errorMessage;
        EXPECT_FALSE(QFileInfo::exists(dir + QStringLiteral("/result.fcg")));
    };

    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("operationType"), QStringLiteral("Part::Boolean"));
        expectReject(bad, "UnsupportedOperation");
    }
    {
        QJsonObject bad = good;
        bad.remove(QStringLiteral("codecVersion"));
        expectReject(bad, "MissingJsonField");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), QStringLiteral("1"));
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), 99);
        expectReject(bad, "UnsupportedCodecVersion");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), -1);
        expectReject(bad, "OutOfRangeJsonNumber");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), 4294967296.0);
        expectReject(bad, "OutOfRangeJsonNumber");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("codecVersion"), 4294967295.0);
        expectReject(bad, "UnsupportedCodecVersion");
    }
    {
        QJsonObject bad = good;
        bad.remove(QStringLiteral("isSolid"));
        expectReject(bad, "MissingJsonField");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("isSolid"), 1);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("isSolid"), QStringLiteral("true"));
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("isSolid"), QJsonValue::Null);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.remove(QStringLiteral("profiles"));
        expectReject(bad, "MissingJsonField");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("profiles"), QStringLiteral("not-an-array"));
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("profiles"), QJsonArray());
        expectReject(bad, "EmptyProfiles");
    }
    {
        QJsonObject bad = good;
        QJsonArray profiles;
        profiles.append(QStringLiteral("not-an-object"));
        bad.insert(QStringLiteral("profiles"), profiles);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("spinePath"), QStringLiteral("/tmp/escape.fcg"));
        expectReject(bad, "UntrustedOperandPath");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("spinePath"), QStringLiteral("../escape.fcg"));
        expectReject(bad, "UntrustedOperandPath");
    }
    {
        QJsonObject bad = good;
        QJsonArray profiles;
        profiles.append(makeValidProfile(QStringLiteral("profile-0.fcg")));
        profiles.append(makeValidProfile(QStringLiteral("profile-0.fcg")));
        bad.insert(QStringLiteral("profiles"), profiles);
        expectReject(bad, "DuplicateOperandPath");
    }
    {
        QJsonObject bad = good;
        QJsonArray profiles;
        profiles.append(makeValidProfile(QStringLiteral("spine.fcg")));
        profiles.append(makeValidProfile(QStringLiteral("profile-1.fcg")));
        bad.insert(QStringLiteral("profiles"), profiles);
        expectReject(bad, "DuplicateOperandPath");
    }
    {
        QJsonObject bad = good;
        QJsonArray profiles;
        QJsonObject aliasSpine = makeValidProfile(QStringLiteral("spine.fcg"));
        aliasSpine.insert(QStringLiteral("path"), QStringLiteral("./spine.fcg"));
        profiles.append(aliasSpine);
        profiles.append(makeValidProfile(QStringLiteral("profile-1.fcg")));
        bad.insert(QStringLiteral("profiles"), profiles);
        expectReject(bad, "DuplicateOperandPath");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("spinePath"), QStringLiteral("missing.fcg"));
        expectReject(bad, "MissingOperandArchive");
    }
    {
        QJsonObject bad = good;
        bad.remove(QStringLiteral("spineSize"));
        expectReject(bad, "MissingJsonField");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("spineSize"), QStringLiteral("100"));
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("spineSize"), 1.5);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("spineSize"), -1);
        expectReject(bad, "OperandSizeMismatch");
    }
    {
        QJsonObject bad = good;
        bad.insert(QStringLiteral("spineSize"),
                   bad.value(QStringLiteral("spineSize")).toVariant().toLongLong() + 1);
        expectReject(bad, "OperandSizeMismatch");
    }
    {
        QJsonObject bad = good;
        QString sha = bad.value(QStringLiteral("spineSha256")).toString();
        sha[0] = (sha[0] == QLatin1Char('a')) ? QLatin1Char('b') : QLatin1Char('a');
        bad.insert(QStringLiteral("spineSha256"), sha);
        expectReject(bad, "OperandDigestMismatch");
    }
    {
        QJsonObject bad = good;
        QJsonArray profiles;
        QJsonObject profile = makeValidProfile(QStringLiteral("profile-0.fcg"));
        QString sha = profile.value(QStringLiteral("sha256")).toString();
        sha[0] = (sha[0] == QLatin1Char('a')) ? QLatin1Char('b') : QLatin1Char('a');
        profile.insert(QStringLiteral("sha256"), sha);
        profiles.append(profile);
        profiles.append(makeValidProfile(QStringLiteral("profile-1.fcg")));
        bad.insert(QStringLiteral("profiles"), profiles);
        expectReject(bad, "OperandDigestMismatch");
    }
    {
        QJsonObject bad = good;
        QJsonArray profiles;
        QJsonObject profile = makeValidProfile(QStringLiteral("profile-0.fcg"));
        profile.remove(QStringLiteral("path"));
        profiles.append(profile);
        profiles.append(makeValidProfile(QStringLiteral("profile-1.fcg")));
        bad.insert(QStringLiteral("profiles"), profiles);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        QJsonArray profiles;
        QJsonObject profile = makeValidProfile(QStringLiteral("profile-0.fcg"));
        profile.insert(QStringLiteral("path"), QStringLiteral("../profile-0.fcg"));
        profiles.append(profile);
        profiles.append(makeValidProfile(QStringLiteral("profile-1.fcg")));
        bad.insert(QStringLiteral("profiles"), profiles);
        expectReject(bad, "UntrustedOperandPath");
    }
    {
        QJsonObject bad = good;
        QJsonArray profiles;
        QJsonObject profile = makeValidProfile(QStringLiteral("profile-0.fcg"));
        profile.remove(QStringLiteral("size"));
        profiles.append(profile);
        profiles.append(makeValidProfile(QStringLiteral("profile-1.fcg")));
        bad.insert(QStringLiteral("profiles"), profiles);
        expectReject(bad, "MissingJsonField");
    }
    {
        QJsonObject bad = good;
        QJsonArray profiles;
        QJsonObject profile = makeValidProfile(QStringLiteral("profile-0.fcg"));
        profile.insert(QStringLiteral("size"), 1.5);
        profiles.append(profile);
        profiles.append(makeValidProfile(QStringLiteral("profile-1.fcg")));
        bad.insert(QStringLiteral("profiles"), profiles);
        expectReject(bad, "WrongJsonType");
    }
    {
        QJsonObject bad = good;
        QJsonArray profiles;
        QJsonObject profile = makeValidProfile(QStringLiteral("profile-0.fcg"));
        profile.insert(QStringLiteral("size"), -1);
        profiles.append(profile);
        profiles.append(makeValidProfile(QStringLiteral("profile-1.fcg")));
        bad.insert(QStringLiteral("profiles"), profiles);
        expectReject(bad, "OperandSizeMismatch");
    }
    {
        QJsonObject bad = good;
        QJsonArray profiles;
        QJsonObject profile = makeValidProfile(QStringLiteral("profile-0.fcg"));
        profile.remove(QStringLiteral("sha256"));
        profiles.append(profile);
        profiles.append(makeValidProfile(QStringLiteral("profile-1.fcg")));
        bad.insert(QStringLiteral("profiles"), profiles);
        expectReject(bad, "WrongJsonType");
    }
    {
        const QString corruptDir = _tempDir->path() + QStringLiteral("/sweep_corrupt");
        QDir().mkpath(corruptDir);
        const QString corruptSpinePath = corruptDir + QStringLiteral("/spine.fcg");
        ASSERT_TRUE(QFile::copy(workDir + QStringLiteral("/spine.fcg"), corruptSpinePath));
        {
            std::ofstream ofs(corruptSpinePath.toStdString(), std::ios::binary | std::ios::app);
            ofs << "TRAIL";
        }
        QJsonObject bad = good;
        bad.insert(QStringLiteral("spinePath"), QStringLiteral("spine.fcg"));
        bad.insert(QStringLiteral("spineSize"), static_cast<qint64>(QFileInfo(corruptSpinePath).size()));
        bad.insert(QStringLiteral("spineSha256"),
                   QString::fromStdString(
                       Part::TopoShapeArchive::calculateSha256File(corruptSpinePath.toStdString())));
        expectReject(bad, "OperandDecodeFailed", corruptDir);
    }
    {
        const QString mismatchDir = _tempDir->path() + QStringLiteral("/sweep_threshold");
        QDir().mkpath(mismatchDir);
        ASSERT_TRUE(
            QFile::copy(workDir + QStringLiteral("/spine.fcg"), mismatchDir + QStringLiteral("/spine.fcg")));
        ASSERT_TRUE(QFile::copy(workDir + QStringLiteral("/profile-0.fcg"),
                                mismatchDir + QStringLiteral("/profile-0.fcg")));

        Part::FrozenTopoShapeBundle mismatchedProfileBundle;
        const std::string sourceProfile1 =
            (workDir + QStringLiteral("/profile-1.fcg")).toStdString();
        ASSERT_TRUE(
            Part::TopoShapeArchive::readArchive(sourceProfile1, mismatchedProfileBundle));
        ASSERT_TRUE(mismatchedProfileBundle.valid);
        mismatchedProfileBundle.hasherSnapshot.threshold = 20;
        if (mismatchedProfileBundle.hasher) {
            mismatchedProfileBundle.hasher->setThreshold(20);
        }
        const QString mismatchedProfilePath = mismatchDir + QStringLiteral("/profile-1.fcg");
        ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(mismatchedProfileBundle,
                                                         mismatchedProfilePath.toStdString()));

        const auto makeValidProfileInDir = [&](const QString& dir, const QString& path) {
            QJsonObject profile;
            profile.insert(QStringLiteral("path"), path);
            const QString abs = dir + QStringLiteral("/") + path;
            profile.insert(QStringLiteral("size"), static_cast<qint64>(QFileInfo(abs).size()));
            profile.insert(QStringLiteral("sha256"),
                           QString::fromStdString(
                               Part::TopoShapeArchive::calculateSha256File(abs.toStdString())));
            return profile;
        };

        QJsonObject bad = good;
        bad.insert(QStringLiteral("spinePath"), QStringLiteral("spine.fcg"));
        bad.insert(QStringLiteral("spineSize"),
                   static_cast<qint64>(
                       QFileInfo(mismatchDir + QStringLiteral("/spine.fcg")).size()));
        bad.insert(QStringLiteral("spineSha256"),
                   QString::fromStdString(Part::TopoShapeArchive::calculateSha256File(
                       (mismatchDir + QStringLiteral("/spine.fcg")).toStdString())));
        QJsonArray profiles;
        profiles.append(makeValidProfileInDir(mismatchDir, QStringLiteral("profile-0.fcg")));
        profiles.append(makeValidProfileInDir(mismatchDir, QStringLiteral("profile-1.fcg")));
        bad.insert(QStringLiteral("profiles"), profiles);
        expectReject(bad, "ThresholdMismatch", mismatchDir);
    }
}

TEST_F(NonBlockingGeometryTest, SweepCodecStagingFailureDoesNotPublish)
{
    Part::FrozenTopoShapeBundle spineBundle;
    Part::FrozenTopoShapeBundle profileBundle0;
    Part::FrozenTopoShapeBundle profileBundle1;
    ASSERT_TRUE(makeSweepCodecOperandBundles(spineBundle, profileBundle0, profileBundle1));
    Part::SweepGeometryOperation op(spineBundle, {profileBundle0, profileBundle1}, /*isSolid=*/false);

    struct StubArchiveWriter : App::GeometryArchiveWriter
    {
        void writeSection(const std::string&, const std::vector<uint8_t>&) override
        {
        }
        void writeString(const std::string&, const std::string&) override
        {
        }
        void writeBytes(const std::string&, const uint8_t*, size_t) override
        {
        }
    };
    StubArchiveWriter stub;
    const auto stubWrite = op.writeArchive(stub);
    EXPECT_FALSE(stubWrite.success);
    EXPECT_EQ(stubWrite.errorCode, "UnsupportedArchiveWriter");

    const QString blockedWs = _tempDir->path() + QStringLiteral("/sweep_stage_blocked");
    QDir().mkpath(blockedWs);
    QFile staleFile(blockedWs + QStringLiteral("/profile-1.fcg"));
    ASSERT_TRUE(staleFile.open(QIODevice::WriteOnly));
    staleFile.write("stale-operand");
    staleFile.close();
    ASSERT_TRUE(QDir().mkpath(blockedWs + QStringLiteral("/profile-1.fcg.tmp")));

    App::GeometryRequestWorkspace blocked(blockedWs);
    EXPECT_FALSE(blocked.hasFailed());
    const auto blockedWrite = op.writeArchive(blocked);
    EXPECT_FALSE(blockedWrite.success);
    EXPECT_TRUE(blocked.hasFailed());
    EXPECT_FALSE(blocked.publishRequestJson());
    EXPECT_FALSE(QFileInfo::exists(blockedWs + QStringLiteral("/request.json")));
    EXPECT_FALSE(QFileInfo::exists(blockedWs + QStringLiteral("/profile-1.fcg")))
        << "stale profile-1.fcg must be cleared and not accepted after failed staging";

    const QString reuseWs = _tempDir->path() + QStringLiteral("/sweep_reuse_empty");
    App::GeometryRequestWorkspace reuse(reuseWs);
    ASSERT_TRUE(op.writeArchive(reuse).success);
    ASSERT_TRUE(reuse.publishRequestJson());
    ASSERT_TRUE(QFileInfo::exists(reuseWs + QStringLiteral("/request.json")));

    Part::SweepGeometryOperation emptyProfilesOp(spineBundle, {});
    const auto emptyWrite = emptyProfilesOp.writeArchive(reuse);
    EXPECT_FALSE(emptyWrite.success);
    EXPECT_EQ(emptyWrite.errorCode, "NoProfiles");
    EXPECT_TRUE(reuse.hasFailed());
    EXPECT_FALSE(reuse.publishRequestJson());
    EXPECT_FALSE(QFileInfo::exists(reuseWs + QStringLiteral("/request.json")));
}

TEST_F(NonBlockingGeometryTest, FilletCodecStagingFailureDoesNotPublish)
{
    BRepPrimAPI_MakeBox box(5.0, 5.0, 5.0);
    Part::TopoShape shape(box.Shape());
    Part::FrozenTopoShapeBundle base = Part::TopoShapeArchive::createBundle(shape);
    ASSERT_TRUE(base.valid);
    Part::FilletGeometryOperation op(base, {{0, 1.0, 1.0}});

    struct StubArchiveWriter : App::GeometryArchiveWriter
    {
        void writeSection(const std::string&, const std::vector<uint8_t>&) override
        {
        }
        void writeString(const std::string&, const std::string&) override
        {
        }
        void writeBytes(const std::string&, const uint8_t*, size_t) override
        {
        }
    };
    StubArchiveWriter stub;
    const auto writeResult = op.writeArchive(stub);
    EXPECT_FALSE(writeResult.success);
    EXPECT_EQ(writeResult.errorCode, "UnsupportedArchiveWriter");

    const QString blockedWs = _tempDir->path() + QStringLiteral("/fillet_stage_blocked");
    QDir().mkpath(blockedWs);
    QDir().mkpath(blockedWs + QStringLiteral("/base.fcg"));
    App::GeometryRequestWorkspace blocked(blockedWs);
    EXPECT_FALSE(blocked.hasFailed());
    const auto blockedWrite = op.writeArchive(blocked);
    EXPECT_FALSE(blockedWrite.success);
    EXPECT_TRUE(blocked.hasFailed());
    EXPECT_FALSE(blocked.publishRequestJson());
    EXPECT_FALSE(QFileInfo::exists(blockedWs + QStringLiteral("/request.json")));
}

namespace
{

Part::FrozenTopoShapeBundle makeMappedBoxBundleForFillet()
{
    BRepPrimAPI_MakeBox box(10.0, 10.0, 10.0);
    App::StringHasherRef hasher(new App::StringHasher);
    Part::TopoShape shape(box.Shape(), /*tag=*/1, hasher);
    auto map = std::make_shared<Data::ElementMap>();
    map->hasher = hasher;
    shape.resetElementMap(map);
    Data::IndexedName edge1("Edge", 1);
    App::StringIDRef sid = hasher->getID(QByteArray("FilletWorkerSeedEdge"));
    if (!sid) {
        return {};
    }
    Data::MappedName mapped(sid);
    Data::ElementIDRefs sids;
    sids.push_back(sid);
    map->setElementName(edge1, mapped, shape.Tag, &sids);
    sid.mark();
    Part::FrozenTopoShapeBundle base = Part::TopoShapeArchive::createBundle(shape);
    EXPECT_TRUE(base.valid);
    return base;
}

bool publishFilletWorkerRequest(const QString& workDir,
                                const Part::FrozenTopoShapeBundle& base,
                                App::GeometryJobId jobId)
{
    std::string jobWire;
    if (!App::formatGeometryJobId(jobId, jobWire)) {
        return false;
    }
    App::GeometryRequestWorkspace workspace(workDir);
    workspace.requestObject().insert(QStringLiteral("jobId"), QString::fromStdString(jobWire));
    Part::FilletGeometryOperation op(base, {{0, 1.0, 1.0}});
    if (!op.writeArchive(workspace).success) {
        return false;
    }
    return workspace.publishRequestJson();
}

} // namespace

TEST_F(NonBlockingGeometryTest, FilletWorkerProcessProducesMappedResult)
{
    Part::GeometryWorkerRegistry::instance().registerBuiltins();
    const Part::FrozenTopoShapeBundle base = makeMappedBoxBundleForFillet();
    ASSERT_TRUE(base.valid);
    ASSERT_TRUE(base.elementMap);
    EXPECT_GT(base.elementMap->size(), 0u);

    const QString workDir = _tempDir->path() + QStringLiteral("/fillet_worker_ok");
    constexpr App::GeometryJobId kJobId = 25;
    ASSERT_TRUE(publishFilletWorkerRequest(workDir, base, kJobId));
    const QString requestPath = workDir + QStringLiteral("/request.json");
    ASSERT_TRUE(QFileInfo::exists(requestPath));

    const int exitCode =
        Part::GeometryWorker::runWorkerProcess(requestPath.toStdString());
    EXPECT_EQ(exitCode, 0);

    const QString resultPath = workDir + QStringLiteral("/result.fcg");
    ASSERT_TRUE(QFileInfo::exists(resultPath));

    Part::FilletGeometryOperation filletOp(base, {{0, 1.0, 1.0}});
    const App::DetachedGeometryResult decoded =
        filletOp.decodeResultArchive(resultPath.toStdString());
    ASSERT_TRUE(decoded.success) << decoded.errorCode << ": " << decoded.errorMessage;

    Part::FrozenTopoShapeBundle outBundle;
    ASSERT_TRUE(Part::TopoShapeArchive::readArchive(resultPath.toStdString(), outBundle));
    EXPECT_TRUE(outBundle.valid);
    EXPECT_FALSE(outBundle.shape.isNull());
    ASSERT_TRUE(outBundle.hasher);
    ASSERT_TRUE(outBundle.elementMap);
    EXPECT_GT(outBundle.elementMap->size(), 0u);
    EXPECT_FALSE(outBundle.hasherSnapshot.entries.empty());
    EXPECT_EQ(static_cast<App::StringHasher*>(outBundle.shape.Hasher),
              static_cast<App::StringHasher*>(outBundle.hasher));
    EXPECT_EQ(outBundle.elementMap->hasher, outBundle.hasher);
}

TEST_F(NonBlockingGeometryTest, FilletWorkerProcessRecoveryAfterBadRequest)
{
    Part::GeometryWorkerRegistry::instance().registerBuiltins();
    const Part::FrozenTopoShapeBundle base = makeMappedBoxBundleForFillet();
    constexpr App::GeometryJobId kJobId = 26;
    std::string jobWire;
    ASSERT_TRUE(App::formatGeometryJobId(kJobId, jobWire));

    const QString badDir = _tempDir->path() + QStringLiteral("/fillet_worker_bad");
    ASSERT_TRUE(publishFilletWorkerRequest(badDir, base, kJobId));
    ASSERT_TRUE(QFile::remove(badDir + QStringLiteral("/base.fcg")));

    const int badExit =
        Part::GeometryWorker::runWorkerProcess((badDir + QStringLiteral("/request.json")).toStdString());
    EXPECT_EQ(badExit, 2);
    EXPECT_FALSE(QFileInfo::exists(badDir + QStringLiteral("/result.fcg")));

    const QString goodDir = _tempDir->path() + QStringLiteral("/fillet_worker_good");
    ASSERT_TRUE(publishFilletWorkerRequest(goodDir, base, kJobId));
    const int goodExit =
        Part::GeometryWorker::runWorkerProcess((goodDir + QStringLiteral("/request.json")).toStdString());
    ASSERT_EQ(goodExit, 0);
    const QString resultPath = goodDir + QStringLiteral("/result.fcg");
    ASSERT_TRUE(QFileInfo::exists(resultPath));

    Part::FilletGeometryOperation filletOp(base, {{0, 1.0, 1.0}});
    const App::DetachedGeometryResult decoded =
        filletOp.decodeResultArchive(resultPath.toStdString());
    EXPECT_TRUE(decoded.success) << decoded.errorCode << ": " << decoded.errorMessage;
}

TEST_F(NonBlockingGeometryTest, FilletResultDecodeRejectsBadArchives)
{
    const Part::FrozenTopoShapeBundle base = makeMappedBoxBundleForFillet();
    Part::FilletGeometryOperation filletOp(base, {{0, 1.0, 1.0}});
    TestWorkerContext ctx(_tempDir->path().toStdString());
    const App::DetachedGeometryResult runResult = filletOp.run(ctx);
    ASSERT_TRUE(runResult.success) << runResult.errorCode;
    const std::string goodPath = runResult.resultArchivePath;
    ASSERT_FALSE(goodPath.empty());
    ASSERT_TRUE(filletOp.decodeResultArchive(goodPath).success);

    const std::string corruptPath = (_tempDir->path() + QStringLiteral("/fillet_corrupt_result.fcg")).toStdString();
    {
        QFile in(QString::fromStdString(goodPath));
        ASSERT_TRUE(in.open(QIODevice::ReadOnly));
        const QByteArray bytes = in.readAll();
        in.close();
        QFile out(QString::fromStdString(corruptPath));
        ASSERT_TRUE(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
        out.write(bytes);
        out.write("TRAIL");
        out.close();
    }
    {
        const App::DetachedGeometryResult bad = filletOp.decodeResultArchive(corruptPath);
        EXPECT_FALSE(bad.success);
        EXPECT_EQ(bad.errorCode, "ResultDecodeFailed");
        EXPECT_NE(bad.errorMessage.find("Fillet"), std::string::npos);
    }

    const std::string unmappedPath =
        (_tempDir->path() + QStringLiteral("/fillet_unmapped_result.fcg")).toStdString();
    {
        BRepPrimAPI_MakeBox box(5.0, 5.0, 5.0);
        Part::TopoShape plain(box.Shape());
        Part::FrozenTopoShapeBundle plainBundle = Part::TopoShapeArchive::createBundle(plain);
        ASSERT_TRUE(plainBundle.valid);
        ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(plainBundle, unmappedPath));
    }
    {
        const App::DetachedGeometryResult bad = filletOp.decodeResultArchive(unmappedPath);
        EXPECT_FALSE(bad.success);
        EXPECT_EQ(bad.errorCode, "MissingResultElementMap");
        EXPECT_NE(bad.errorMessage.find("Fillet"), std::string::npos);
    }

    const std::string missingPath =
        (_tempDir->path() + QStringLiteral("/fillet_missing_result.fcg")).toStdString();
    {
        const App::DetachedGeometryResult bad = filletOp.decodeResultArchive(missingPath);
        EXPECT_FALSE(bad.success);
        EXPECT_FALSE(bad.errorCode.empty());
    }
}

TEST_F(NonBlockingGeometryTest, ReadArchiveRejectsTrailingDataWithoutPublishing)
{
    BRepPrimAPI_MakeBox mkBox(5.0, 5.0, 5.0);
    Part::TopoShape shape(mkBox.Shape());
    auto bundle = Part::TopoShapeArchive::createBundle(shape);
    ASSERT_TRUE(bundle.valid);
    const std::string path = (_tempDir->path() + "/trail.fcg").toStdString();
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(bundle, path));
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::app);
        ofs << "TRAILING";
    }
    Part::FrozenTopoShapeBundle out;
    out.valid = false;
    out.shapeTag = 0;
    EXPECT_FALSE(Part::TopoShapeArchive::readArchive(path, out));
    // Failure must not publish a candidate into the caller's bundle.
    EXPECT_FALSE(out.valid);
    EXPECT_EQ(out.shapeTag, 0);
    EXPECT_TRUE(out.shape.isNull());
    EXPECT_FALSE(out.elementMap);
}

TEST_F(NonBlockingGeometryTest, HasherEmbeddedNulValueKeysAreDistinct)
{
    App::StringHasherRef hasher(new App::StringHasher);
    App::StringHasherClosure closure;
    closure.threshold = 0;
    // Distinct pairs that collide under data+NUL+postfix concatenation.
    QByteArray dataA("ab");
    dataA.append('\0');
    dataA.append('c');
    QByteArray postfixA("d");
    QByteArray dataB("ab");
    QByteArray postfixB;
    postfixB.append('c');
    postfixB.append('\0');
    postfixB.append('d');
    // dataA+"\\0"+postfixA == "ab\\0c\\0d" == dataB+"\\0"+postfixB
    closure.entries.push_back({1, 0u, dataA, postfixA, {}});
    closure.entries.push_back({2, 0u, dataB, postfixB, {}});

    auto merged = hasher->materializeExactClosure(closure);
    ASSERT_TRUE(merged.success) << merged.errorCode << ": " << merged.errorMessage;
    EXPECT_TRUE(hasher->getID(1));
    EXPECT_TRUE(hasher->getID(2));
    EXPECT_NE(hasher->getID(1).dataToText(), hasher->getID(2).dataToText());
}

TEST_F(NonBlockingGeometryTest, HasherIdenticalBinaryValueUnderDifferentIdsRejected)
{
    App::StringHasherRef hasher(new App::StringHasher);
    const size_t sizeBefore = hasher->size();
    App::StringHasherClosure closure;
    closure.threshold = 0;
    QByteArray data("x");
    data.append('\0');
    data.append('y');
    QByteArray postfix("z");
    closure.entries.push_back({1, 0u, data, postfix, {}});
    closure.entries.push_back({2, 0u, data, postfix, {}});
    auto merged = hasher->mergeExactClosure(closure, 0);
    EXPECT_FALSE(merged.success);
    EXPECT_EQ(merged.errorCode, "ValueCollision");
    EXPECT_EQ(hasher->size(), sizeBefore);
}

TEST_F(NonBlockingGeometryTest, RebindBundleLeavesSourceMarksUnchanged)
{
    App::StringHasherRef source(new App::StringHasher);
    BRepPrimAPI_MakeBox mkBox(4.0, 4.0, 4.0);
    Part::TopoShape shape(mkBox.Shape(), /*tag=*/7, source);
    auto map = std::make_shared<Data::ElementMap>();
    map->hasher = source;
    App::StringIDRef sid = source->getID(QByteArray("MarkProbe"));
    Data::MappedName mapped(sid);
    Data::ElementIDRefs sids;
    sids.push_back(sid);
    map->setElementName(Data::IndexedName("Face", 1), mapped, shape.Tag, &sids);
    sid.mark();
    shape.resetElementMap(map);

    auto bundle = Part::TopoShapeArchive::createBundle(shape);
    ASSERT_TRUE(bundle.valid);
    ASSERT_TRUE(bundle.elementMap);
    ASSERT_TRUE(bundle.hasher);

    const auto sourceMarksBefore = source->snapshotMarks();
    const auto bundleMarksBefore = bundle.hasher->snapshotMarks();
    std::unordered_map<const Data::ElementMap*, unsigned> idsBefore;
    bundle.elementMap->snapshotArchiveIds(idsBefore);

    App::StringHasherRef shared(new App::StringHasher);
    ASSERT_TRUE(shared->materializeExactClosure(bundle.hasherSnapshot).success);
    Part::FrozenTopoShapeBundle rebound = bundle;
    ASSERT_TRUE(Part::TopoShapeArchive::rebindBundleToHasher(rebound, shared));

    EXPECT_EQ(bundle.hasher->snapshotMarks(), bundleMarksBefore);
    EXPECT_EQ(source->snapshotMarks(), sourceMarksBefore);
    std::unordered_map<const Data::ElementMap*, unsigned> idsAfter;
    bundle.elementMap->snapshotArchiveIds(idsAfter);
    EXPECT_EQ(idsAfter, idsBefore);
    EXPECT_TRUE(sid.isMarked());

    const std::string path1 = (_tempDir->path() + "/rebind1.fcg").toStdString();
    const std::string path2 = (_tempDir->path() + "/rebind2.fcg").toStdString();
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(rebound, path1));
    ASSERT_TRUE(Part::TopoShapeArchive::rebindBundleToHasher(rebound, shared));
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(rebound, path2));
    EXPECT_EQ(bundle.hasher->snapshotMarks(), bundleMarksBefore);
    EXPECT_EQ(source->snapshotMarks(), sourceMarksBefore);
}

TEST_F(NonBlockingGeometryTest, RebindFailureRestoresSourceState)
{
    App::StringHasherRef source(new App::StringHasher);
    BRepPrimAPI_MakeBox mkBox(3.0, 3.0, 3.0);
    Part::TopoShape shape(mkBox.Shape(), /*tag=*/5, source);
    auto map = std::make_shared<Data::ElementMap>();
    map->hasher = source;
    App::StringIDRef sid = source->getID(QByteArray("FailRebindSid"));
    Data::MappedName mapped(sid);
    Data::ElementIDRefs sids;
    sids.push_back(sid);
    map->setElementName(Data::IndexedName("Face", 1), mapped, shape.Tag, &sids);
    sid.mark();
    shape.resetElementMap(map);

    auto bundle = Part::TopoShapeArchive::createBundle(shape);
    ASSERT_TRUE(bundle.valid);
    auto marksBefore = bundle.hasher->snapshotMarks();
    std::unordered_map<const Data::ElementMap*, unsigned> idsBefore;
    bundle.elementMap->snapshotArchiveIds(idsBefore);
    const auto* sourceMapPtr = bundle.elementMap.get();
    const auto* sourceHasherPtr = static_cast<App::StringHasher*>(bundle.hasher);

    // Target hasher deliberately missing required SIDs (empty) → rebind restore fails.
    App::StringHasherRef emptyTarget(new App::StringHasher);
    Part::FrozenTopoShapeBundle attempted = bundle;
    EXPECT_FALSE(Part::TopoShapeArchive::rebindBundleToHasher(attempted, emptyTarget));

    EXPECT_EQ(bundle.elementMap.get(), sourceMapPtr);
    EXPECT_EQ(static_cast<App::StringHasher*>(bundle.hasher), sourceHasherPtr);
    EXPECT_EQ(bundle.hasher->snapshotMarks(), marksBefore);
    std::unordered_map<const Data::ElementMap*, unsigned> idsAfter;
    bundle.elementMap->snapshotArchiveIds(idsAfter);
    EXPECT_EQ(idsAfter, idsBefore);
    EXPECT_TRUE(sid.isMarked());
    // Failed rebind must not publish a rebound map onto the caller's source bundle.
    EXPECT_EQ(static_cast<App::StringHasher*>(bundle.shape.Hasher), sourceHasherPtr);
}

TEST(TopoShapeArchiveInt64ToLongTest, RejectsValueBelowLongMin)
{
    if constexpr (sizeof(long) >= sizeof(int64_t)) {
        GTEST_SKIP() << "LP64: long and int64 share the same lower bound; narrowing is not observable";
    }
    else {
        long out = 0;
        const int64_t below = static_cast<int64_t>(std::numeric_limits<long>::min()) - 1;
        EXPECT_FALSE(Part::TopoShapeArchive::int64ToLongChecked(below, out));
    }
}

TEST(TopoShapeArchiveInt64ToLongTest, RejectsValueAboveLongMax)
{
    if constexpr (sizeof(long) >= sizeof(int64_t)) {
        GTEST_SKIP() << "LP64: long and int64 share the same upper bound; narrowing is not observable";
    }
    else {
        long out = 0;
        const int64_t above = static_cast<int64_t>(std::numeric_limits<long>::max()) + 1;
        EXPECT_FALSE(Part::TopoShapeArchive::int64ToLongChecked(above, out));
    }
}

TEST(TopoShapeArchiveInt64ToLongTest, AcceptsLongMax)
{
    long out = 0;
    const int64_t atMax = static_cast<int64_t>(std::numeric_limits<long>::max());
    EXPECT_TRUE(Part::TopoShapeArchive::int64ToLongChecked(atMax, out));
    EXPECT_EQ(out, std::numeric_limits<long>::max());
}

#ifdef FC_TOPOSHape_ARCHIVE_TEST_SEAMS

namespace
{

struct ForceClosureCaptureFailureGuard
{
    ForceClosureCaptureFailureGuard()
    {
        Part::TopoShapeArchive::setTestForceClosureCaptureFailure(true);
    }
    ~ForceClosureCaptureFailureGuard()
    {
        Part::TopoShapeArchive::setTestForceClosureCaptureFailure(false);
    }
};

} // namespace

TEST_F(NonBlockingGeometryTest, LiveClosureCaptureFailureDoesNotPublishArchive)
{
    App::StringHasherRef source(new App::StringHasher);
    BRepPrimAPI_MakeBox mkBox(2.0, 2.0, 2.0);
    Part::TopoShape shape(mkBox.Shape(), 1, source);
    auto map = std::make_shared<Data::ElementMap>();
    map->hasher = source;
    App::StringIDRef sid = source->getID(QByteArray("LiveMark"));
    Data::MappedName mapped(sid);
    Data::ElementIDRefs sids;
    sids.push_back(sid);
    map->setElementName(Data::IndexedName("Face", 1), mapped, shape.Tag, &sids);
    sid.mark();
    shape.resetElementMap(map);

    auto bundle = Part::TopoShapeArchive::createBundle(shape);
    ASSERT_TRUE(bundle.valid);
    const auto marksBefore = bundle.hasher->snapshotMarks();
    std::unordered_map<const Data::ElementMap*, unsigned> idsBefore;
    bundle.elementMap->snapshotArchiveIds(idsBefore);

    const std::string dest = (_tempDir->path() + "/live_closure.fcg").toStdString();
    const std::string seed = (_tempDir->path() + "/seed.fcg").toStdString();
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(bundle, seed));
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(bundle, dest));

    QFile destBefore(QString::fromStdString(dest));
    ASSERT_TRUE(destBefore.open(QIODevice::ReadOnly));
    const QByteArray destBytesBefore = destBefore.readAll();
    destBefore.close();

    {
        ForceClosureCaptureFailureGuard guard;
        EXPECT_FALSE(Part::TopoShapeArchive::writeArchive(bundle, dest));
    }

    EXPECT_FALSE(QFileInfo::exists(QString::fromStdString(dest + ".tmp")));
    EXPECT_EQ(bundle.hasher->snapshotMarks(), marksBefore);
    std::unordered_map<const Data::ElementMap*, unsigned> idsAfter;
    bundle.elementMap->snapshotArchiveIds(idsAfter);
    EXPECT_EQ(idsAfter, idsBefore);

    QFile destAfter(QString::fromStdString(dest));
    ASSERT_TRUE(destAfter.open(QIODevice::ReadOnly));
    EXPECT_EQ(destAfter.readAll(), destBytesBefore)
        << "failed capture must leave existing destination byte-identical";

    QFile seedFile(QString::fromStdString(seed));
    ASSERT_TRUE(seedFile.open(QIODevice::ReadOnly));
    const QByteArray seedBytes = seedFile.readAll();
    seedFile.close();
    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(bundle, dest));
    QFile outFile(QString::fromStdString(dest));
    ASSERT_TRUE(outFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(outFile.readAll(), seedBytes);
}

#endif // FC_TOPOSHape_ARCHIVE_TEST_SEAMS

TEST_F(NonBlockingGeometryTest, DuplicateChildInsertionDoesNotCrashReaders)
{
    App::StringHasherRef hasher(new App::StringHasher);
    auto map = std::make_shared<Data::ElementMap>();
    map->hasher = hasher;
    auto leaf = std::make_shared<Data::ElementMap>();
    leaf->hasher = hasher;
    Data::ElementIDRefs empty;
    const Data::ElementMap::MappedChildElements child {Data::IndexedName("Face", 1),
                                                      6,
                                                      0,
                                                      1,
                                                      leaf,
                                                      QByteArray("dupChild"),
                                                      empty};
    map->addChildElements(1, {child, child});
    EXPECT_NO_THROW({
        (void)map->getChildElements();
        (void)map->getAll();
    });
    App::StringHasherRef shared(new App::StringHasher);
    Part::FrozenTopoShapeBundle bundle;
    bundle.valid = true;
    bundle.elementMap = map;
    bundle.hasher = hasher;
    bundle.shapeTag = 1;
    EXPECT_NO_THROW({
        Part::TopoShapeArchive::rebindBundleToHasher(bundle, shared);
    });
}
