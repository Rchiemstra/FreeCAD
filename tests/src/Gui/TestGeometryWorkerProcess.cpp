// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Gui/GeometryWorkerProcess.h>
#include <App/Application.h>
#include <App/ElementMap.h>
#include <App/GeometryJob.h>
#include <App/GeometryJobManager.h>
#include <App/GeometryRequestWorkspace.h>
#include <App/IndexedName.h>
#include <App/MappedName.h>
#include <App/StringHasher.h>
#include <Mod/Part/App/BooleanGeometryOperation.h>
#include <Mod/Part/App/FilletGeometryOperation.h>
#include <Mod/Part/App/TopoShapeArchive.h>
#include <src/App/InitApplication.h>

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <gp_Pnt.hxx>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QTemporaryDir>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace
{

QString findFreeCADCmd()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QStringLiteral("/code/build_docker/bin/FreeCADCmd"),
        appDir + QStringLiteral("/../bin/FreeCADCmd"),
        appDir + QStringLiteral("/FreeCADCmd"),
    };
    for (const QString& path : candidates) {
        if (QFileInfo::exists(path)) {
            return QFileInfo(path).absoluteFilePath();
        }
    }
    return {};
}

QString findGeometryWorkerPy()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QStringLiteral("/code/build_docker/Mod/Part/GeometryWorker.py"),
        appDir + QStringLiteral("/../Mod/Part/GeometryWorker.py"),
        appDir + QStringLiteral("/Mod/Part/GeometryWorker.py"),
    };
    for (const QString& path : candidates) {
        if (QFileInfo::exists(path)) {
            return QFileInfo(path).absoluteFilePath();
        }
    }
    return {};
}

std::shared_ptr<Part::BooleanGeometryOperation> makeMappedBooleanTask(int threshold,
                                                                      long* hashedIdOut)
{
    App::StringHasherRef hasher(new App::StringHasher);
    hasher->setThreshold(threshold);
    BRepPrimAPI_MakeBox box1(10.0, 10.0, 10.0);
    BRepPrimAPI_MakeBox box2(gp_Pnt(5, 5, 5), 10.0, 10.0, 10.0);
    Part::TopoShape shape1(box1.Shape(), 1, hasher);
    Part::TopoShape shape2(box2.Shape(), 2, hasher);

    auto sharedLeaf = std::make_shared<Data::ElementMap>();
    sharedLeaf->hasher = hasher;
    for (int i = 1; i <= 6; ++i) {
        App::StringIDRef sid = hasher->getID(QByteArray("SharedEdge") + QByteArray::number(i));
        Data::MappedName mapped(sid);
        Data::ElementIDRefs sids;
        sids.push_back(sid);
        sharedLeaf->setElementName(Data::IndexedName("Edge", i), mapped, 11, &sids);
        sid.mark();
    }
    auto intermediate = std::make_shared<Data::ElementMap>();
    intermediate->hasher = hasher;
    Data::ElementIDRefs empty;
    intermediate->addChildElements(
        21,
        {{Data::IndexedName("Face", 1), 6, 0, 11, sharedLeaf, QByteArray("interSharedx"), empty},
         {Data::IndexedName("Face", 7), 6, 0, 11, sharedLeaf, QByteArray("interSharedy"), empty}});

    {
        auto map = std::make_shared<Data::ElementMap>();
        map->hasher = hasher;
        App::StringIDRef sid = hasher->getID(QByteArray("SeedFaceA"));
        Data::MappedName mapped(sid);
        Data::ElementIDRefs sids;
        sids.push_back(sid);
        map->setElementName(Data::IndexedName("Face", 1), mapped, 1, &sids);
        sid.mark();
        const QByteArray longName(
            "MappedFaceNameThatDefinitelyExceedsTheSmallHashThresholdXX");
        App::StringIDRef longSid =
            hasher->getID(longName, App::StringHasher::Option::Hashable);
        if (hashedIdOut) {
            *hashedIdOut = longSid.value();
        }
        Data::MappedName longMapped(QByteArray::fromStdString(longSid.toString()));
        Data::ElementIDRefs longSids;
        longSids.push_back(longSid);
        map->setElementName(Data::IndexedName("Face", 2), longMapped, 1, &longSids);
        longSid.mark();
        map->addChildElements(
            1,
            {{Data::IndexedName("Face", 20), 6, 0, 21, intermediate, QByteArray("parentInter1"), empty}});
        shape1.resetElementMap(map);
    }
    {
        auto map = std::make_shared<Data::ElementMap>();
        map->hasher = hasher;
        App::StringIDRef sid = hasher->getID(QByteArray("SeedFaceB"));
        Data::MappedName mapped(sid);
        Data::ElementIDRefs sids;
        sids.push_back(sid);
        map->setElementName(Data::IndexedName("Face", 1), mapped, 2, &sids);
        sid.mark();
        shape2.resetElementMap(map);
    }

    auto b1 = Part::TopoShapeArchive::createBundle(shape1);
    auto b2 = Part::TopoShapeArchive::createBundle(shape2);
    return std::make_shared<Part::BooleanGeometryOperation>(Part::BooleanType::Fuse, b1, b2);
}

