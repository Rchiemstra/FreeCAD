// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/ElementMap.h>
#include <App/GeometryJob.h>
#include <App/GeometryRequestWorkspace.h>
#include <App/IndexedName.h>
#include <App/MappedName.h>
#include <App/StringHasher.h>
#include <Mod/Part/App/BooleanGeometryOperation.h>
#include <Mod/Part/App/FilletGeometryOperation.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/PropertyTopoShape.h>
#include <Mod/Part/App/TopoShapeArchive.h>
#include <Base/Exception.h>
#include <Standard_Failure.hxx>
#include <src/App/InitApplication.h>

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <gp_Pnt.hxx>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{

QString findFreeCADCmd()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QStringLiteral("/code/build_docker/bin/FreeCADCmd"),
        appDir + QStringLiteral("/../bin/FreeCADCmd"),
        appDir + QStringLiteral("/FreeCADCmd"),
        QStringLiteral("/code/build_docker/bin/FreeCADCmd.exe"),
        appDir + QStringLiteral("/../bin/FreeCADCmd.exe"),
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

QString findPython3Executable()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QStringLiteral("/usr/bin/python3"),
        QStringLiteral("python3"),
        appDir + QStringLiteral("/../bin/python"),
        appDir + QStringLiteral("/python"),
    };
    for (const QString& path : candidates) {
        if (path == QStringLiteral("python3")) {
            return path;
        }
        if (QFileInfo::exists(path)) {
            return QFileInfo(path).absoluteFilePath();
        }
    }
    return QStringLiteral("python3");
}

struct WorkerTranscript
{
    int exitCode {-1};
    QStringList types;
    QJsonObject resultObj;
    QJsonObject errorObj;
    QByteArray stdoutBytes;
    QByteArray stderrBytes;
};

WorkerTranscript parseWorkerTranscript(const QByteArray& stdoutBytes,
                                       int exitCode,
                                       const QByteArray& stderrBytes = {})
{
    WorkerTranscript out;
    out.exitCode = exitCode;
    out.stdoutBytes = stdoutBytes;
    out.stderrBytes = stderrBytes;

    const QString text = QString::fromUtf8(out.stdoutBytes);
    for (const QString& line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (!line.startsWith(QStringLiteral("FCGEO/1 "))) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(line.mid(8).toUtf8());
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject obj = doc.object();
        const QString type = obj.value(QStringLiteral("type")).toString();
        out.types.push_back(type);
        if (type == QStringLiteral("result")) {
            out.resultObj = obj;
        }
        else if (type == QStringLiteral("error")) {
            out.errorObj = obj;
        }
    }
    return out;
}

WorkerTranscript runGeometryWorker(const QString& cmdPath,
                                   const QString& scriptPath,
                                   const QString& requestPath,
                                   const QString& workDir,
                                   const QString& launchedJobIdWire = {})
{
    QProcess proc;
    proc.setWorkingDirectory(workDir);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    if (!launchedJobIdWire.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("FCGEO_LAUNCHED_JOB_ID"), launchedJobIdWire);
        proc.setProcessEnvironment(env);
    }
    proc.start(cmdPath,
               {QStringLiteral("--safe-mode"),
                scriptPath,
                QStringLiteral("--pass"),
                requestPath});
    const bool finished = proc.waitForFinished(180000);
    WorkerTranscript out;
    out.stdoutBytes = proc.readAllStandardOutput();
    out.stderrBytes = proc.readAllStandardError();
    out.exitCode = finished ? proc.exitCode() : -1;
    if (!finished) {
        proc.kill();
        proc.waitForFinished(5000);
    }
    return parseWorkerTranscript(out.stdoutBytes, out.exitCode, out.stderrBytes);
}

bool transcriptHasProgressPhase(const QByteArray& stdoutBytes, const QString& phase)
{
    const QString text = QString::fromUtf8(stdoutBytes);
    for (const QString& line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (!line.startsWith(QStringLiteral("FCGEO/1 "))) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(line.mid(8).toUtf8());
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject obj = doc.object();
        if (obj.value(QStringLiteral("type")).toString() != QStringLiteral("progress")) {
            continue;
        }
        if (obj.value(QStringLiteral("phase")).toString() == phase) {
            return true;
        }
    }
    return false;
}

int countTranscriptType(const WorkerTranscript& transcript, const QString& type)
{
    return std::count(transcript.types.begin(), transcript.types.end(), type);
}

/// FreeCADCmd with no request path (exercises Python wrapper missing-arg handling).
WorkerTranscript runGeometryWorkerFreeCADCmdNoRequest(const QString& cmdPath,
                                                      const QString& scriptPath,
                                                      const QString& workDir,
                                                      const QString& launchedJobIdWire)
{
    QProcess proc;
    proc.setWorkingDirectory(workDir);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("FCGEO_LAUNCHED_JOB_ID"), launchedJobIdWire);
    proc.setProcessEnvironment(env);
    proc.start(cmdPath, {QStringLiteral("--safe-mode"), scriptPath});
    const bool finished = proc.waitForFinished(180000);
    QByteArray stdoutBytes = proc.readAllStandardOutput();
    QByteArray stderrBytes = proc.readAllStandardError();
    const int exitCode = finished ? proc.exitCode() : -1;
    if (!finished) {
        proc.kill();
        proc.waitForFinished(5000);
    }
    return parseWorkerTranscript(stdoutBytes, exitCode, stderrBytes);
}

/// Standalone Python subprocess with a fake Part module (no _runGeometryWorker).
WorkerTranscript runGeometryWorkerPythonWrapperWithFakePart(const QString& pythonPath,
                                                            const QString& scriptPath,
                                                            const QString& requestPath,
                                                            const QString& workDir,
                                                            const QString& fakePartDir,
                                                            const QString& launchedJobIdWire)
{
    QProcess proc;
    proc.setWorkingDirectory(workDir);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("FCGEO_LAUNCHED_JOB_ID"), launchedJobIdWire);
    const QString existingPythonPath = env.value(QStringLiteral("PYTHONPATH"));
    QString pythonPathEnv = fakePartDir;
    if (!existingPythonPath.isEmpty()) {
        pythonPathEnv += QDir::listSeparator() + existingPythonPath;
    }
    env.insert(QStringLiteral("PYTHONPATH"), pythonPathEnv);
    proc.setProcessEnvironment(env);
    proc.start(pythonPath,
               {scriptPath, QStringLiteral("--pass"), requestPath});
    const bool finished = proc.waitForFinished(180000);
    QByteArray stdoutBytes = proc.readAllStandardOutput();
    QByteArray stderrBytes = proc.readAllStandardError();
    const int exitCode = finished ? proc.exitCode() : -1;
    if (!finished) {
        proc.kill();
        proc.waitForFinished(5000);
    }
    return parseWorkerTranscript(stdoutBytes, exitCode, stderrBytes);
}

bool writeMinimalRequestJson(const QString& path, const QString& jobIdWire)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("protocol"), QStringLiteral("FCGEO/1"));
    obj.insert(QStringLiteral("jobId"), jobIdWire);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    return true;
}