struct MappedFilletBox
{
    Part::FrozenTopoShapeBundle base;
    long hashedId {0};
    QByteArray longName;
    QByteArray seedName;
    int threshold {16};
};

MappedFilletBox makeMappedFilletBox()
{
    MappedFilletBox out;
    out.threshold = 16;
    out.longName = QByteArray("MappedEdgeNameThatDefinitelyExceedsTheSmallHashThresholdXX");
    out.seedName = QByteArray("FilletCrossProcessSeedEdge");

    App::StringHasherRef hasher(new App::StringHasher);
    hasher->setThreshold(out.threshold);

    BRepPrimAPI_MakeBox box(10.0, 10.0, 10.0);
    Part::TopoShape shape(box.Shape(), /*tag=*/1, hasher);
    auto map = std::make_shared<Data::ElementMap>();
    map->hasher = hasher;

    App::StringIDRef seedSid = hasher->getID(out.seedName);
    if (!seedSid) {
        return {};
    }
    Data::MappedName seedMapped(seedSid);
    Data::ElementIDRefs seedSids;
    seedSids.push_back(seedSid);
    map->setElementName(Data::IndexedName("Edge", 1), seedMapped, shape.Tag, &seedSids);
    seedSid.mark();

    App::StringIDRef longSid = hasher->getID(out.longName, App::StringHasher::Option::Hashable);
    if (!longSid || !longSid.isHashed()) {
        return {};
    }
    out.hashedId = longSid.value();
    Data::MappedName longMapped(QByteArray::fromStdString(longSid.toString()));
    Data::ElementIDRefs longSids;
    longSids.push_back(longSid);
    map->setElementName(Data::IndexedName("Edge", 2), longMapped, shape.Tag, &longSids);
    longSid.mark();

    shape.resetElementMap(map);
    out.base = Part::TopoShapeArchive::createBundle(shape);
    return out;
}

bool allMapsUseHasher(const Data::ElementMapPtr& map, const App::StringHasherRef& hasher)
{
    if (!map) {
        return true;
    }
    if (static_cast<App::StringHasher*>(map->hasher)
        != static_cast<App::StringHasher*>(hasher)) {
        return false;
    }
    for (const auto& child : map->getChildElements()) {
        for (const auto& sid : child.sids) {
            if (!sid.isFromSameHasher(hasher)) {
                return false;
            }
        }
        if (!allMapsUseHasher(child.elementMap, hasher)) {
            return false;
        }
    }
    for (const auto& el : map->getAll()) {
        Data::ElementIDRefs sids;
        (void)map->find(el.index, &sids);
        for (const auto& sid : sids) {
            if (!sid.isFromSameHasher(hasher)) {
                return false;
            }
        }
    }
    return true;
}