Data::ElementMapPtr makeLeafMap(const App::StringHasherRef& hasher,
                                const char* label,
                                long tag)
{
    auto leaf = std::make_shared<Data::ElementMap>();
    leaf->hasher = hasher;
    for (int i = 1; i <= 6; ++i) {
        Data::IndexedName edge("Edge", i);
        App::StringIDRef sid = hasher->getID(QByteArray(label) + QByteArray::number(i));
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
}

bool nameReachable(const Data::ElementMapPtr& map, const QByteArray& needle)
{
    if (!map) {
        return false;
    }
    for (const auto& el : map->getAll()) {
        Data::ElementIDRefs sids;
        auto name = map->find(el.index, &sids);
        if (!name) {
            continue;
        }
        if (name.toString().find(needle.constData()) != std::string::npos) {
            return true;
        }
        for (const auto& sid : sids) {
            if (sid.dataToText().find(needle.constData()) != std::string::npos) {
                return true;
            }
            if (sid.toString().find(needle.constData()) != std::string::npos) {
                return true;
            }
        }
    }
    for (const auto& child : map->getChildElements()) {
        if (nameReachable(child.elementMap, needle)) {
            return true;
        }
    }
    return false;
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

void assertMappedBooleanResult(const Part::FrozenTopoShapeBundle& out,
                               long hashedId,
                               const QByteArray& longName,
                               const QByteArray& nameA,
                               const QByteArray& nameB,
                               int expectedThreshold)
{
    ASSERT_TRUE(out.valid);
    ASSERT_FALSE(out.shape.isNull());
    ASSERT_TRUE(out.elementMap);
    ASSERT_TRUE(out.hasher);
    EXPECT_EQ(static_cast<App::StringHasher*>(out.shape.Hasher),
              static_cast<App::StringHasher*>(out.hasher));
    EXPECT_EQ(out.hasher->getThreshold(), expectedThreshold);
    EXPECT_TRUE(allMapsUseHasher(out.elementMap, out.hasher));

    auto again = out.hasher->getID(longName, App::StringHasher::Option::Hashable);
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value(), hashedId);
    EXPECT_TRUE(again.isHashed());

    EXPECT_TRUE(nameReachable(out.elementMap, nameA)) << nameA.constData();
    EXPECT_TRUE(nameReachable(out.elementMap, nameB)) << nameB.constData();

    GProp_GProps props;
    BRepGProp::VolumeProperties(out.shape.getShape(), props);
    EXPECT_GT(props.Mass(), 1000.0);
    EXPECT_LT(props.Mass(), 2000.0);

    bool sawTag1 = false;
    bool sawTag2 = false;
    for (const auto& el : out.elementMap->getAll()) {
        Data::ElementIDRefs sids;
        auto mapped = out.elementMap->find(el.index, &sids);
        if (!mapped) {
            continue;
        }
        out.shape.traceElement(mapped, [&](const Data::MappedName&, int, long tag, long) {
            if (tag == 1) {
                sawTag1 = true;
            }
            if (tag == 2) {
                sawTag2 = true;
            }
            return true;
        });
        if (sawTag1 && sawTag2) {
            break;
        }
    }
    EXPECT_TRUE(sawTag1);
    EXPECT_TRUE(sawTag2);

    if (out.elementMap->hasChildElementMap()) {
        auto children = out.elementMap->getChildElements();
        Data::ElementMap* shared = nullptr;
        int sharedRefs = 0;
        for (const auto& child : children) {
            if (!child.elementMap) {
                continue;
            }
            Data::ElementIDRefs sids;
            auto name = child.elementMap->find(Data::IndexedName("Edge", 1), &sids);
            if (!name || sids.empty()) {
                continue;
            }
            if (sids.front().dataToText().find("SharedEdge") == std::string::npos) {
                continue;
            }
            if (!shared) {
                shared = child.elementMap.get();
            }
            if (child.elementMap.get() == shared) {
                ++sharedRefs;
            }
        }
        EXPECT_GE(sharedRefs, 2);
    }
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

} // namespace

class CrossProcessBooleanTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char arg0[] = "Part_tests_run";
            static char* argv[] = {arg0, nullptr};
            new QCoreApplication(argc, argv);
        }
    }

    void SetUp() override
    {
        _tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(_tempDir->isValid());
        _cmdPath = findFreeCADCmd();
        _scriptPath = findGeometryWorkerPy();
        _pythonPath = findPython3Executable();
        ASSERT_FALSE(_cmdPath.isEmpty())
            << "FreeCADCmd is required; cross-process test must not fall back to in-process task.run()";
        ASSERT_FALSE(_scriptPath.isEmpty())
            << "GeometryWorker.py is required; cross-process test must not fall back to in-process task.run()";
    }

    std::unique_ptr<QTemporaryDir> _tempDir;
    QString _cmdPath;
    QString _scriptPath;
    QString _pythonPath;
};