void assertMappedFilletResult(const Part::FrozenTopoShapeBundle& out,
                              long hashedId,
                              const QByteArray& longName,
                              const QByteArray& seedName,
                              int expectedThreshold)
{
    ASSERT_TRUE(out.valid);
    ASSERT_FALSE(out.shape.isNull());
    ASSERT_TRUE(out.elementMap);
    ASSERT_TRUE(out.hasher);
    EXPECT_GT(out.elementMap->size(), 0u);
    EXPECT_FALSE(out.hasherSnapshot.entries.empty());
    EXPECT_EQ(static_cast<App::StringHasher*>(out.shape.Hasher),
              static_cast<App::StringHasher*>(out.hasher));
    EXPECT_EQ(out.hasher->getThreshold(), expectedThreshold);
    EXPECT_TRUE(allMapsUseHasher(out.elementMap, out.hasher));
    EXPECT_EQ(static_cast<App::StringHasher*>(out.elementMap->hasher),
              static_cast<App::StringHasher*>(out.hasher));

    auto again = out.hasher->getID(longName, App::StringHasher::Option::Hashable);
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value(), hashedId);
    EXPECT_TRUE(again.isHashed());

    auto seedSid = out.hasher->getID(seedName);
    ASSERT_TRUE(seedSid);
    EXPECT_TRUE(seedSid.isFromSameHasher(out.hasher));
    EXPECT_TRUE(out.hasher->getID(seedSid.value()));

    GProp_GProps props;
    BRepGProp::VolumeProperties(out.shape.getShape(), props);
    EXPECT_GT(props.Mass(), 900.0);
    EXPECT_LT(props.Mass(), 1100.0);

    std::function<void(const Data::ElementMapPtr&)> assertSidsResolve =
        [&](const Data::ElementMapPtr& em) {
            if (!em) {
                return;
            }
            EXPECT_EQ(static_cast<App::StringHasher*>(em->hasher),
                      static_cast<App::StringHasher*>(out.hasher));
            for (const auto& el : em->getAll()) {
                Data::ElementIDRefs sids;
                auto mapped = em->find(el.index, &sids);
                for (const auto& ref : sids) {
                    ASSERT_TRUE(ref);
                    EXPECT_TRUE(ref.isFromSameHasher(out.hasher));
                    EXPECT_TRUE(out.hasher->getID(ref.value()));
                }
                if (mapped) {
                    out.shape.traceElement(mapped, [&](const Data::MappedName&, int, long, long) {
                        return true;
                    });
                }
            }
            for (const auto& child : em->getChildElements()) {
                assertSidsResolve(child.elementMap);
            }
        };
    assertSidsResolve(out.elementMap);

    bool sawSourceTag = false;
    bool historyWalked = false;
    for (const auto& el : out.elementMap->getAll()) {
        Data::ElementIDRefs sids;
        auto mapped = out.elementMap->find(el.index, &sids);
        if (!mapped) {
            continue;
        }
        out.shape.traceElement(mapped, [&](const Data::MappedName&, int, long tag, long) {
            if (tag == 1) {
                sawSourceTag = true;
            }
            return true;
        });
        const std::string mappedText = mapped.toString();
        if (!mappedText.empty()) {
            historyWalked = true;
        }
    }
    EXPECT_TRUE(historyWalked);
    EXPECT_TRUE(sawSourceTag);
}

std::shared_ptr<Part::FilletGeometryOperation> makeMappedFilletTask(MappedFilletBox* boxOut = nullptr)
{
    const MappedFilletBox mapped = makeMappedFilletBox();
    if (boxOut) {
        *boxOut = mapped;
    }
    if (!mapped.base.valid) {
        return {};
    }
    return std::make_shared<Part::FilletGeometryOperation>(mapped.base,
                                                           std::vector<Part::FilletEdgeSpec>{{0, 1.0, 1.0}});
}

QString helloLine()
{
    return QStringLiteral(
        "FCGEO/1 {\"type\":\"hello\",\"protocol\":\"FCGEO/1\",\"version\":\"1.0\"}");
}

} // namespace

class GeometryWorkerProcessTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char arg0[] = "Gui_tests_run";
            static char* argv[] = {arg0, nullptr};
            new QCoreApplication(argc, argv);
        }
    }

    void SetUp() override
    {
        _tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(_tempDir->isValid());
    }

    std::unique_ptr<QTemporaryDir> _tempDir;
};

TEST_F(GeometryWorkerProcessTest, ProtocolMissingHelloFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 11;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());

    proc.injectStdoutLine(
        QStringLiteral("FCGEO/1 {\"type\":\"progress\",\"fraction\":0.1,\"phase\":\"x\"}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.state(), App::GeometryJobState::Failed);
    EXPECT_EQ(proc.result().errorCode, "MissingHello");
}

TEST_F(GeometryWorkerProcessTest, ProtocolHelloMissingProtocolFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 12;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(QStringLiteral("FCGEO/1 {\"type\":\"hello\",\"version\":\"1.0\"}"));
    proc.injectProcessFinished(1);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "MissingJsonField");
}

TEST_F(GeometryWorkerProcessTest, ProtocolWrongJobIdFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 13;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":0,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":\"99\"}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "JobIdMismatch");
    EXPECT_NE(proc.state(), App::GeometryJobState::ReadyToCommit);
}

TEST_F(GeometryWorkerProcessTest, ProtocolNumericJobIdFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 14;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":0,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":14}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "WrongJsonType");
}