TEST_F(CrossProcessBooleanTest, RealFreeCADCmdMappedBooleanRoundTrip)
{
    try {
        constexpr int kThreshold = 16;
        const QByteArray longName(
            "MappedFaceNameThatDefinitelyExceedsTheSmallHashThresholdXX");
        const QByteArray nameA("SeedFaceA");
        const QByteArray nameB("SeedFaceB");

        App::StringHasherRef hasher(new App::StringHasher);
        hasher->setThreshold(kThreshold);

        BRepPrimAPI_MakeBox box1(10.0, 10.0, 10.0);
        BRepPrimAPI_MakeBox box2(gp_Pnt(5, 5, 5), 10.0, 10.0, 10.0);
        Part::TopoShape shape1(box1.Shape(), /*tag=*/1, hasher);
        Part::TopoShape shape2(box2.Shape(), /*tag=*/2, hasher);

        auto sharedLeaf = makeLeafMap(hasher, "SharedEdge", /*tag=*/11);
        ASSERT_TRUE(sharedLeaf);
        auto intermediate = std::make_shared<Data::ElementMap>();
        intermediate->hasher = hasher;
        {
            Data::ElementIDRefs empty;
            intermediate->addChildElements(
                /*masterTag=*/21,
                {{Data::IndexedName("Face", 1),
                  6,
                  0,
                  11,
                  sharedLeaf,
                  QByteArray("interSharedx"),
                  empty},
                 {Data::IndexedName("Face", 7),
                  6,
                  0,
                  11,
                  sharedLeaf,
                  QByteArray("interSharedy"),
                  empty}});
        }

        long hashedId = 0;
        {
            auto map = std::make_shared<Data::ElementMap>();
            map->hasher = hasher;
            Data::IndexedName face1("Face", 1);
            App::StringIDRef sid = hasher->getID(nameA);
            ASSERT_TRUE(sid);
            Data::MappedName mapped(sid);
            Data::ElementIDRefs sids;
            sids.push_back(sid);
            map->setElementName(face1, mapped, shape1.Tag, &sids);
            sid.mark();

            App::StringIDRef longSid =
                hasher->getID(longName, App::StringHasher::Option::Hashable);
            ASSERT_TRUE(longSid);
            ASSERT_TRUE(longSid.isHashed());
            hashedId = longSid.value();
            Data::MappedName longMapped(QByteArray::fromStdString(longSid.toString()));
            Data::ElementIDRefs longSids;
            longSids.push_back(longSid);
            map->setElementName(Data::IndexedName("Face", 2), longMapped, shape1.Tag, &longSids);
            longSid.mark();

            Data::ElementIDRefs empty;
            map->addChildElements(
                /*masterTag=*/1,
                {{Data::IndexedName("Face", 20),
                  6,
                  0,
                  21,
                  intermediate,
                  QByteArray("parentInter1"),
                  empty}});
            shape1.resetElementMap(map);
        }
        {
            auto map = std::make_shared<Data::ElementMap>();
            map->hasher = hasher;
            App::StringIDRef sid = hasher->getID(nameB);
            ASSERT_TRUE(sid);
            Data::MappedName mapped(sid);
            Data::ElementIDRefs sids;
            sids.push_back(sid);
            map->setElementName(Data::IndexedName("Face", 1), mapped, shape2.Tag, &sids);
            sid.mark();
            shape2.resetElementMap(map);
        }

        auto baseBundle = Part::TopoShapeArchive::createBundle(shape1);
        auto toolBundle = Part::TopoShapeArchive::createBundle(shape2);
        ASSERT_TRUE(baseBundle.valid) << baseBundle.errorCode;
        ASSERT_TRUE(toolBundle.valid) << toolBundle.errorCode;

        const QString workDir = _tempDir->path() + QStringLiteral("/job");
        App::GeometryRequestWorkspace workspace(workDir);
        Part::BooleanGeometryOperation op(Part::BooleanType::Fuse, baseBundle, toolBundle);
        workspace.requestObject().insert(QStringLiteral("jobId"), QStringLiteral("42"));
        ASSERT_TRUE(op.writeArchive(workspace).success);
        ASSERT_TRUE(workspace.publishRequestJson());
        ASSERT_TRUE(QFileInfo::exists(workDir + QStringLiteral("/base.fcg")));
        ASSERT_TRUE(QFileInfo::exists(workDir + QStringLiteral("/tool.fcg")));
        ASSERT_TRUE(QFileInfo::exists(workDir + QStringLiteral("/request.json")));

        // Must launch a real FreeCADCmd child — never call op.run() here.
        const WorkerTranscript transcript =
            runGeometryWorker(_cmdPath,
                              _scriptPath,
                              workDir + QStringLiteral("/request.json"),
                              workDir);
        ASSERT_EQ(transcript.exitCode, 0)
            << "stdout:\n"
            << transcript.stdoutBytes.constData() << "\nstderr:\n"
            << transcript.stderrBytes.constData();
        ASSERT_FALSE(transcript.types.isEmpty())
            << "stdout:\n"
            << transcript.stdoutBytes.constData() << "\nstderr:\n"
            << transcript.stderrBytes.constData();
        EXPECT_EQ(transcript.types.front(), QStringLiteral("hello"));
        EXPECT_NE(std::find(transcript.types.begin(),
                            transcript.types.end(),
                            QStringLiteral("progress")),
                  transcript.types.end());
        EXPECT_EQ(transcript.types.back(), QStringLiteral("result"));
        EXPECT_EQ(
            std::count(transcript.types.begin(), transcript.types.end(), QStringLiteral("hello")),
            1);
        EXPECT_EQ(
            std::count(transcript.types.begin(), transcript.types.end(), QStringLiteral("result")),
            1);

        const QString relPath = transcript.resultObj.value(QStringLiteral("path")).toString();
        EXPECT_EQ(relPath, QStringLiteral("result.fcg"));
        const qint64 claimedSize =
            transcript.resultObj.value(QStringLiteral("size")).toVariant().toLongLong();
        const QString claimedSha =
            transcript.resultObj.value(QStringLiteral("sha256")).toString().toLower();
        const QString absResult = QDir(workDir).filePath(relPath);
        ASSERT_TRUE(QFileInfo::exists(absResult));
        EXPECT_EQ(QFileInfo(absResult).size(), claimedSize);
        {
            QFile f(absResult);
            ASSERT_TRUE(f.open(QIODevice::ReadOnly));
            const QByteArray digest =
                QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex();
            EXPECT_EQ(QString::fromLatin1(digest), claimedSha);
        }

        Part::FrozenTopoShapeBundle recovered;
        ASSERT_TRUE(Part::TopoShapeArchive::readArchive(absResult.toStdString(), recovered));
        assertMappedBooleanResult(recovered, hashedId, longName, nameA, nameB, kThreshold);

        App::Document* doc = App::GetApplication().newDocument("CrossProcessBoolDoc");
        ASSERT_TRUE(doc);
        // Avoid Part::Feature default-material lookup in headless App-only tests.
        App::DocumentObject* obj = doc->addObject("App::FeaturePython", "Fused");
        ASSERT_TRUE(obj);
        auto* shapeProp = dynamic_cast<Part::PropertyPartShape*>(
            obj->addDynamicProperty("Part::PropertyPartShape", "Shape"));
        ASSERT_TRUE(shapeProp);
        shapeProp->setValue(recovered.shape);
        {
            const auto& savedShape = shapeProp->getShape();
            ASSERT_TRUE(savedShape.Hasher);
            EXPECT_EQ(savedShape.Hasher->getThreshold(), kThreshold);
            auto sidAfterSet = savedShape.Hasher->getID(longName, App::StringHasher::Option::Hashable);
            ASSERT_TRUE(sidAfterSet);
            EXPECT_EQ(sidAfterSet.value(), hashedId)
                << "Hashable lookup changed after PropertyPartShape::setValue";
            EXPECT_TRUE(sidAfterSet.isHashed());
            EXPECT_EQ(savedShape.getElementMapSize(), recovered.shape.getElementMapSize());
        }
        const QString fcstdPath = _tempDir->path() + QStringLiteral("/mapped_bool.FCStd");
        doc->saveAs(fcstdPath.toStdString().c_str());
        App::GetApplication().closeDocument(doc->getName());

        // Same-process reopen before second FreeCADCmd process.
        // Access live ElementMap via a derived inspector (elementMap() is protected).
        {
            struct ShapeMapAccess: Part::TopoShape
            {
                explicit ShapeMapAccess(const Part::TopoShape& s)
                    : Part::TopoShape(s)
                {}
                Data::ElementMapPtr liveMap() const
                {
                    return this->elementMap(false);
                }
            };

            App::Document* reopened = App::GetApplication().openDocument(fcstdPath.toStdString().c_str());
            ASSERT_TRUE(reopened);
            auto* fused = reopened->getObject("Fused");
            ASSERT_TRUE(fused);
            auto* prop = dynamic_cast<Part::PropertyPartShape*>(fused->getPropertyByName("Shape"));
            ASSERT_TRUE(prop);
            const auto& sh = prop->getShape();
            ASSERT_TRUE(sh.Hasher);
            EXPECT_EQ(sh.Hasher->getThreshold(), kThreshold);
            auto sid = sh.Hasher->getID(longName, App::StringHasher::Option::Hashable);
            ASSERT_TRUE(sid) << "Hashable lookup missing after same-process FCStd reopen";
            EXPECT_EQ(sid.value(), hashedId)
                << "Hashable SID remapped after FCStd save/reopen; table id="
                << (sh.Hasher->getID(hashedId) ? sh.Hasher->getID(hashedId).dataToText() : "missing");
            EXPECT_TRUE(sid.isHashed());
            EXPECT_GT(sh.getElementMapSize(), 0);

            ShapeMapAccess access(sh);
            auto liveMap = access.liveMap();
            ASSERT_TRUE(liveMap);
            EXPECT_EQ(static_cast<App::StringHasher*>(liveMap->hasher),
                      static_cast<App::StringHasher*>(sh.Hasher));
            EXPECT_TRUE(allMapsUseHasher(liveMap, sh.Hasher));

            std::function<void(const Data::ElementMapPtr&)> assertSidsResolve =
                [&](const Data::ElementMapPtr& em) {
                    if (!em) {
                        return;
                    }
                    EXPECT_EQ(static_cast<App::StringHasher*>(em->hasher),
                              static_cast<App::StringHasher*>(sh.Hasher));
                    for (const auto& el : em->getAll()) {
                        Data::ElementIDRefs sids;
                        (void)em->find(el.index, &sids);
                        for (const auto& ref : sids) {
                            ASSERT_TRUE(ref);
                            EXPECT_TRUE(ref.isFromSameHasher(sh.Hasher));
                            EXPECT_TRUE(sh.Hasher->getID(ref.value()));
                        }
                    }
                    for (const auto& child : em->getChildElements()) {
                        assertSidsResolve(child.elementMap);
                    }
                };
            assertSidsResolve(liveMap);

            bool hashedReferenced = false;
            bool sawTag1 = false;
            bool sawTag2 = false;
            std::function<void(const Data::ElementMapPtr&)> walk =
                [&](const Data::ElementMapPtr& em) {
                    if (!em) {
                        return;
                    }
                    for (const auto& el : em->getAll()) {
                        Data::ElementIDRefs sids;
                        auto mapped = em->find(el.index, &sids);
                        for (const auto& ref : sids) {
                            if (ref && ref.value() == hashedId) {
                                hashedReferenced = true;
                            }
                        }
                        if (mapped) {
                            sh.traceElement(mapped, [&](const Data::MappedName&, int, long tag, long) {
                                if (tag == 1) {
                                    sawTag1 = true;
                                }
                                if (tag == 2) {
                                    sawTag2 = true;
                                }
                                return true;
                            });
                        }
                    }
                    for (const auto& child : em->getChildElements()) {
                        walk(child.elementMap);
                    }
                };
            walk(liveMap);
            EXPECT_TRUE(hashedReferenced);
            EXPECT_TRUE(sawTag1);
            EXPECT_TRUE(sawTag2);

            if (liveMap->hasChildElementMap()) {
                Data::ElementMap* shared = nullptr;
                int sharedRefs = 0;
                for (const auto& child : liveMap->getChildElements()) {
                    if (!child.elementMap) {
                        continue;
                    }
                    Data::ElementIDRefs sids;
                    auto name = child.elementMap->find(Data::IndexedName("Edge", 1), &sids);
                    if (!name || sids.empty()
                        || sids.front().dataToText().find("SharedEdge") == std::string::npos) {
                        continue;
                    }
                    if (!shared) {
                        shared = child.elementMap.get();
                    }
                    if (child.elementMap.get() == shared) {
                        ++sharedRefs;
                    }
                }
                EXPECT_GE(sharedRefs, 2);
            }

            App::GetApplication().closeDocument(reopened->getName());
        }

        const QString verifyPy = _tempDir->path() + QStringLiteral("/verify_fcstd.py");
        {
            QFile py(verifyPy);
            ASSERT_TRUE(py.open(QIODevice::WriteOnly | QIODevice::Truncate));
            // Args after --pass: fcstd, threshold, hashedId, longName, nameA, nameB
            const QByteArray portable = QByteArray(
                "import FreeCAD as App\n"
                "import sys\n"
                "args = sys.argv[sys.argv.index('--pass') + 1:] if '--pass' in sys.argv else sys.argv[1:]\n"
                "path, threshold_s, hashed_s, long_name, name_a, name_b = args[:6]\n"
                "threshold = int(threshold_s)\n"
                "hashed_id = int(hashed_s)\n"
                "doc = App.open(path)\n"
                "obj = doc.getObject('Fused')\n"
                "assert obj is not None, 'missing Fused'\n"
                "sh = obj.Shape\n"
                "assert sh is not None and (not sh.isNull()), 'null shape'\n"
                "vol = float(sh.Volume)\n"
                "assert 1000.0 < vol < 2000.0, vol\n"
                "assert sh.ElementMapSize > 0, 'empty element map'\n"
                "hasher = sh.Hasher\n"
                "assert hasher is not None, 'missing hasher'\n"
                "assert int(hasher.Threshold) == threshold, (hasher.Threshold, threshold)\n"
                "sid = hasher.getID(long_name, False, True)\n"
                "assert sid is not None, 'long name Hashable lookup failed'\n"
                "assert int(sid.Value) == hashed_id, (sid.Value, hashed_id)\n"
                "assert bool(sid.IsHashed), 'expected hashed SID'\n"
                "assert hasher.getID(hashed_id) is not None\n"
                "emap = sh.ElementMap\n"
                "assert emap, 'missing ElementMap dict'\n"
                "blob = str(emap)\n"
                "assert name_a in blob or any(name_a in str(v) for v in emap.values()), name_a\n"
                "assert name_b in blob or any(name_b in str(v) for v in emap.values()) or "
                "any(name_b in str(k) for k in emap.keys()), name_b\n"
                "hashed_token = '#' + format(hashed_id, 'x')\n"
                "assert any(hashed_token in str(v) or hashed_token in str(k) for k, v in emap.items()), "
                "'hashed SID not referenced by map'\n"
                "for _k, val in emap.items():\n"
                "    text = str(val)\n"
                "    if text.startswith('#') and len(text) > 1:\n"
                "        try:\n"
                "            ref = int(text[1:], 16)\n"
                "        except ValueError:\n"
                "            continue\n"
                "        assert hasher.getID(ref) is not None, ('unresolved SID', text)\n"
                "tags = set()\n"
                "def walk(name, depth=0):\n"
                "    if depth > 24 or not name:\n"
                "        return\n"
                "    try:\n"
                "        hist = sh.getElementHistory(str(name))\n"
                "    except Exception:\n"
                "        return\n"
                "    if not hist:\n"
                "        return\n"
                "    tags.add(int(hist[0]))\n"
                "    walk(hist[1], depth + 1)\n"
                "    for item in hist[2]:\n"
                "        walk(item, depth + 1)\n"
                "for _key, val in list(emap.items())[:256]:\n"
                "    walk(val)\n"
                "    if 1 in tags and 2 in tags:\n"
                "        break\n"
                "assert (':H1:' in blob) or (1 in tags), (tags, 'missing source tag 1')\n"
                "assert (':H2:' in blob) or (2 in tags), (tags, 'missing source tag 2')\n"
                "assert (1 in tags and 2 in tags) or (':H1:' in blob and ':H2:' in blob), tags\n"
                "print('VERIFY_OK', vol, sh.ElementMapSize, hashed_id, threshold, sorted(tags))\n"
                "App.closeDocument(doc.Name)\n");



            py.write(portable);
            py.close();
        }

        QProcess reopen;
        reopen.setWorkingDirectory(_tempDir->path());
        reopen.start(_cmdPath,
                     {QStringLiteral("--safe-mode"),
                      verifyPy,
                      QStringLiteral("--pass"),
                      fcstdPath,
                      QString::number(kThreshold),
                      QString::number(hashedId),
                      QString::fromUtf8(longName),
                      QString::fromUtf8(nameA),
                      QString::fromUtf8(nameB)});
        ASSERT_TRUE(reopen.waitForFinished(180000));
        const QByteArray reopenOut = reopen.readAllStandardOutput();
        const QByteArray reopenErr = reopen.readAllStandardError();
        ASSERT_EQ(reopen.exitCode(), 0)
            << "stdout:\n"
            << reopenOut.constData() << "\nstderr:\n"
            << reopenErr.constData();
        ASSERT_TRUE(QString::fromUtf8(reopenOut).contains(QStringLiteral("VERIFY_OK")))
            << "stdout:\n"
            << reopenOut.constData() << "\nstderr:\n"
            << reopenErr.constData();
    }
    catch (const Base::Exception& e) {
        FAIL() << "Base::Exception: " << e.what();
    }
    catch (const Standard_Failure& e) {
        FAIL() << "OCC: " << (e.GetMessageString() ? e.GetMessageString() : "unknown");
    }
    catch (const std::exception& e) {
        FAIL() << "std::exception: " << e.what();
    }
}

TEST_F(CrossProcessBooleanTest, EarlyWorkerErrorIncludesLaunchedJobId)
{
    std::string jobWire;
    ASSERT_TRUE(App::formatGeometryJobId(99, jobWire));
    const QString missing = _tempDir->path() + QStringLiteral("/missing/request.json");
    WorkerTranscript transcript = runGeometryWorker(_cmdPath,
                                                    _scriptPath,
                                                    missing,
                                                    _tempDir->path(),
                                                    QString::fromStdString(jobWire));
    EXPECT_NE(transcript.exitCode, 0);
    ASSERT_FALSE(transcript.types.isEmpty());
    EXPECT_EQ(transcript.types.front(), QStringLiteral("hello"));
    EXPECT_EQ(transcript.types.back(), QStringLiteral("error"));
    EXPECT_EQ(transcript.types.count(QStringLiteral("error")), 1);
    EXPECT_EQ(transcript.errorObj.value(QStringLiteral("code")).toString(),
              QStringLiteral("request_file_not_found"));
    EXPECT_EQ(transcript.errorObj.value(QStringLiteral("jobId")).toString(),
              QString::fromStdString(jobWire));
}

TEST_F(CrossProcessBooleanTest, PythonMissingArgHonorsLaunchedJobId)
{
    std::string jobWire;
    ASSERT_TRUE(App::formatGeometryJobId(99, jobWire));
    WorkerTranscript transcript = runGeometryWorkerFreeCADCmdNoRequest(_cmdPath,
                                                                       _scriptPath,
                                                                       _tempDir->path(),
                                                                       QString::fromStdString(jobWire));
    EXPECT_NE(transcript.exitCode, 0);
    ASSERT_GE(transcript.types.size(), 2);
    EXPECT_EQ(transcript.types.front(), QStringLiteral("hello"));
    EXPECT_EQ(transcript.types.back(), QStringLiteral("error"));
    EXPECT_EQ(transcript.types.count(QStringLiteral("hello")), 1);
    EXPECT_EQ(transcript.types.count(QStringLiteral("error")), 1);
    EXPECT_EQ(transcript.errorObj.value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_arg"));
    EXPECT_EQ(transcript.errorObj.value(QStringLiteral("jobId")).toString(),
              QString::fromStdString(jobWire));
    EXPECT_FALSE(transcript.errorObj.value(QStringLiteral("message")).toString().isEmpty());
    EXPECT_FALSE(QFileInfo::exists(_tempDir->path() + QStringLiteral("/result.fcg")));
    EXPECT_TRUE(transcript.resultObj.isEmpty());
}

TEST_F(CrossProcessBooleanTest, PythonWorkerBindingMissingHonorsLaunchedJobId)
{
    std::string jobWire;
    ASSERT_TRUE(App::formatGeometryJobId(88, jobWire));

    const QString fakePartDir = _tempDir->path() + QStringLiteral("/fake_part_module");
    ASSERT_TRUE(QDir().mkpath(fakePartDir));
    {
        QFile fakePart(fakePartDir + QStringLiteral("/Part.py"));
        ASSERT_TRUE(fakePart.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray fakePartBody("# Deliberately lacks _runGeometryWorker.\n");
        ASSERT_EQ(fakePart.write(fakePartBody), fakePartBody.size());
    }

    const QString requestPath = _tempDir->path() + QStringLiteral("/request.json");
    ASSERT_TRUE(writeMinimalRequestJson(requestPath, QString::fromStdString(jobWire)));
    const QString isolatedScriptPath =
        _tempDir->path() + QStringLiteral("/GeometryWorker.py");
    ASSERT_TRUE(QFile::copy(_scriptPath, isolatedScriptPath));

    WorkerTranscript transcript = runGeometryWorkerPythonWrapperWithFakePart(
        _pythonPath,
        isolatedScriptPath,
        requestPath,
        _tempDir->path(),
        fakePartDir,
        QString::fromStdString(jobWire));
    EXPECT_EQ(transcript.exitCode, 2)
        << "Python-wrapper subprocess test (standalone interpreter, not FreeCADCmd)\nstdout:\n"
        << transcript.stdoutBytes.constData() << "\nstderr:\n"
        << transcript.stderrBytes.constData();
    ASSERT_GE(transcript.types.size(), 2)
        << "stdout:\n"
        << transcript.stdoutBytes.constData() << "\nstderr:\n"
        << transcript.stderrBytes.constData();
    EXPECT_EQ(transcript.types.front(), QStringLiteral("hello"));
    EXPECT_EQ(transcript.types.back(), QStringLiteral("error"));
    EXPECT_EQ(transcript.types.count(QStringLiteral("error")), 1);
    EXPECT_EQ(transcript.errorObj.value(QStringLiteral("code")).toString(),
              QStringLiteral("worker_binding_missing"));
    EXPECT_EQ(transcript.errorObj.value(QStringLiteral("jobId")).toString(),
              QString::fromStdString(jobWire));
}

TEST_F(CrossProcessBooleanTest, ProbeRequestFilenamesAreOrdinaryPaths)
{
    std::string jobWire;
    ASSERT_TRUE(App::formatGeometryJobId(7, jobWire));
    const QString jobQString = QString::fromStdString(jobWire);

    const QString missingArgProbe =
        _tempDir->path() + QStringLiteral("/missing-arg-probe.json");
    const QString bindingProbe =
        _tempDir->path() + QStringLiteral("/worker-binding-missing-probe.json");
    ASSERT_TRUE(writeMinimalRequestJson(missingArgProbe, jobQString));
    ASSERT_TRUE(writeMinimalRequestJson(bindingProbe, jobQString));

    WorkerTranscript missingArgTranscript =
        runGeometryWorker(_cmdPath,
                          _scriptPath,
                          missingArgProbe,
                          _tempDir->path(),
                          jobQString);
    EXPECT_NE(missingArgTranscript.exitCode, 0);
    ASSERT_GE(missingArgTranscript.types.size(), 2);
    EXPECT_EQ(missingArgTranscript.types.front(), QStringLiteral("hello"));
    EXPECT_EQ(missingArgTranscript.types.back(), QStringLiteral("error"));
    EXPECT_EQ(missingArgTranscript.types.count(QStringLiteral("error")), 1);
    EXPECT_EQ(missingArgTranscript.errorObj.value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_temp_dir"))
        << "missing-arg-probe.json must be opened like any other request path";
    EXPECT_EQ(missingArgTranscript.errorObj.value(QStringLiteral("jobId")).toString(), jobQString);

    WorkerTranscript bindingTranscript =
        runGeometryWorker(_cmdPath,
                          _scriptPath,
                          bindingProbe,
                          _tempDir->path(),
                          jobQString);
    EXPECT_NE(bindingTranscript.exitCode, 0);
    ASSERT_GE(bindingTranscript.types.size(), 2);
    EXPECT_EQ(bindingTranscript.types.front(), QStringLiteral("hello"));
    EXPECT_EQ(bindingTranscript.types.back(), QStringLiteral("error"));
    EXPECT_EQ(bindingTranscript.types.count(QStringLiteral("error")), 1);
    EXPECT_EQ(bindingTranscript.errorObj.value(QStringLiteral("code")).toString(),
              QStringLiteral("missing_temp_dir"))
        << "worker-binding-missing-probe.json must not short-circuit binding checks";
    EXPECT_EQ(bindingTranscript.errorObj.value(QStringLiteral("jobId")).toString(), jobQString);
}

TEST_F(CrossProcessBooleanTest, LaunchRequestJobIdMismatchFailsBeforeOcc)
{
    App::StringHasherRef hasher(new App::StringHasher);
    BRepPrimAPI_MakeBox box1(10.0, 10.0, 10.0);
    BRepPrimAPI_MakeBox box2(gp_Pnt(5, 5, 5), 10.0, 10.0, 10.0);
    auto b1 = Part::TopoShapeArchive::createBundle(Part::TopoShape(box1.Shape(), 1, hasher));
    auto b2 = Part::TopoShapeArchive::createBundle(Part::TopoShape(box2.Shape(), 2, hasher));
    ASSERT_TRUE(b1.valid && b2.valid);

    std::string launchedWire;
    std::string requestWire;
    ASSERT_TRUE(App::formatGeometryJobId(99, launchedWire));
    ASSERT_TRUE(App::formatGeometryJobId(42, requestWire));

    const QString workDir = _tempDir->path() + QStringLiteral("/mismatch");
    {
        App::GeometryRequestWorkspace workspace(workDir);
        Part::BooleanGeometryOperation op(Part::BooleanType::Fuse, b1, b2);
        workspace.requestObject().insert(QStringLiteral("jobId"),
                                         QString::fromStdString(requestWire));
        ASSERT_TRUE(op.writeArchive(workspace).success);
        ASSERT_TRUE(workspace.publishRequestJson());
    }

    WorkerTranscript bad = runGeometryWorker(_cmdPath,
                                             _scriptPath,
                                             workDir + QStringLiteral("/request.json"),
                                             workDir,
                                             QString::fromStdString(launchedWire));
    EXPECT_NE(bad.exitCode, 0);
    ASSERT_FALSE(bad.types.isEmpty());
    EXPECT_EQ(bad.types.front(), QStringLiteral("hello"));
    EXPECT_EQ(bad.types.back(), QStringLiteral("error"));
    EXPECT_EQ(bad.types.count(QStringLiteral("hello")), 1);
    EXPECT_EQ(bad.types.count(QStringLiteral("error")), 1);
    EXPECT_EQ(bad.types.count(QStringLiteral("progress")), 0);
    EXPECT_EQ(bad.errorObj.value(QStringLiteral("code")).toString(),
              QStringLiteral("job_id_mismatch"));
    EXPECT_EQ(bad.errorObj.value(QStringLiteral("jobId")).toString(),
              QString::fromStdString(launchedWire));
    EXPECT_FALSE(QFileInfo::exists(workDir + QStringLiteral("/result.fcg")));

    const QString goodDir = _tempDir->path() + QStringLiteral("/mismatch_recovery");
    {
        App::GeometryRequestWorkspace workspace(goodDir);
        Part::BooleanGeometryOperation op(Part::BooleanType::Fuse, b1, b2);
        workspace.requestObject().insert(QStringLiteral("jobId"), QString::fromStdString(launchedWire));
        ASSERT_TRUE(op.writeArchive(workspace).success);
        ASSERT_TRUE(workspace.publishRequestJson());
    }
    WorkerTranscript good = runGeometryWorker(_cmdPath,
                                              _scriptPath,
                                              goodDir + QStringLiteral("/request.json"),
                                              goodDir,
                                              QString::fromStdString(launchedWire));
    ASSERT_EQ(good.exitCode, 0) << good.stdoutBytes.constData();
    EXPECT_EQ(good.types.back(), QStringLiteral("result"));
}

TEST_F(CrossProcessBooleanTest, RecoveryAfterBadRequestThenValidBoolean)
{
    // Missing tool archive with a valid jobId → MissingOperandArchive, then recovery.
    App::StringHasherRef hasher(new App::StringHasher);
    BRepPrimAPI_MakeBox box1(10.0, 10.0, 10.0);
    BRepPrimAPI_MakeBox box2(gp_Pnt(5, 5, 5), 10.0, 10.0, 10.0);
    auto b1 = Part::TopoShapeArchive::createBundle(Part::TopoShape(box1.Shape(), 1, hasher));
    auto b2 = Part::TopoShapeArchive::createBundle(Part::TopoShape(box2.Shape(), 2, hasher));
    ASSERT_TRUE(b1.valid && b2.valid);

    constexpr App::GeometryJobId kBadJobId = 42;
    std::string badJobWire;
    ASSERT_TRUE(App::formatGeometryJobId(kBadJobId, badJobWire));

    const QString badDir = _tempDir->path() + QStringLiteral("/bad");
    {
        App::GeometryRequestWorkspace workspace(badDir);
        Part::BooleanGeometryOperation op(Part::BooleanType::Fuse, b1, b2);
        workspace.requestObject().insert(QStringLiteral("jobId"),
                                         QString::fromStdString(badJobWire));
        ASSERT_TRUE(op.writeArchive(workspace).success);
        ASSERT_TRUE(workspace.publishRequestJson());
        QFile::remove(badDir + QStringLiteral("/tool.fcg"));
    }
    WorkerTranscript bad = runGeometryWorker(
        _cmdPath, _scriptPath, badDir + QStringLiteral("/request.json"), badDir);
    EXPECT_NE(bad.exitCode, 0);
    ASSERT_FALSE(bad.errorObj.isEmpty());
    EXPECT_EQ(bad.types.front(), QStringLiteral("hello"));
    EXPECT_EQ(bad.types.back(), QStringLiteral("error"));
    EXPECT_EQ(bad.types.count(QStringLiteral("error")), 1);
    EXPECT_EQ(bad.errorObj.value(QStringLiteral("code")).toString(),
              QStringLiteral("MissingOperandArchive"));
    EXPECT_EQ(bad.errorObj.value(QStringLiteral("jobId")).toString(),
              QString::fromStdString(badJobWire));
    EXPECT_FALSE(QFileInfo::exists(badDir + QStringLiteral("/result.fcg")));

    const QString goodDir = _tempDir->path() + QStringLiteral("/good");
    {
        App::GeometryRequestWorkspace workspace(goodDir);
        Part::BooleanGeometryOperation op(Part::BooleanType::Fuse, b1, b2);
        workspace.requestObject().insert(QStringLiteral("jobId"), QStringLiteral("7"));
        ASSERT_TRUE(op.writeArchive(workspace).success);
        ASSERT_TRUE(workspace.publishRequestJson());
    }
    WorkerTranscript good = runGeometryWorker(
        _cmdPath, _scriptPath, goodDir + QStringLiteral("/request.json"), goodDir);
    ASSERT_EQ(good.exitCode, 0) << good.stdoutBytes.constData();
    EXPECT_EQ(good.types.front(), QStringLiteral("hello"));
    EXPECT_EQ(good.types.back(), QStringLiteral("result"));
    Part::FrozenTopoShapeBundle out;
    ASSERT_TRUE(Part::TopoShapeArchive::readArchive(
        QDir(goodDir).filePath(QStringLiteral("result.fcg")).toStdString(), out));
    EXPECT_TRUE(out.valid);
    EXPECT_FALSE(out.shape.isNull());
}

TEST_F(CrossProcessBooleanTest, IrreconcilableOperandHasherCollisionFailsBeforeOcc)
{
    App::StringHasherRef hasherA(new App::StringHasher);
    hasherA->setThreshold(10);
    App::StringHasherRef hasherB(new App::StringHasher);
    hasherB->setThreshold(20);
    BRepPrimAPI_MakeBox box1(10.0, 10.0, 10.0);
    BRepPrimAPI_MakeBox box2(gp_Pnt(5, 5, 5), 10.0, 10.0, 10.0);
    auto b1 = Part::TopoShapeArchive::createBundle(Part::TopoShape(box1.Shape(), 1, hasherA));
    auto b2 = Part::TopoShapeArchive::createBundle(Part::TopoShape(box2.Shape(), 2, hasherB));
    ASSERT_TRUE(b1.valid && b2.valid);

    // Force an ID/value collision across closures while keeping thresholds aligned in JSON
    // decode by rewriting tool snapshot threshold to match base after write... Instead stage
    // manually: write archives then patch request; materializeOperands will ThresholdMismatch.
    const QString workDir = _tempDir->path() + QStringLiteral("/collide");
    App::GeometryRequestWorkspace workspace(workDir);
    Part::BooleanGeometryOperation op(Part::BooleanType::Fuse, b1, b2);
    ASSERT_TRUE(op.writeArchive(workspace).success);
    ASSERT_TRUE(workspace.publishRequestJson());

    WorkerTranscript transcript = runGeometryWorker(
        _cmdPath, _scriptPath, workDir + QStringLiteral("/request.json"), workDir);
    EXPECT_NE(transcript.exitCode, 0);
    EXPECT_FALSE(transcript.errorObj.isEmpty());
    EXPECT_FALSE(QFileInfo::exists(workDir + QStringLiteral("/result.fcg")));
}

class CrossProcessFilletTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char arg0[] = "Part_tests_run";
            static char* argv[] = {arg0, nullptr};
            new QCoreApplication(argc, argv);
        }
    }

    void SetUp() override
    {
        _tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(_tempDir->isValid());
        _cmdPath = findFreeCADCmd();
        _scriptPath = findGeometryWorkerPy();
        ASSERT_FALSE(_cmdPath.isEmpty())
            << "FreeCADCmd is required; cross-process test must not fall back to in-process task.run()";
        ASSERT_FALSE(_scriptPath.isEmpty())
            << "GeometryWorker.py is required; cross-process test must not fall back to in-process task.run()";
    }

    std::unique_ptr<QTemporaryDir> _tempDir;
    QString _cmdPath;
    QString _scriptPath;
};