TEST_F(GeometryWorkerProcessTest, ProtocolMaxUint64JobIdAccepted)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = App::kMaxGeometryJobIdWire;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    const QString line =
        QStringLiteral(
            "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":0,\"sha256\":"
            "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":\"%1\"}")
            .arg(QString::fromStdString(std::to_string(App::kMaxGeometryJobIdWire)));
    proc.injectStdoutLine(line);
    // Empty digest-valid file so acceptTrustedResult can run; decode may still fail.
    QFile f(_tempDir->path() + QStringLiteral("/result.fcg"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.close();
    proc.injectProcessFinished(0);
    EXPECT_FALSE(proc.protocolFailed()) << proc.result().errorCode;
    // Without a valid FCG body, decode fails — but jobId parsing must have succeeded.
    EXPECT_NE(proc.result().errorCode, "InvalidJobId");
    EXPECT_NE(proc.result().errorCode, "JobIdMismatch");
    EXPECT_NE(proc.result().errorCode, "WrongJsonType");
}

TEST_F(GeometryWorkerProcessTest, ProtocolZeroJobIdFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 20;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":0,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":\"0\"}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "InvalidJobId");
}

TEST_F(GeometryWorkerProcessTest, ProtocolNegativeJobIdFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 21;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":0,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":\"-21\"}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "InvalidJobId");
}

TEST_F(GeometryWorkerProcessTest, ProtocolOverflowJobIdFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 22;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":0,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\","
        "\"jobId\":\"18446744073709551616\"}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "InvalidJobId");
}

TEST_F(GeometryWorkerProcessTest, ProtocolBadExecutionTimeLatchesFailure)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 23;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":0,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\","
        "\"jobId\":\"23\",\"executionTime\":\"fast\"}"));
    // A later valid-looking result must not recover the latched failure.
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":0,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":\"23\"}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "WrongJsonType");
}

TEST_F(GeometryWorkerProcessTest, ProtocolNegativeExecutionTimeFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 24;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":0,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\","
        "\"jobId\":\"24\",\"executionTime\":-1}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "InvalidExecutionTime");
}

TEST_F(GeometryWorkerProcessTest, ProtocolMalformedErrorTerminalFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 25;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(
        QStringLiteral("FCGEO/1 {\"type\":\"error\",\"code\":\"\",\"message\":\"x\",\"jobId\":\"25\"}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "EmptyErrorTerminal");
}

TEST_F(GeometryWorkerProcessTest, ProtocolPostResultMessageFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 15;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":0,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":\"15\"}"));
    proc.injectStdoutLine(
        QStringLiteral("FCGEO/1 {\"type\":\"progress\",\"fraction\":1.0,\"phase\":\"late\"}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "PostTerminalMessage");
}

TEST_F(GeometryWorkerProcessTest, DigestCorrectMalformedFcgRejectedBeforeReadyToCommit)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 16;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());

    const QString resultPath = _tempDir->path() + QStringLiteral("/result.fcg");
    {
        QFile f(resultPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("NOT_FCG1_BUT_FIXED_DIGEST_CONTENT___________");
        f.close();
    }
    QFile f(resultPath);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    f.close();
    const QByteArray digest =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toLower();

    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(
        QStringLiteral("FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":%1,"
                       "\"sha256\":\"%2\",\"jobId\":\"16\"}")
            .arg(bytes.size())
            .arg(QString::fromLatin1(digest)));
    proc.injectProcessFinished(0);

    EXPECT_EQ(proc.state(), App::GeometryJobState::Failed);
    EXPECT_NE(proc.state(), App::GeometryJobState::ReadyToCommit);
    EXPECT_NE(proc.state(), App::GeometryJobState::Completed);
    EXPECT_FALSE(proc.result().success);
    EXPECT_FALSE(proc.result().errorCode.empty());
}

TEST_F(GeometryWorkerProcessTest, ParentControlledRealFreeCADCmdMappedBoolean)
{
    ASSERT_FALSE(findFreeCADCmd().isEmpty()) << "FreeCADCmd required";
    ASSERT_FALSE(findGeometryWorkerPy().isEmpty()) << "GeometryWorker.py required";

    long hashedId = 0;
    auto task = makeMappedBooleanTask(/*threshold=*/16, &hashedId);
    ASSERT_TRUE(task);

    App::GeometryJobSpec spec;
    spec.id = 42;
    spec.task = task;
    spec.backend = App::GeometryBackend::FreeCADCmd;
    spec.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);

    Gui::GeometryWorkerProcess proc;
    bool finished = false;
    App::GeometryJobState terminal = App::GeometryJobState::Queued;
    App::DetachedGeometryResult terminalResult;
    QObject::connect(&proc,
                     &Gui::GeometryWorkerProcess::jobFinished,
                     &proc,
                     [&](App::GeometryJobId, App::GeometryJobState state,
                         const App::DetachedGeometryResult& result) {
                         finished = true;
                         terminal = state;
                         terminalResult = result;
                     });

    ASSERT_TRUE(proc.startJob(spec, _tempDir->path() + QStringLiteral("/parent_job")));
    ASSERT_TRUE(QFileInfo::exists(proc.workspaceDir() + QStringLiteral("/request.json")));
    ASSERT_TRUE(QFileInfo::exists(proc.workspaceDir() + QStringLiteral("/base.fcg")));
    ASSERT_TRUE(QFileInfo::exists(proc.workspaceDir() + QStringLiteral("/tool.fcg")));

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&proc, &Gui::GeometryWorkerProcess::jobFinished, &loop, &QEventLoop::quit);
    timeout.start(180000);
    if (!finished) {
        loop.exec();
    }

    ASSERT_TRUE(finished) << "GeometryWorkerProcess did not finish";
    EXPECT_EQ(terminal, App::GeometryJobState::ReadyToCommit) << terminalResult.errorCode << ": "
                                                              << terminalResult.errorMessage;
    EXPECT_TRUE(terminalResult.success);
    ASSERT_FALSE(terminalResult.resultArchivePath.empty());
    EXPECT_TRUE(QFileInfo::exists(QString::fromStdString(terminalResult.resultArchivePath)));
    EXPECT_TRUE(QFileInfo::exists(proc.workspaceDir()));

    Part::FrozenTopoShapeBundle recovered;
    ASSERT_TRUE(Part::TopoShapeArchive::readArchive(terminalResult.resultArchivePath, recovered));
    ASSERT_TRUE(recovered.valid);
    ASSERT_TRUE(recovered.elementMap);
    ASSERT_TRUE(recovered.hasher);
    EXPECT_EQ(recovered.hasher->getThreshold(), 16);
    auto again = recovered.hasher->getID(
        QByteArray("MappedFaceNameThatDefinitelyExceedsTheSmallHashThresholdXX"),
        App::StringHasher::Option::Hashable);
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value(), hashedId);

    // Consumer releases workspace explicitly.
    QDir(proc.workspaceDir()).removeRecursively();
}