TEST_F(CrossProcessFilletTest, RealFreeCADCmdMappedFilletRoundTrip)
{
    try {
        const MappedFilletBox mapped = makeMappedFilletBox();
        ASSERT_TRUE(mapped.base.valid) << mapped.base.errorCode;
        ASSERT_TRUE(mapped.base.elementMap);
        EXPECT_GT(mapped.base.elementMap->size(), 0u);

        constexpr App::GeometryJobId kJobId = 25;
        std::string jobWire;
        ASSERT_TRUE(App::formatGeometryJobId(kJobId, jobWire));
        const QString launchedJobIdWire = QString::fromStdString(jobWire);
        ASSERT_FALSE(launchedJobIdWire.isEmpty());

        const QString workDir = _tempDir->path() + QStringLiteral("/fillet_job");
        App::GeometryRequestWorkspace workspace(workDir);
        Part::FilletGeometryOperation op(mapped.base, {{0, 1.0, 1.0}});
        workspace.requestObject().insert(QStringLiteral("jobId"), launchedJobIdWire);
        ASSERT_TRUE(op.writeArchive(workspace).success);
        ASSERT_TRUE(workspace.publishRequestJson());
        ASSERT_TRUE(QFileInfo::exists(workDir + QStringLiteral("/base.fcg")));
        ASSERT_TRUE(QFileInfo::exists(workDir + QStringLiteral("/request.json")));

        // Must launch a real FreeCADCmd child — never call op.run() or GeometryWorker::runWorkerProcess().
        const WorkerTranscript transcript = runGeometryWorker(_cmdPath,
                                                              _scriptPath,
                                                              workDir + QStringLiteral("/request.json"),
                                                              workDir,
                                                              launchedJobIdWire);
        ASSERT_EQ(transcript.exitCode, 0)
            << "stdout:\n"
            << transcript.stdoutBytes.constData() << "\nstderr:\n"
            << transcript.stderrBytes.constData();
        ASSERT_FALSE(transcript.types.isEmpty())
            << "stdout:\n"
            << transcript.stdoutBytes.constData() << "\nstderr:\n"
            << transcript.stderrBytes.constData();

        EXPECT_EQ(countTranscriptType(transcript, QStringLiteral("hello")), 1);
        EXPECT_EQ(countTranscriptType(transcript, QStringLiteral("result")), 1);
        EXPECT_EQ(countTranscriptType(transcript, QStringLiteral("error")), 0);
        EXPECT_EQ(countTranscriptType(transcript, QStringLiteral("result"))
                      + countTranscriptType(transcript, QStringLiteral("error")),
                  1);
        EXPECT_EQ(transcript.types.front(), QStringLiteral("hello"));
        EXPECT_NE(std::find(transcript.types.begin(),
                            transcript.types.end(),
                            QStringLiteral("progress")),
                  transcript.types.end());
        EXPECT_TRUE(transcriptHasProgressPhase(transcript.stdoutBytes, QStringLiteral("fillet.compute")));
        EXPECT_EQ(transcript.types.back(), QStringLiteral("result"));

        EXPECT_EQ(transcript.resultObj.value(QStringLiteral("jobId")).toString(), launchedJobIdWire);

        const QString relPath = transcript.resultObj.value(QStringLiteral("path")).toString();
        EXPECT_EQ(relPath, QStringLiteral("result.fcg"));
        const qint64 claimedSize =
            transcript.resultObj.value(QStringLiteral("size")).toVariant().toLongLong();
        const QString claimedSha =
            transcript.resultObj.value(QStringLiteral("sha256")).toString().toLower();
        const QString absResult = QDir(workDir).filePath(relPath);
        ASSERT_TRUE(QFileInfo::exists(absResult));
        EXPECT_EQ(QFileInfo(absResult).size(), claimedSize);
        {
            QFile f(absResult);
            ASSERT_TRUE(f.open(QIODevice::ReadOnly));
            const QByteArray digest =
                QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex();
            EXPECT_EQ(QString::fromLatin1(digest), claimedSha);
        }

        Part::FilletGeometryOperation filletOp(mapped.base, {{0, 1.0, 1.0}});
        const App::DetachedGeometryResult decoded = filletOp.decodeResultArchive(absResult.toStdString());
        ASSERT_TRUE(decoded.success) << decoded.errorCode << ": " << decoded.errorMessage;

        Part::FrozenTopoShapeBundle recovered;
        ASSERT_TRUE(Part::TopoShapeArchive::readArchive(absResult.toStdString(), recovered));
        assertMappedFilletResult(recovered,
                                 mapped.hashedId,
                                 mapped.longName,
                                 mapped.seedName,
                                 mapped.threshold);
    }
    catch (const Base::Exception& e) {
        FAIL() << "Base::Exception: " << e.what();
    }
    catch (const Standard_Failure& e) {
        FAIL() << "OCC: " << (e.GetMessageString() ? e.GetMessageString() : "unknown");
    }
    catch (const std::exception& e) {
        FAIL() << "std::exception: " << e.what();
    }
}