TEST_F(GeometryWorkerProcessTest, ParentControlledRealFreeCADCmdMappedFillet)
{
    ASSERT_FALSE(findFreeCADCmd().isEmpty()) << "FreeCADCmd required";
    ASSERT_FALSE(findGeometryWorkerPy().isEmpty()) << "GeometryWorker.py required";

    MappedFilletBox mapped;
    auto task = makeMappedFilletTask(&mapped);
    ASSERT_TRUE(task);
    ASSERT_TRUE(mapped.base.valid);
    ASSERT_TRUE(mapped.base.elementMap);
    EXPECT_GT(mapped.base.elementMap->size(), 0u);

    App::GeometryJobSpec spec;
    spec.id = 43;
    spec.task = task;
    spec.backend = App::GeometryBackend::FreeCADCmd;
    spec.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);

    Gui::GeometryWorkerProcess proc;
    std::atomic<int> callbacks {0};
    bool finished = false;
    App::GeometryJobState terminal = App::GeometryJobState::Queued;
    App::DetachedGeometryResult terminalResult;
    QObject::connect(&proc,
                     &Gui::GeometryWorkerProcess::jobFinished,
                     &proc,
                     [&](App::GeometryJobId, App::GeometryJobState state,
                         const App::DetachedGeometryResult& result) {
                         ++callbacks;
                         finished = true;
                         terminal = state;
                         terminalResult = result;
                     });

    ASSERT_TRUE(proc.startJob(spec, _tempDir->path() + QStringLiteral("/parent_fillet_job")));
    ASSERT_TRUE(QFileInfo::exists(proc.workspaceDir() + QStringLiteral("/request.json")));
    ASSERT_TRUE(QFileInfo::exists(proc.workspaceDir() + QStringLiteral("/base.fcg")));
    EXPECT_FALSE(QFileInfo::exists(proc.workspaceDir() + QStringLiteral("/tool.fcg")));

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&proc, &Gui::GeometryWorkerProcess::jobFinished, &loop, &QEventLoop::quit);
    timeout.start(180000);
    if (!finished) {
        loop.exec();
    }

    ASSERT_TRUE(finished) << "GeometryWorkerProcess did not finish";
    EXPECT_EQ(callbacks.load(), 1);
    EXPECT_FALSE(proc.protocolFailed());
    EXPECT_EQ(terminal, App::GeometryJobState::ReadyToCommit) << terminalResult.errorCode << ": "
                                                              << terminalResult.errorMessage;
    EXPECT_TRUE(terminalResult.success);
    ASSERT_FALSE(terminalResult.resultArchivePath.empty());
    EXPECT_TRUE(QFileInfo::exists(QString::fromStdString(terminalResult.resultArchivePath)));
    EXPECT_TRUE(QFileInfo::exists(proc.workspaceDir()));

    Part::FrozenTopoShapeBundle recovered;
    ASSERT_TRUE(Part::TopoShapeArchive::readArchive(terminalResult.resultArchivePath, recovered));
    assertMappedFilletResult(recovered,
                             mapped.hashedId,
                             mapped.longName,
                             mapped.seedName,
                             mapped.threshold);

    QDir(proc.workspaceDir()).removeRecursively();
}

TEST_F(GeometryWorkerProcessTest, ParentControlledFilletEmptyEdgesFailsBeforeOcc)
{
    ASSERT_FALSE(findFreeCADCmd().isEmpty()) << "FreeCADCmd required";
    ASSERT_FALSE(findGeometryWorkerPy().isEmpty()) << "GeometryWorker.py required";

    const MappedFilletBox mapped = makeMappedFilletBox();
    ASSERT_TRUE(mapped.base.valid);
    ASSERT_TRUE(mapped.base.elementMap);
    EXPECT_GT(mapped.base.elementMap->size(), 0u);

    auto badTask =
        std::make_shared<Part::FilletGeometryOperation>(mapped.base, std::vector<Part::FilletEdgeSpec> {});

    App::GeometryJobSpec badSpec;
    badSpec.id = 44;
    badSpec.task = badTask;
    badSpec.backend = App::GeometryBackend::FreeCADCmd;
    badSpec.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);

    Gui::GeometryWorkerProcess badProc;
    std::atomic<int> badCallbacks {0};
    bool badFinished = false;
    App::GeometryJobState badTerminal = App::GeometryJobState::Queued;
    App::DetachedGeometryResult badResult;
    QString badWorkspace;
    QObject::connect(&badProc,
                     &Gui::GeometryWorkerProcess::jobFinished,
                     &badProc,
                     [&](App::GeometryJobId, App::GeometryJobState state,
                         const App::DetachedGeometryResult& result) {
                         ++badCallbacks;
                         badFinished = true;
                         badTerminal = state;
                         badResult = result;
                         badWorkspace = badProc.workspaceDir();
                     });

    ASSERT_TRUE(badProc.startJob(badSpec, _tempDir->path() + QStringLiteral("/parent_fillet_bad")));
    ASSERT_TRUE(QFileInfo::exists(badProc.workspaceDir() + QStringLiteral("/request.json")));
    ASSERT_TRUE(QFileInfo::exists(badProc.workspaceDir() + QStringLiteral("/base.fcg")));

    QEventLoop badLoop;
    QTimer badTimeout;
    badTimeout.setSingleShot(true);
    QObject::connect(&badTimeout, &QTimer::timeout, &badLoop, &QEventLoop::quit);
    QObject::connect(&badProc, &Gui::GeometryWorkerProcess::jobFinished, &badLoop, &QEventLoop::quit);
    badTimeout.start(180000);
    if (!badFinished) {
        badLoop.exec();
    }

    ASSERT_TRUE(badFinished);
    EXPECT_EQ(badCallbacks.load(), 1);
    EXPECT_FALSE(badProc.protocolFailed()) << badResult.errorCode;
    EXPECT_EQ(badTerminal, App::GeometryJobState::Failed);
    EXPECT_EQ(badResult.errorCode, "EmptyEdges");
    EXPECT_NE(badTerminal, App::GeometryJobState::ReadyToCommit);
    EXPECT_NE(badTerminal, App::GeometryJobState::Completed);
    EXPECT_FALSE(badResult.success);
    EXPECT_TRUE(badResult.resultArchivePath.empty());
    if (!badWorkspace.isEmpty()) {
        EXPECT_FALSE(QFileInfo::exists(badWorkspace + QStringLiteral("/result.fcg")));
    }

    auto goodTask = std::make_shared<Part::FilletGeometryOperation>(
        mapped.base, std::vector<Part::FilletEdgeSpec> {{0, 1.0, 1.0}});
    App::GeometryJobSpec goodSpec;
    goodSpec.id = 45;
    goodSpec.task = goodTask;
    goodSpec.backend = App::GeometryBackend::FreeCADCmd;
    goodSpec.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);

    Gui::GeometryWorkerProcess goodProc;
    std::atomic<int> goodCallbacks {0};
    bool goodFinished = false;
    App::GeometryJobState goodTerminal = App::GeometryJobState::Queued;
    App::DetachedGeometryResult goodResult;
    QObject::connect(&goodProc,
                     &Gui::GeometryWorkerProcess::jobFinished,
                     &goodProc,
                     [&](App::GeometryJobId, App::GeometryJobState state,
                         const App::DetachedGeometryResult& result) {
                         ++goodCallbacks;
                         goodFinished = true;
                         goodTerminal = state;
                         goodResult = result;
                     });

    ASSERT_TRUE(goodProc.startJob(goodSpec, _tempDir->path() + QStringLiteral("/parent_fillet_good")));
    QEventLoop goodLoop;
    QTimer goodTimeout;
    goodTimeout.setSingleShot(true);
    QObject::connect(&goodTimeout, &QTimer::timeout, &goodLoop, &QEventLoop::quit);
    QObject::connect(&goodProc, &Gui::GeometryWorkerProcess::jobFinished, &goodLoop, &QEventLoop::quit);
    goodTimeout.start(180000);
    if (!goodFinished) {
        goodLoop.exec();
    }

    ASSERT_TRUE(goodFinished);
    EXPECT_EQ(goodCallbacks.load(), 1);
    EXPECT_EQ(goodTerminal, App::GeometryJobState::ReadyToCommit) << goodResult.errorCode;
    EXPECT_TRUE(goodResult.success);
    QDir(goodProc.workspaceDir()).removeRecursively();
}