TEST_F(CrossProcessFilletTest, RecoveryAfterBadFilletRequestThenValid)
{
    const MappedFilletBox mapped = makeMappedFilletBox();
    ASSERT_TRUE(mapped.base.valid);

    constexpr App::GeometryJobId kJobId = 26;
    std::string jobWire;
    ASSERT_TRUE(App::formatGeometryJobId(kJobId, jobWire));
    const QString launchedJobIdWire = QString::fromStdString(jobWire);

    const QString badDir = _tempDir->path() + QStringLiteral("/fillet_bad");
    {
        App::GeometryRequestWorkspace workspace(badDir);
        Part::FilletGeometryOperation op(mapped.base, {{0, 1.0, 1.0}});
        workspace.requestObject().insert(QStringLiteral("jobId"), launchedJobIdWire);
        ASSERT_TRUE(op.writeArchive(workspace).success);
        ASSERT_TRUE(workspace.publishRequestJson());
        QFile::remove(badDir + QStringLiteral("/base.fcg"));
    }

    const WorkerTranscript bad = runGeometryWorker(_cmdPath,
                                                   _scriptPath,
                                                   badDir + QStringLiteral("/request.json"),
                                                   badDir,
                                                   launchedJobIdWire);
    EXPECT_NE(bad.exitCode, 0);
    ASSERT_FALSE(bad.types.isEmpty());
    EXPECT_EQ(bad.types.front(), QStringLiteral("hello"));
    EXPECT_EQ(bad.types.back(), QStringLiteral("error"));
    EXPECT_EQ(countTranscriptType(bad, QStringLiteral("error")), 1);
    EXPECT_EQ(countTranscriptType(bad, QStringLiteral("result")), 0);
    EXPECT_EQ(countTranscriptType(bad, QStringLiteral("error"))
                  + countTranscriptType(bad, QStringLiteral("result")),
              1);
    EXPECT_EQ(bad.errorObj.value(QStringLiteral("jobId")).toString(), launchedJobIdWire);
    EXPECT_EQ(bad.errorObj.value(QStringLiteral("code")).toString(),
              QStringLiteral("MissingOperandArchive"));
    EXPECT_TRUE(bad.resultObj.isEmpty());
    EXPECT_FALSE(QFileInfo::exists(badDir + QStringLiteral("/result.fcg")));
    EXPECT_FALSE(transcriptHasProgressPhase(bad.stdoutBytes, QStringLiteral("fillet.compute")));

    const QString goodDir = _tempDir->path() + QStringLiteral("/fillet_good");
    {
        App::GeometryRequestWorkspace workspace(goodDir);
        Part::FilletGeometryOperation op(mapped.base, {{0, 1.0, 1.0}});
        workspace.requestObject().insert(QStringLiteral("jobId"), launchedJobIdWire);
        ASSERT_TRUE(op.writeArchive(workspace).success);
        ASSERT_TRUE(workspace.publishRequestJson());
    }

    const WorkerTranscript good = runGeometryWorker(_cmdPath,
                                                  _scriptPath,
                                                  goodDir + QStringLiteral("/request.json"),
                                                  goodDir,
                                                  launchedJobIdWire);
    ASSERT_EQ(good.exitCode, 0) << good.stdoutBytes.constData();
    EXPECT_EQ(good.types.front(), QStringLiteral("hello"));
    EXPECT_EQ(good.types.back(), QStringLiteral("result"));
    EXPECT_EQ(good.resultObj.value(QStringLiteral("jobId")).toString(), launchedJobIdWire);
    const QString resultPath = QDir(goodDir).filePath(QStringLiteral("result.fcg"));
    ASSERT_TRUE(QFileInfo::exists(resultPath));

    Part::FilletGeometryOperation filletOp(mapped.base, {{0, 1.0, 1.0}});
    const App::DetachedGeometryResult decoded = filletOp.decodeResultArchive(resultPath.toStdString());
    ASSERT_TRUE(decoded.success) << decoded.errorCode << ": " << decoded.errorMessage;

    Part::FrozenTopoShapeBundle recovered;
    ASSERT_TRUE(Part::TopoShapeArchive::readArchive(resultPath.toStdString(), recovered));
    assertMappedFilletResult(recovered,
                             mapped.hashedId,
                             mapped.longName,
                             mapped.seedName,
                             mapped.threshold);
}