namespace
{

class FailingSerializeTask : public App::DetachedGeometryTask
{
public:
    std::string operationType() const override
    {
        return "Test::FailingSerialize";
    }
    uint32_t codecVersion() const override
    {
        return 1;
    }
    App::GeometryOperationTraits traits() const override
    {
        return {};
    }
    App::DetachedGeometryResult run(App::GeometryWorkerContext&) const override
    {
        App::DetachedGeometryResult r;
        r.success = false;
        r.errorCode = "ShouldNotRun";
        return r;
    }
    App::GeometryArchiveWriteResult writeArchive(App::GeometryArchiveWriter& writer) const override
    {
        App::GeometryArchiveWriteResult out;
        out.success = false;
        out.errorCode = "RequestSerializeFailed";
        out.errorMessage = "Deliberate serialization failure for manager-path coverage";
        if (auto* workspace = dynamic_cast<App::GeometryRequestWorkspace*>(&writer)) {
            // Leave a stale request.json that must be scrubbed by markFailed/clearPublishedRequest.
            QFile stale(QDir(workspace->workspaceDir()).filePath(QStringLiteral("request.json")));
            if (stale.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                stale.write("{\"stale\":true}");
                stale.close();
            }
            workspace->markFailed(out.errorCode, out.errorMessage);
        }
        return out;
    }
};

} // namespace

TEST_F(GeometryWorkerProcessTest, ManagerPathRequestSerializeFailedNoLaunch)
{
    auto& mgr = App::GeometryJobManager::instance();
    mgr.clearProcessBackend();
    Gui::GeometryWorkerProcess::installManagerBackend();

    std::atomic<int> callbacks {0};
    std::string seenCode;
    std::string seenWorkspace;

    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 2401;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 99;
    spec.key.documentIncarnation = 2401;
    spec.key.targetObjectId = 99;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    spec.backend = App::GeometryBackend::FreeCADCmd;
    spec.task = std::make_shared<FailingSerializeTask>();
    spec.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    auto handle = mgr.submit(spec);
    ASSERT_TRUE(handle.isValid());
    // Terminal was delivered during submit/launch; register after and ensure result is specific.
    // Callback coverage here is late registration only (not while launch is blocked).
    EXPECT_EQ(mgr.getJobState(handle.id()), App::GeometryJobState::Failed);
    EXPECT_EQ(mgr.getJobResult(handle.id()).errorCode, "RequestSerializeFailed");
    mgr.registerCallback(handle.id(),
                         [&](App::GeometryJobId,
                             App::GeometryJobState,
                             const App::DetachedGeometryResult& result) {
                             ++callbacks;
                             seenCode = result.errorCode;
                         });
    // Late registration on an already-terminal job must still invoke exactly once.
    EXPECT_EQ(callbacks.load(), 1);
    EXPECT_EQ(seenCode, "RequestSerializeFailed");

    // Manager-owned workspace cleanup is idempotent (failProcessStart + worker destroy);
    // no retained request.json after serialize failure is the observable guarantee.
    Gui::GeometryWorkerProcess::uninstallManagerBackend();
    mgr.clearProcessBackend();

    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec direct = spec;
    direct.id = 77;
    const QString reused = _tempDir->path() + QStringLiteral("/reuse_ws");
    QDir().mkpath(reused);
    {
        QFile stale(reused + QStringLiteral("/request.json"));
        ASSERT_TRUE(stale.open(QIODevice::WriteOnly | QIODevice::Truncate));
        stale.write("{\"stale\":true}");
        stale.close();
    }
    EXPECT_FALSE(proc.startJob(direct, reused));
    EXPECT_EQ(proc.state(), App::GeometryJobState::Failed);
    EXPECT_EQ(proc.result().errorCode, "RequestSerializeFailed");
    EXPECT_FALSE(QFileInfo::exists(reused + QStringLiteral("/request.json")))
        << "stale request.json must not remain after serialize failure";
    EXPECT_FALSE(proc.isRunning());
}

TEST_F(GeometryWorkerProcessTest, WriteBytesNullAndEmptyAreSafe)
{
    App::GeometryRequestWorkspace workspace(_tempDir->path() + QStringLiteral("/wb"));
    workspace.writeBytes("empty.bin", nullptr, 0);
    EXPECT_FALSE(workspace.hasFailed());
    workspace.writeBytes("bad.bin", nullptr, 4);
    EXPECT_TRUE(workspace.hasFailed());
    EXPECT_EQ(workspace.failureCode(), "NullWriteBytes");
    EXPECT_FALSE(workspace.publishRequestJson());
    EXPECT_FALSE(QFileInfo::exists(workspace.workspaceDir() + QStringLiteral("/request.json")));
}

namespace
{

QString resultLineWithSize(qint64 size, App::GeometryJobId jobId)
{
    std::string jobWire;
    App::formatGeometryJobId(jobId, jobWire);
    return QStringLiteral(
               "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":%1,\"sha256\":"
               "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":"
               "\"%2\"}")
        .arg(QString::number(size), QString::fromStdString(jobWire));
}

} // namespace

TEST_F(GeometryWorkerProcessTest, ProtocolResultSizeZeroAccepted)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 901;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    const QString ws = _tempDir->path() + QStringLiteral("/size_zero");
    QDir().mkpath(ws);
    QFile empty(ws + QStringLiteral("/result.fcg"));
    ASSERT_TRUE(empty.open(QIODevice::WriteOnly | QIODevice::Truncate));
    empty.close();
    proc.prepareIdleJob(spec, ws);
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(resultLineWithSize(0, spec.id));
    proc.injectProcessFinished(0);
    EXPECT_FALSE(proc.protocolFailed());
}

TEST_F(GeometryWorkerProcessTest, ProtocolResultSizeTrustedMaxAccepted)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 902;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    const qint64 maxBytes = 512LL * 1024 * 1024;
    proc.injectStdoutLine(resultLineWithSize(maxBytes, spec.id));
    proc.injectProcessFinished(0);
    EXPECT_FALSE(proc.protocolFailed());
}

TEST_F(GeometryWorkerProcessTest, ProtocolResultSizeInt64OverflowFailsClosed)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 903;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":9223372036854775808,"
        "\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":\"903\"}"));
    proc.injectStdoutLine(resultLineWithSize(0, spec.id));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "OutOfRangeJsonNumber");
}

TEST_F(GeometryWorkerProcessTest, ProtocolResultSizeAboveTrustedMaxFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 904;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    const qint64 over = 512LL * 1024 * 1024 + 1;
    proc.injectStdoutLine(resultLineWithSize(over, spec.id));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "OutOfRangeJsonNumber");
}

TEST_F(GeometryWorkerProcessTest, ProtocolResultSizeNegativeFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 905;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":-1,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":\"905\"}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "InvalidResultSize");
}

TEST_F(GeometryWorkerProcessTest, ProtocolResultSizeFractionalFails)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 907;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":1.5,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":\"907\"}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "WrongJsonType");
}

TEST_F(GeometryWorkerProcessTest, ProtocolResultSizeNullWrongJsonType)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 908;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":null,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":\"908\"}"));
    proc.injectStdoutLine(resultLineWithSize(0, spec.id));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "WrongJsonType");
}

TEST_F(GeometryWorkerProcessTest, ProtocolResultSizeHugeExponentFailsClosed)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 909;
    spec.task = std::make_shared<Part::BooleanGeometryOperation>();
    proc.prepareIdleJob(spec, _tempDir->path());
    proc.injectStdoutLine(helloLine());
    proc.injectStdoutLine(QStringLiteral(
        "FCGEO/1 {\"type\":\"result\",\"path\":\"result.fcg\",\"size\":1e9999,\"sha256\":"
        "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"jobId\":\"909\"}"));
    proc.injectProcessFinished(0);
    EXPECT_TRUE(proc.protocolFailed());
    EXPECT_EQ(proc.result().errorCode, "MalformedControlJson");
}

TEST_F(GeometryWorkerProcessTest, StaleRequestDirectoryBlocksChildLaunch)
{
    Gui::GeometryWorkerProcess proc;
    App::GeometryJobSpec spec;
    spec.id = 906;
    spec.document.runtimeIncarnation = 1;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 1;
    spec.task = std::make_shared<FailingSerializeTask>();
    const QString ws = _tempDir->path() + QStringLiteral("/blocked_launch");
    QDir().mkpath(ws);
    QDir().mkpath(ws + QStringLiteral("/request.json"));

    EXPECT_FALSE(proc.startJob(spec, ws));
    EXPECT_EQ(proc.state(), App::GeometryJobState::Failed);
    EXPECT_FALSE(proc.isRunning());
    EXPECT_EQ(proc.result().errorCode, "WorkspaceReplaceFailed");
    EXPECT_TRUE(QFileInfo(ws + QStringLiteral("/request.json")).isDir());
}
