// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Inventor/SoDB.h>
#include <Inventor/sensors/SoOneShotSensor.h>
#include <Inventor/sensors/SoSensorManager.h>
#include <Inventor/SoPath.h>
#include <Inventor/actions/SoGetPrimitiveCountAction.h>
#include <Inventor/actions/SoSearchAction.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoMaterialBinding.h>
#include <Inventor/nodes/SoSeparator.h>

#include <QApplication>
#include <QEvent>
#include <QTemporaryDir>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentPresentationBoundary.h>
#include <App/PropertyStandard.h>
#include <Base/Interpreter.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/DocumentPresentationCapture.h>

#include <Gui/DocumentPresentationCache.h>
#include <Gui/PresentationApplyScheduler.h>
#include <Gui/PresentationCaptureRegistry.h>
#include <Gui/ViewProviderDocumentObject.h>
#include <Gui/ViewProviderGeometryObject.h>
#include <Gui/ViewProviderPresentationCapability.h>
#include <src/App/InitApplication.h>

#if defined(FREECAD_TEST_PART_CAPTURE_CACHE)
#include <Mod/Part/Gui/DocumentPresentationCapture.h>
#include <Mod/Part/Gui/ViewProviderExt.h>
#include <Mod/Part/Gui/SoFCShapeObject.h>
#include <Mod/Part/Gui/SoBrepFaceSet.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoNormal.h>
#include <Mod/Part/App/PartFeature.h>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#endif

#if defined(FREECAD_TEST_PARTDESIGN_DATUM_EXTENTS)
# include <App/CollaborativeSetPropertyOperation.h>
# include <App/MutationClassification.h>
# include <App/PropertyUnits.h>
# include <Precision.hxx>
#endif

#include "CollaborationGuiTestHelpers.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

constexpr App::DocumentInstanceId TestDocumentInstance = 91;
constexpr App::DocumentLifecycleEpoch TestDocumentEpoch = 3;

void ensureCoinInitialized()
{
    // Idempotent; SoSeparator construction requires the Coin type database.
    SoDB::init();
}

Gui::PresentationDelta makeDelta(Gui::PresentationSequence sequence,
                                 App::DocumentRevision sourceRevision,
                                 std::string marker)
{
    Gui::PresentationDelta delta;
    delta.revision.documentInstanceId = TestDocumentInstance;
    delta.revision.lifecycleEpoch = TestDocumentEpoch;
    delta.revision.sequence = sequence;
    delta.revision.sourceModelRevision = sourceRevision;
    delta.status.state = Gui::DocumentPresentationState::Committed;
    delta.status.statusMessage = marker;

    Gui::PresentationTreeNode node;
    node.stableObjectIdentity = marker;
    node.label = marker;
    delta.tree.push_back(std::move(node));
    return delta;
}

Gui::PresentationDelta makeDeltaWithRenderBuffers(Gui::PresentationSequence sequence,
                                                  App::DocumentRevision sourceRevision,
                                                  std::size_t bufferCount)
{
    auto delta = makeDelta(sequence, sourceRevision, "render-heavy");
    delta.renderBuffers.resize(bufferCount);
    for (std::size_t index = 0; index < bufferCount; ++index) {
        auto& buffer = delta.renderBuffers[index];
        buffer.stableObjectIdentity = "buffer-" + std::to_string(index);
        buffer.vertices.assign(12288, static_cast<float>(index) + 0.25F);
        buffer.normals.assign(12288, 1.0F);
        buffer.indices.assign(6144, static_cast<std::uint32_t>(index));
    }
    return delta;
}

Gui::PresentationDelta makeTriangleDelta()
{
    auto delta = makeDelta(1, 1, "triangles");
    Gui::PresentationRenderBuffer buffer;
    buffer.stableObjectIdentity = "triangles";
    buffer.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0,
                       2, 0, 0, 3, 0, 0, 2, 1, 0,
                       4, 0, 0, 5, 0, 0, 4, 1, 0};
    buffer.indices = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    buffer.topology = {1, 2};
    delta.renderBuffers.push_back(std::move(buffer));
    return delta;
}

int triangleCount(SoSeparator* root)
{
    SoGetPrimitiveCountAction count;
    count.apply(root);
    return count.getTriangleCount();
}

}  // namespace

TEST(DocumentPresentationCacheTest, RejectsStalePackets)
{
    Gui::DocumentPresentationCache cache;
    cache.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);

    auto first = makeDelta(1, 10, "first");
    EXPECT_EQ(cache.tryCommit(std::move(first)), Gui::PresentationCommitResult::Accepted);

    auto duplicateSequence = makeDelta(1, 11, "duplicate-sequence");
    EXPECT_EQ(cache.tryCommit(std::move(duplicateSequence)),
              Gui::PresentationCommitResult::StaleSequence);

    auto regressedSource = makeDelta(2, 9, "regressed-source");
    EXPECT_EQ(cache.tryCommit(std::move(regressedSource)),
              Gui::PresentationCommitResult::StaleSourceRevision);

    auto wrongEpoch = makeDelta(3, 10, "wrong-epoch");
    wrongEpoch.revision.lifecycleEpoch = TestDocumentEpoch + 1;
    EXPECT_EQ(cache.tryCommit(std::move(wrongEpoch)),
              Gui::PresentationCommitResult::StaleDocumentIdentity);

    auto wrongInstance = makeDelta(3, 10, "wrong-instance");
    wrongInstance.revision.documentInstanceId = TestDocumentInstance + 1;
    EXPECT_EQ(cache.tryCommit(std::move(wrongInstance)),
              Gui::PresentationCommitResult::StaleDocumentIdentity);
}

TEST(DocumentPresentationCacheTest, AtomicRevisionSwap)
{
    Gui::DocumentPresentationCache cache;
    cache.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);

    auto first = makeDelta(1, 1, "revision-a");
    EXPECT_EQ(cache.tryCommit(std::move(first)), Gui::PresentationCommitResult::Accepted);

    const auto committed = cache.current();
    ASSERT_TRUE(committed.has_value());
    EXPECT_EQ(committed->revision.sequence, 1U);
    EXPECT_EQ(committed->tree.front().label, "revision-a");

    auto second = makeDelta(2, 2, "revision-b");
    EXPECT_EQ(cache.tryCommit(std::move(second)), Gui::PresentationCommitResult::Accepted);

    const auto swapped = cache.current();
    ASSERT_TRUE(swapped.has_value());
    EXPECT_EQ(swapped->revision.sequence, 2U);
    EXPECT_EQ(swapped->tree.size(), 1U);
    EXPECT_EQ(swapped->tree.front().label, "revision-b");
    EXPECT_NE(swapped->tree.front().label, "revision-a");
    EXPECT_EQ(cache.committedSequence(), 2U);
}

TEST(PresentationApplySchedulerTest, CoalescesToNewestStagingRevision)
{
    ensureCoinInitialized();
    Gui::DocumentPresentationCache cache;
    cache.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    Gui::PresentationApplyScheduler scheduler(cache);

    scheduler.enqueue(makeDelta(1, 1, "seq-1"));
    scheduler.enqueue(makeDelta(2, 2, "seq-2"));
    scheduler.enqueue(makeDelta(3, 3, "seq-3"));

    const auto staging = scheduler.stagingRevision();
    ASSERT_TRUE(staging.has_value());
    EXPECT_EQ(staging->sequence, 3U);

    const auto pumpResult = scheduler.pump(0);
    EXPECT_TRUE(pumpResult.stagingComplete);
    EXPECT_TRUE(pumpResult.committedRevision);
    // Tree + Properties + Selection; coalesced packet has no render buffers.
    EXPECT_EQ(pumpResult.slicesApplied, 3U);

    const auto committed = cache.current();
    ASSERT_TRUE(committed.has_value());
    EXPECT_EQ(committed->revision.sequence, 3U);
    EXPECT_EQ(committed->tree.front().label, "seq-3");
    EXPECT_EQ(cache.status().state, Gui::DocumentPresentationState::Committed);
}

TEST(PresentationApplySchedulerTest, RecordsSliceBudgetAcrossPumpTurns)
{
    ensureCoinInitialized();
    Gui::DocumentPresentationCache cache;
    cache.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    Gui::PresentationApplyScheduler scheduler(cache);

    scheduler.enqueue(makeDeltaWithRenderBuffers(1, 1, 48));

    const std::size_t expectedSlices = 3 + 48;
    std::size_t totalSlices = 0;
    std::size_t pumpTurns = 0;

    while (scheduler.hasStagingWork()) {
        const auto result = scheduler.pump(1);
        ++pumpTurns;
        EXPECT_EQ(result.sliceDurationsMicros.size(), result.slicesApplied);
        totalSlices += result.slicesApplied;
        if (result.stagingComplete) {
            EXPECT_TRUE(result.committedRevision);
            break;
        }
    }

    EXPECT_EQ(totalSlices, expectedSlices);
    EXPECT_GT(pumpTurns, 1U);
    EXPECT_EQ(cache.committedSequence(), 1U);
}

TEST(PresentationApplySchedulerTest, PackedTriplesRenderThreeFacesWithCopiedAppearance)
{
    ensureCoinInitialized();
    Gui::DocumentPresentationCache cache;
    cache.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    Gui::PresentationApplyScheduler scheduler(cache);
    auto delta = makeTriangleDelta();
    delta.renderBuffers.front().material.diffuseColor = {0.2F, 0.4F, 0.6F};
    delta.renderBuffers.front().material.transparency = {0.35F};
    scheduler.enqueue(std::move(delta));
    ASSERT_TRUE(scheduler.pump(0).committedRevision);
    EXPECT_EQ(triangleCount(cache.committedCoinRoot()), 3);

    SoSearchAction search;
    search.setType(SoMaterial::getClassTypeId());
    search.apply(cache.committedCoinRoot());
    ASSERT_NE(search.getPath(), nullptr);
    auto* material = static_cast<SoMaterial*>(search.getPath()->getTail());
    EXPECT_EQ(material->diffuseColor[0], SbColor(0.2F, 0.4F, 0.6F));
    EXPECT_FLOAT_EQ(material->transparency[0], 0.35F);
}

TEST(PresentationApplySchedulerTest, SkipsMalformedGeometry)
{
    ensureCoinInitialized();
    for (int malformed = 0; malformed < 6; ++malformed) {
        SCOPED_TRACE(malformed);
        Gui::DocumentPresentationCache cache;
        cache.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
        Gui::PresentationApplyScheduler scheduler(cache);
        auto delta = makeTriangleDelta();
        auto& buffer = delta.renderBuffers.front();
        switch (malformed) {
            case 0: buffer.indices.pop_back(); break;
            case 1: buffer.vertices.pop_back(); break;
            case 2: buffer.normals.push_back(1.0F); break;
            case 3: buffer.indices[0] = static_cast<std::uint32_t>(-1); break;
            case 4: buffer.indices[0] = 9; break;
            case 5: buffer.topology = {1, 3}; break;
        }
        scheduler.enqueue(std::move(delta));
        ASSERT_TRUE(scheduler.pump(0).committedRevision);
        EXPECT_EQ(cache.committedCoinRoot()->getNumChildren(), 0);
    }
}

TEST(PresentationApplySchedulerTest, SkipsHiddenObjectBuffer)
{
    ensureCoinInitialized();
    Gui::DocumentPresentationCache cache;
    cache.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    Gui::PresentationApplyScheduler scheduler(cache);
    auto delta = makeTriangleDelta();
    delta.tree.front().visible = false;
    scheduler.enqueue(std::move(delta));
    ASSERT_TRUE(scheduler.pump(0).committedRevision);
    EXPECT_EQ(cache.committedCoinRoot()->getNumChildren(), 0);
}

namespace
{

class PartPresentationCaptureTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        static int argc = 1;
        static char executable[] = "Gui_tests_run";
        static char* argv[] = {executable, nullptr};
        if (!QApplication::instance()) {
            new QApplication(argc, argv);
        }
        tests::initApplication();
        if (!Gui::Application::Instance) {
            Gui::Application::initApplication();
            Gui::Application::initOpenInventor();
            new Gui::Application(true);
        }
        Base::Interpreter().runString("import Part; import PartGui");
    }

    void SetUp() override
    {
        App::DocumentInitFlags flags;
        flags.createView = false;
        documentName = App::GetApplication().getUniqueDocumentName("presentationCapture");
        document = App::GetApplication().newDocument(documentName.c_str(), "Presentation", flags);
        ASSERT_NE(document, nullptr);
        Base::Interpreter().runString(
            "doc = App.ActiveDocument\n"
            "feature = doc.addObject('Part::Part2DObject', 'Plane')\n"
            "feature.Shape = Part.makeCompound([Part.Face(Part.makePolygon([\n"
            "    App.Vector(x, 0, 0), App.Vector(x+1, 0, 0),\n"
            "    App.Vector(x, 1, 0), App.Vector(x, 0, 0)])) for x in (0, 2, 4)])\n");
        Gui::Test::recomputeWithoutBlockingGui(*document);
        guiDocument = Gui::Application::Instance->getDocument(document);
        ASSERT_NE(guiDocument, nullptr);
        guiDocument->catchUpIdleLivePresentation();
        QApplication::processEvents();
        (void)guiDocument->pumpPresentationApply(0);
        object = document->getObject("Plane");
        ASSERT_NE(object, nullptr);
        provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(guiDocument->getViewProvider(object));
        ASSERT_NE(provider, nullptr);
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(documentName.c_str());
        QApplication::processEvents();
    }

    void captureAndApplyBoundary()
    {
        Gui::installDocumentPresentationBoundaryHook();
        Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
            App::invokeDocumentPresentationBoundary(*document);
        });
        QApplication::sendPostedEvents(QCoreApplication::instance(), QEvent::MetaCall);
        ASSERT_TRUE(guiDocument->presentationApplyScheduler().hasStagingWork());
        ASSERT_TRUE(guiDocument->pumpPresentationApply(0).committedRevision);
    }

    std::string documentName;
    App::Document* document {};
    App::DocumentObject* object {};
    Gui::Document* guiDocument {};
    Gui::ViewProviderDocumentObject* provider {};
};

#if defined(FREECAD_TEST_PARTDESIGN_DATUM_EXTENTS)
// A deferred Placement replay must preserve the conservative wildcard fence
// used by structural compatibility edits, even though PropertyLength publishes
// that revision whenever its setter is called.
TEST_F(PartPresentationCaptureTest, UnchangedDatumExtentsPreserveRevisionFence)
{
    Base::Interpreter().runString("import PartDesignGui\n"
                                  "feature.addProperty('App::PropertyString', 'Text')\n"
                                  "datum = doc.addObject('PartDesign::Plane', 'MidX')\n");
    auto* datum = document->getObject("MidX");
    ASSERT_NE(datum, nullptr);
    auto* view = guiDocument->getViewProvider(datum);
    ASSERT_NE(view, nullptr);
    auto* length = dynamic_cast<App::PropertyLength*>(datum->getPropertyByName("Length"));
    auto* width = dynamic_cast<App::PropertyLength*>(datum->getPropertyByName("Width"));
    ASSERT_NE(length, nullptr);
    ASSERT_NE(width, nullptr);
    const auto* placement = datum->getPropertyByName("Placement");
    view->updateData(placement);
    Gui::Test::settlePendingRecomputeWithoutBlockingGui(*document);
    const double originalLength = length->getValue();
    const double originalWidth = width->getValue();
    App::ensureCollaborativeSetPropertyOperationRegistered();
    auto& service = document->collaborationService();
    const auto session = Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] {
        return service.beginEditSession("datum-refresh-test");
    });
    const App::CollaborativeOperationIntent intent {
        std::string(App::CollaborativeSetPropertyOperationType),
        {{"object", "Plane"},
         {"property", "Text"},
         {"value_type", "string"},
         {"value", "After refresh"}}
    };

    // Check exact equality, sub-tolerance differences, and a real size change.
    for (double difference : {0.0, 0.5 * Precision::Confusion(), 2.0 * Precision::Confusion()}) {
        SCOPED_TRACE(difference);
        length->setValue(originalLength + difference);
        width->setValue(originalWidth + difference);
        // Native scalar preparation needs no GUI callbacks. Keep queued refresh
        // out until the explicit Placement replay below.
        auto prepared
            = std::async(std::launch::async, [&] {
                  return service.prepareEdit(session.sessionId(), "datum-refresh", intent, "test");
              }).get();
        const auto wildcard = App::DocumentRevisionKey::unknownModelMutation();
        const auto before = document->collaborationRevisions().current(wildcard);
        // Replay the same callback used by updateView during GUI catch-up.
        {
            App::LivePresentationRefreshScope refresh;
            ASSERT_TRUE(refresh.acquired());
            view->updateData(placement);
        }
        const bool changed = difference > Precision::Confusion();
        EXPECT_EQ(document->collaborationRevisions().current(wildcard), before + (changed ? 2 : 0));
        const auto result = Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] {
            return service.commitEdit(session.sessionId(), prepared);
        });
        EXPECT_EQ(
            result.status,
            changed ? App::DocumentCommitStatus::Conflict : App::DocumentCommitStatus::Committed
        ) << result.message;
        EXPECT_DOUBLE_EQ(
            length->getValue(), changed ? originalLength : originalLength + difference
        );
        EXPECT_DOUBLE_EQ(width->getValue(), changed ? originalWidth : originalWidth + difference);
    }
}

#endif

TEST_F(PartPresentationCaptureTest, BothCoinProducersRoundTripThreeTriangles)
{
    const std::string identity = document->collaborationObjectIdentity(*object);
    for (bool fromProvider : {false, true}) {
        SCOPED_TRACE(fromProvider);
        Gui::PresentationRenderBuffer buffer;
        bool captured = false;
        // Provider capture deliberately refuses the GUI thread.
        std::thread owner([&] {
            if (fromProvider) {
                Gui::ViewProviderPresentationCaptureRequest request;
                request.stableObjectIdentity = identity;
                captured = provider->capturePresentationRenderBuffer(request, buffer);
            }
            else {
                captured = Gui::PresentationCaptureRegistry::tryCapture(*object, identity, buffer);
            }
        });
        owner.join();
        ASSERT_TRUE(captured);
        EXPECT_TRUE(buffer.material.diffuseColor.empty());
        EXPECT_TRUE(buffer.material.transparency.empty());
        ASSERT_EQ(buffer.indices.size(), 9U);
        EXPECT_EQ(buffer.topology, (std::vector<std::uint32_t> {1, 1, 1}));
        auto delta = makeDelta(1, 1, "captured");
        delta.renderBuffers.push_back(std::move(buffer));
        Gui::DocumentPresentationCache cache;
        cache.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
        Gui::PresentationApplyScheduler scheduler(cache);
        scheduler.enqueue(std::move(delta));
        ASSERT_TRUE(scheduler.pump(0).committedRevision);
        EXPECT_EQ(triangleCount(cache.committedCoinRoot()), 3);
    }
}

TEST_F(PartPresentationCaptureTest, InfiniteDatumsKeepLocationOrientationAndCacheIdentity)
{
    const std::string identity = document->collaborationObjectIdentity(*object);
    auto* feature = dynamic_cast<Part::Feature*>(object);
    ASSERT_NE(feature, nullptr);
    gp_Trsf transform;
    transform.SetRotation(gp_Ax1(gp::Origin(), gp::DY()), M_PI / 6);
    transform.SetTranslationPart(gp_Vec(11, 22, 33));
    const TopLoc_Location location(transform);
    const auto plane = BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY())).Face();
    const auto finite = BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), 100, 112, 200, 208).Face();
    for (bool compound : {false, true}) {
        for (bool reversed : {false, true}) {
            SCOPED_TRACE(compound);
            SCOPED_TRACE(reversed);
            TopoDS_Shape face = plane.Located(location);
            if (reversed) {
                face.Reverse();
            }
            TopoDS_Shape shape = face;
            if (compound) {
                BRep_Builder builder;
                TopoDS_Compound mixed;
                builder.MakeCompound(mixed);
                builder.Add(mixed, face);
                builder.Add(mixed, finite);
                shape = mixed;
            }
            Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
                if (compound) {
                    feature->Shape.setValue(shape);
                }
                else {
                    // Set root Placement through the property, as a datum feature does.
                    const auto placement = Part::TopoShape(shape).getPlacement();
                    shape.Location(TopLoc_Location());
                    feature->Shape.setValue(shape);
                    feature->Placement.setValue(placement);
                }
            });
            for (bool fromProvider : {false, true}) {
                SCOPED_TRACE(fromProvider);
                Gui::PresentationRenderBuffer buffer;
                std::thread owner([&] {
                    if (fromProvider) {
                        Gui::ViewProviderPresentationCaptureRequest request;
                        request.stableObjectIdentity = identity;
                        EXPECT_TRUE(provider->capturePresentationRenderBuffer(request, buffer));
                    }
                    else {
                        EXPECT_TRUE(Gui::PresentationCaptureRegistry::tryCapture(*object, identity, buffer));
                    }
                });
                owner.join();
                ASSERT_EQ(buffer.topology, (compound ? std::vector<std::uint32_t> {2, 2}
                                                     : std::vector<std::uint32_t> {2}));
                ASSERT_EQ(buffer.indices.size(), compound ? 12U : 6U);
                ASSERT_GE(buffer.vertices.size(), 12U);
                const auto point = [&](std::uint32_t index) {
                    return gp_Pnt(buffer.vertices[3 * index], buffer.vertices[3 * index + 1],
                                  buffer.vertices[3 * index + 2]);
                };
                // Root Placement is applied by the consumer; nested locations are in the buffer.
                const gp_Trsf inverse = compound ? transform.Inverted() : gp_Trsf();
                for (std::size_t i = 0; i < 6; ++i) {
                    const auto p = point(buffer.indices[i]).Transformed(inverse);
                    EXPECT_NEAR(std::abs(p.X()), 50, 1e-4);
                    EXPECT_NEAR(std::abs(p.Y()), 50, 1e-4);
                    EXPECT_NEAR(p.Z(), 0, 1e-4);
                }
                const auto a = point(buffer.indices[0]).Transformed(inverse);
                const auto b = point(buffer.indices[1]).Transformed(inverse);
                const auto c = point(buffer.indices[2]).Transformed(inverse);
                EXPECT_NEAR(gp_Vec(a, b).Crossed(gp_Vec(a, c)).Z(), reversed ? -10000 : 10000, 0.01);
                const auto before = PartGui::presentationCaptureCacheStatistics().tessellations;
                std::thread cached([&] {
                    if (fromProvider) {
                        Gui::ViewProviderPresentationCaptureRequest request;
                        request.stableObjectIdentity = identity;
                        EXPECT_TRUE(provider->capturePresentationRenderBuffer(request, buffer));
                    }
                    else {
                        EXPECT_TRUE(Gui::PresentationCaptureRegistry::tryCapture(*object, identity, buffer));
                    }
                });
                cached.join();
                EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, before);
            }
        }
    }
}

TEST_F(PartPresentationCaptureTest, PlaneRefreshDoesNotMarkFileChanged)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("plane.FCStd").toStdString();
    Gui::Test::saveAsWithoutBlockingGui(*document, path.c_str());
    ASSERT_FALSE(document->hasPendingFileChanges());
    int notifications = 0;
    auto connection = document->signalFileChangeStateChanged().connect(
        [&](const App::Document&) { ++notifications; });
    for (int i = 0; i < 3; ++i) {
        provider->updateData(object->getPropertyByName("Shape"));
    }
    EXPECT_EQ(notifications, 0);
    EXPECT_FALSE(document->hasPendingFileChanges());
    auto* showPlane = dynamic_cast<App::PropertyBool*>(provider->getPropertyByName("ShowPlane"));
    ASSERT_NE(showPlane, nullptr);
    showPlane->setValue(!showPlane->getValue());
    EXPECT_GT(notifications, 0);
    EXPECT_TRUE(document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));
    connection.disconnect();
}

TEST_F(PartPresentationCaptureTest, ShownGeometryReturnsAtNextDocumentBoundary)
{
    object->Visibility.setValue(false);
    captureAndApplyBoundary();
    auto packet = guiDocument->presentationCache().current();
    ASSERT_TRUE(packet.has_value());
    ASSERT_EQ(packet->tree.size(), 1U);
    EXPECT_FALSE(packet->tree.front().visible);
    EXPECT_TRUE(packet->renderBuffers.empty());
    EXPECT_EQ(triangleCount(guiDocument->presentationCache().committedCoinRoot()), 0);
    const auto sequence = guiDocument->presentationCache().committedSequence();

    object->Visibility.setValue(true);
    QApplication::processEvents();
    EXPECT_FALSE(guiDocument->presentationApplyScheduler().hasStagingWork());
    EXPECT_EQ(guiDocument->presentationCache().committedSequence(), sequence);
    EXPECT_EQ(triangleCount(guiDocument->presentationCache().committedCoinRoot()), 0);

    captureAndApplyBoundary();
    packet = guiDocument->presentationCache().current();
    ASSERT_TRUE(packet.has_value());
    ASSERT_EQ(packet->tree.size(), 1U);
    EXPECT_TRUE(packet->tree.front().visible);
    ASSERT_EQ(packet->renderBuffers.size(), 1U);
    EXPECT_EQ(triangleCount(guiDocument->presentationCache().committedCoinRoot()), 3);
}

TEST_F(PartPresentationCaptureTest, DocumentBoundaryCopiesAppearanceOnGuiThread)
{
    auto* geometry = dynamic_cast<Gui::ViewProviderGeometryObject*>(provider);
    ASSERT_NE(geometry, nullptr);
    auto material = geometry->ShapeAppearance[0];
    material.uuid.clear();
    material.diffuseColor = Base::Color(0.2F, 0.4F, 0.6F);
    material.transparency = 0.35F;
    geometry->ShapeAppearance.setValue(material);
    ASSERT_EQ(geometry->ShapeAppearance[0].diffuseColor, material.diffuseColor);
    ASSERT_FLOAT_EQ(geometry->ShapeAppearance[0].transparency, material.transparency);
    captureAndApplyBoundary();
    const auto packet = guiDocument->presentationCache().current();
    ASSERT_TRUE(packet.has_value());
    ASSERT_EQ(packet->renderBuffers.size(), 1U);
    const auto& appearance = packet->renderBuffers.front().material;
    EXPECT_EQ(appearance.diffuseColor, (std::vector<float> {0.2F, 0.4F, 0.6F}));
    EXPECT_EQ(appearance.transparency, (std::vector<float> {0.35F}));
    EXPECT_EQ(triangleCount(guiDocument->presentationCache().committedCoinRoot()), 3);
}

TEST_F(PartPresentationCaptureTest, DocumentBoundaryExpandsFaceColoursOverUnequalTriangleCounts)
{
    Base::Interpreter().runString(
        "feature.Shape = Part.makeCompound([Part.Face(Part.makePolygon([\n"
        "    App.Vector(x, y, 0) for x, y in points])) for points in (\n"
        "    [(0, 0), (1, 0), (0, 1), (0, 0)],\n"
        "    [(2, 0), (3, 0), (3, 1), (2, 1), (2, 0)])])\n");
    Gui::Test::recomputeWithoutBlockingGui(*document);
    guiDocument->catchUpIdleLivePresentation();
    QApplication::processEvents();
    (void)guiDocument->pumpPresentationApply(0);
    auto* geometry = dynamic_cast<Gui::ViewProviderGeometryObject*>(provider);
    ASSERT_NE(geometry, nullptr);
    auto materials = geometry->ShapeAppearance.getValues();
    materials.resize(2);
    materials[0].uuid.clear();
    materials[0].diffuseColor = Base::Color(1.0F, 0.0F, 0.0F);
    materials[0].transparency = 0.25F;
    materials[1].uuid.clear();
    materials[1].diffuseColor = Base::Color(0.0F, 1.0F, 0.0F);
    materials[1].transparency = 0.5F;
    geometry->ShapeAppearance.setValues(materials);
    ASSERT_EQ(geometry->ShapeAppearance.getSize(), 2);
    ASSERT_EQ(geometry->ShapeAppearance[0].diffuseColor, materials[0].diffuseColor);
    ASSERT_EQ(geometry->ShapeAppearance[1].diffuseColor, materials[1].diffuseColor);

    captureAndApplyBoundary();
    const auto packet = guiDocument->presentationCache().current();
    ASSERT_TRUE(packet.has_value());
    ASSERT_EQ(packet->renderBuffers.size(), 1U);
    const auto& buffer = packet->renderBuffers.front();
    EXPECT_EQ(buffer.topology, (std::vector<std::uint32_t> {1, 2}));
    EXPECT_EQ(buffer.material.binding, Gui::PresentationMaterialBinding::PerFace);
    EXPECT_EQ(buffer.material.diffuseColor,
              (std::vector<float> {1, 0, 0, 0, 1, 0, 0, 1, 0}));
    EXPECT_EQ(buffer.material.transparency, (std::vector<float> {0.25F, 0.5F, 0.5F}));
    auto* root = guiDocument->presentationCache().committedCoinRoot();
    EXPECT_EQ(triangleCount(root), 3);

    SoSearchAction search;
    search.setType(SoMaterialBinding::getClassTypeId());
    search.apply(root);
    ASSERT_NE(search.getPath(), nullptr);
    auto* binding = static_cast<SoMaterialBinding*>(search.getPath()->getTail());
    EXPECT_EQ(binding->value.getValue(), SoMaterialBinding::PER_FACE);
    search.setType(SoMaterial::getClassTypeId());
    search.apply(root);
    ASSERT_NE(search.getPath(), nullptr);
    auto* material = static_cast<SoMaterial*>(search.getPath()->getTail());
    ASSERT_EQ(material->diffuseColor.getNum(), 3);
    ASSERT_EQ(material->transparency.getNum(), 3);
    for (int triangle = 0; triangle < 3; ++triangle) {
        SCOPED_TRACE(triangle);
        EXPECT_EQ(material->diffuseColor[triangle],
                  triangle == 0 ? SbColor(1, 0, 0) : SbColor(0, 1, 0));
        EXPECT_FLOAT_EQ(material->transparency[triangle], triangle == 0 ? 0.25F : 0.5F);
    }
}

#if defined(FREECAD_TEST_PART_CAPTURE_CACHE)

TEST_F(PartPresentationCaptureTest, CaptureMatchesCoinFields)
{
    const auto box = BRepPrimAPI_MakeBox(2, 3, 4).Shape();
    const auto fused = BRepAlgoAPI_Fuse(box, BRepPrimAPI_MakeCylinder(1, 5).Shape()).Shape();
    for (const auto& shape : {box, fused, TopoDS_Shape()}) {
        for (bool fromUV : {false, true}) {
            auto* coin = new PartGui::SoFCShape;
            coin->ref();
            coin->addChild(coin->coords);
            coin->addChild(coin->norm);
            coin->addChild(coin->faceset);
            coin->addChild(coin->lineset);
            coin->addChild(coin->nodeset);
            PartGui::ViewProviderPartExt::setupCoinGeometry(shape, coin, 0.2, 28.65, fromUV);
            Gui::PresentationRenderBuffer buffer;
            bool captured = false;
            std::thread owner([&] {
                captured = PartGui::capturePartPresentationRenderBuffer(
                    *document, "fields-capture", shape, 0.2, 28.65, fromUV, buffer);
            });
            owner.join();
            EXPECT_TRUE(captured);
            const auto flatten = [](const auto& field) {
                std::vector<float> values;
                for (int i = 0; i < field.getNum(); ++i) {
                    for (int c = 0; c < 3; ++c) {
                        values.push_back(field[i][c]);
                    }
                }
                return values;
            };
            const auto packed = [](const auto& field) {
                std::vector<std::uint32_t> values;
                for (int i = 0; i < field.getNum(); ++i) {
                    if (field[i] >= 0) {
                        values.push_back(static_cast<std::uint32_t>(field[i]));
                    }
                }
                return values;
            };
            EXPECT_EQ(buffer.vertices, flatten(coin->coords->point));
            EXPECT_EQ(buffer.normals, flatten(coin->norm->vector));
            EXPECT_EQ(buffer.indices, packed(coin->faceset->coordIndex));
            EXPECT_EQ(buffer.topology, packed(coin->faceset->partIndex));
            coin->unref();
        }
    }
}

TEST_F(PartPresentationCaptureTest, CaptureInvalidFaceFailsWithoutPublishingGeometry)
{
    TopoDS_Face invalid;
    BRep_Builder().MakeFace(invalid);
    const auto before = PartGui::presentationCaptureCacheStatistics();
    for (int attempt = 0; attempt < 2; ++attempt) {
        Gui::PresentationRenderBuffer buffer;
        buffer.vertices = {1, 2, 3};
        bool captured = true;
        std::thread owner([&] {
            captured = PartGui::capturePartPresentationRenderBuffer(
                *document, "invalid-capture", invalid, 0.2, 28.65, false, buffer);
        });
        owner.join();
        EXPECT_FALSE(captured);
        EXPECT_TRUE(buffer.vertices.empty());
        EXPECT_TRUE(buffer.normals.empty());
        EXPECT_TRUE(buffer.indices.empty());
        EXPECT_TRUE(buffer.topology.empty());
        EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().entries, before.entries);
    }
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, before.tessellations + 2);
}

TEST_F(PartPresentationCaptureTest, CaptureDoesNotDispatchGuiCoinSensorOnWorker)
{
    const auto shape = BRepPrimAPI_MakeBox(2, 3, 4).Shape();
    const auto guiThread = std::this_thread::get_id();
    std::thread::id dispatchedOn;
    SoOneShotSensor sensor([](void* context, SoSensor*) {
        *static_cast<std::thread::id*>(context) = std::this_thread::get_id();
    }, &dispatchedOn);
    sensor.setPriority(0);
    sensor.schedule();
    Gui::PresentationRenderBuffer buffer;
    bool captured = false;
    std::thread owner([&] {
        captured = PartGui::capturePartPresentationRenderBuffer(
            *document, "sensor-capture", shape, 0.2, 28.65, false, buffer);
    });
    owner.join();
    EXPECT_TRUE(captured);
    EXPECT_FALSE(buffer.vertices.empty());
    EXPECT_EQ(dispatchedOn, std::thread::id());
    SoDB::getSensorManager()->processImmediateQueue();
    EXPECT_EQ(dispatchedOn, guiThread);
}

TEST_F(PartPresentationCaptureTest, CaptureCacheReusesPackedGeometryFromBothProducers)
{
    Base::Interpreter().runString(
        "feature.Placement = App.Placement(App.Vector(17, 4, 2), App.Rotation())\n");
    for (bool fromProvider : {false, true}) {
        SCOPED_TRACE(fromProvider);
        const std::string identity = document->collaborationObjectIdentity(*object)
            + (fromProvider ? "-reuse-provider" : "-reuse-registry");
        auto capture = [&] {
            return Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
                Gui::PresentationRenderBuffer buffer;
                bool captured;
                if (fromProvider) {
                    Gui::ViewProviderPresentationCaptureRequest request;
                    request.stableObjectIdentity = identity;
                    captured = provider->capturePresentationRenderBuffer(request, buffer);
                }
                else {
                    captured = Gui::PresentationCaptureRegistry::tryCapture(*object, identity, buffer);
                }
                EXPECT_TRUE(captured);
                return buffer;
            });
        };
        const auto before = PartGui::presentationCaptureCacheStatistics().tessellations;
        auto first = capture();
        ASSERT_FALSE(first.vertices.empty());
        const auto vertices = first.vertices;
        const auto normals = first.normals;
        const auto indices = first.indices;
        const auto topology = first.topology;
        // GUI-side edits to a packet must not mutate cached geometry/material.
        first.vertices.front() = 12345.0F;
        first.material.diffuseColor = {1, 0, 0};
        for (int repeat = 0; repeat < 4; ++repeat) {
            const auto reused = capture();
            EXPECT_EQ(reused.vertices, vertices);
            EXPECT_EQ(reused.normals, normals);
            EXPECT_EQ(reused.indices, indices);
            EXPECT_EQ(reused.topology, topology);
            EXPECT_TRUE(reused.material.diffuseColor.empty());
            EXPECT_TRUE(reused.material.transparency.empty());
        }
        EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, before + 1);
    }
}

TEST_F(PartPresentationCaptureTest, CaptureCacheInvalidatesAllGeometryInputs)
{
    const std::string identity = document->collaborationObjectIdentity(*object) + "-inputs";
    auto shape = BRepPrimAPI_MakeBox(2, 3, 4).Shape();
    const auto before = PartGui::presentationCaptureCacheStatistics().tessellations;
    auto capture = [&](const TopoDS_Shape& value, double deviation = 0.2,
                       double angle = 28.65, bool uv = false) {
        return Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
            Gui::PresentationRenderBuffer buffer;
            EXPECT_TRUE(PartGui::capturePartPresentationRenderBuffer(
                *document, identity, value, deviation, angle, uv, buffer));
            return buffer;
        });
    };
    const auto original = capture(shape);
    ASSERT_FALSE(original.vertices.empty());
    EXPECT_EQ(capture(shape).vertices, original.vertices);
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, before + 1);

    gp_Trsf translation;
    translation.SetTranslation(gp_Vec(17, 0, 0));
    const auto moved = shape.Moved(TopLoc_Location(translation));
    ASSERT_TRUE(shape.IsPartner(moved));
    ASSERT_FALSE(shape.IsEqual(moved));
    const auto placed = capture(moved);
    ASSERT_EQ(placed.vertices.size(), original.vertices.size());
    // Keep the existing Coin placement convention; the location change must
    // force a fresh capture even when some emitted coordinates coincide.
    EXPECT_FALSE(placed.vertices.empty());
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, before + 2);

    const auto reversed = moved.Reversed();
    ASSERT_TRUE(moved.IsSame(reversed));
    ASSERT_FALSE(moved.IsEqual(reversed));
    capture(reversed);
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, before + 3);

    shape = BRepPrimAPI_MakeBox(5, 6, 7).Shape();
    EXPECT_NE(capture(shape).vertices, original.vertices);
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, before + 4);
    capture(shape, 0.1);
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, before + 5);
    capture(shape, 0.1, 14.0);
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, before + 6);
    const auto uv = capture(shape, 0.1, 14.0, true);
    EXPECT_EQ(capture(shape, 0.1, 14.0, true).normals, uv.normals);
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, before + 7);

    const auto empty = capture(TopoDS_Shape {});
    EXPECT_TRUE(empty.vertices.empty());
    EXPECT_TRUE(empty.indices.empty());
    capture(shape, 0.1, 14.0, true);
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, before + 8);
}

TEST_F(PartPresentationCaptureTest, CaptureCacheSeparatesDocumentsAndClearsOnClose)
{
    const auto shape = BRepPrimAPI_MakeBox(2, 3, 4).Shape();
    App::DocumentInitFlags flags;
    flags.createView = false;
    const std::string otherName = App::GetApplication().getUniqueDocumentName("captureCacheOther");
    auto* other = App::GetApplication().newDocument(otherName.c_str(), "Other", flags);
    ASSERT_NE(other, nullptr);
    const auto before = PartGui::presentationCaptureCacheStatistics();
    for (auto* doc : {document, other}) {
        Gui::Test::runOnDocumentOwnerWhilePumpingGui(*doc, [&] {
            Gui::PresentationRenderBuffer buffer;
            EXPECT_TRUE(PartGui::capturePartPresentationRenderBuffer(
                *doc, "same-object-identity", shape, 0.2, 28.65, false, buffer));
        });
    }
    const auto populated = PartGui::presentationCaptureCacheStatistics();
    EXPECT_EQ(populated.entries, before.entries + 2);
    EXPECT_EQ(populated.tessellations, before.tessellations + 2);
    bool acceptedDuringClose = true;
    auto connection = App::GetApplication().signalDeleteDocument.connect(
        [&](const App::Document& closing) {
            if (&closing != other) {
                return;
            }
            std::thread capture([&] {
                Gui::PresentationRenderBuffer buffer;
                acceptedDuringClose = PartGui::capturePartPresentationRenderBuffer(
                    closing, "late-capture", shape, 0.2, 28.65, false, buffer);
            });
            capture.join();
        });
    EXPECT_TRUE(App::GetApplication().closeDocument(otherName.c_str()));
    connection.disconnect();
    EXPECT_FALSE(acceptedDuringClose);
    const auto closed = PartGui::presentationCaptureCacheStatistics();
    EXPECT_EQ(closed.entries, populated.entries - 1);
    EXPECT_LT(closed.packedBytes, populated.packedBytes);
    EXPECT_EQ(closed.tessellations, populated.tessellations);
    Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
        Gui::PresentationRenderBuffer buffer;
        EXPECT_TRUE(PartGui::capturePartPresentationRenderBuffer(
            *document, "same-object-identity", shape, 0.2, 28.65, false, buffer));
    });
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, populated.tessellations);
}

TEST_F(PartPresentationCaptureTest, CaptureCacheAllowsConcurrentMisses)
{
    const auto shape = BRepPrimAPI_MakeBox(2, 3, 4).Shape();
    constexpr std::size_t workers = 8;
    std::barrier start(static_cast<std::ptrdiff_t>(workers));
    std::vector<Gui::PresentationRenderBuffer> results(workers);
    std::vector<std::thread> threads;
    const auto before = PartGui::presentationCaptureCacheStatistics().tessellations;
    for (std::size_t i = 0; i < workers; ++i) {
        threads.emplace_back([&, i] {
            start.arrive_and_wait();
            for (int repeat = 0; repeat < 4; ++repeat) {
                EXPECT_TRUE(PartGui::capturePartPresentationRenderBuffer(
                    *document, "concurrent", shape, 0.2, 28.65, false, results[i]));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    ASSERT_FALSE(results.front().vertices.empty());
    for (const auto& buffer : results) {
        EXPECT_EQ(buffer.vertices, results.front().vertices);
        EXPECT_EQ(buffer.normals, results.front().normals);
        EXPECT_EQ(buffer.indices, results.front().indices);
        EXPECT_EQ(buffer.topology, results.front().topology);
    }
    const auto meshed = PartGui::presentationCaptureCacheStatistics().tessellations;
    EXPECT_GE(meshed, before + 1);
    EXPECT_LE(meshed, before + workers);
}

// Pause a miss in its mesh stage. A bounded wait also lets a locking regression
// fail and unwind its worker threads instead of hanging the test process.
class CaptureMeshPause
{
public:
    explicit CaptureMeshPause(const std::string& identity)
        : entered(enteredPromise.get_future())
        , resumed(resumePromise.get_future().share())
    {
        PartGui::setPresentationCaptureMeshHook([this, identity](const std::string& key) {
            if (key == identity && !claimed.exchange(true)) {
                enteredPromise.set_value();
                EXPECT_EQ(resumed.wait_for(std::chrono::seconds(10)), std::future_status::ready);
            }
        });
    }

    ~CaptureMeshPause()
    {
        PartGui::setPresentationCaptureMeshHook({});
    }

    bool wait()
    {
        return entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    }

    void resume()
    {
        resumePromise.set_value();
    }

private:
    std::promise<void> enteredPromise;
    std::promise<void> resumePromise;
    std::future<void> entered;
    std::shared_future<void> resumed;
    std::atomic<bool> claimed {false};
};

TEST_F(PartPresentationCaptureTest, CaptureCacheMeshesDocumentsIndependently)
{
    App::DocumentInitFlags flags;
    flags.createView = false;
    const std::string otherName = App::GetApplication().getUniqueDocumentName("captureOverlap");
    auto* other = App::GetApplication().newDocument(otherName.c_str(), "Other", flags);
    ASSERT_NE(other, nullptr);
    // Deliberately share a TShape: private copies must isolate OCC meshing too.
    const auto shape = BRepPrimAPI_MakeBox(2, 3, 4).Shape();
    CaptureMeshPause pause("held-mesh");
    std::thread held([&] {
        Gui::PresentationRenderBuffer buffer;
        EXPECT_TRUE(PartGui::capturePartPresentationRenderBuffer(
            *document, "held-mesh", shape, 0.2, 28.65, false, buffer));
        EXPECT_FALSE(buffer.vertices.empty());
    });
    EXPECT_TRUE(pause.wait());
    std::promise<void> completed;
    auto ready = completed.get_future();
    std::thread independent([&] {
        Gui::PresentationRenderBuffer buffer;
        EXPECT_TRUE(PartGui::capturePartPresentationRenderBuffer(
            *other, "independent-mesh", shape, 0.2, 28.65, false, buffer));
        EXPECT_FALSE(buffer.vertices.empty());
        completed.set_value();
    });
    const bool progressed = ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    EXPECT_TRUE(progressed);
    if (progressed) {
        // GUI diagnostics and deletion must complete while the first mesh waits.
        const auto populated = PartGui::presentationCaptureCacheStatistics();
        EXPECT_TRUE(App::GetApplication().closeDocument(otherName.c_str()));
        EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().entries, populated.entries - 1);
    }
    pause.resume();
    held.join();
    independent.join();
    if (!progressed) {
        App::GetApplication().closeDocument(otherName.c_str());
    }
}

TEST_F(PartPresentationCaptureTest, CaptureCacheDoesNotPublishMissAfterClose)
{
    App::DocumentInitFlags flags;
    flags.createView = false;
    const std::string otherName = App::GetApplication().getUniqueDocumentName("captureClosing");
    auto* other = App::GetApplication().newDocument(otherName.c_str(), "Other", flags);
    ASSERT_NE(other, nullptr);
    const auto shape = BRepPrimAPI_MakeBox(2, 3, 4).Shape();
    const auto before = PartGui::presentationCaptureCacheStatistics().entries;
    CaptureMeshPause pause("closing-mesh");
    std::thread held([&] {
        Gui::PresentationRenderBuffer buffer;
        EXPECT_TRUE(PartGui::capturePartPresentationRenderBuffer(
            *other, "closing-mesh", shape, 0.2, 28.65, false, buffer));
        EXPECT_FALSE(buffer.vertices.empty());
    });
    EXPECT_TRUE(pause.wait());
    EXPECT_TRUE(App::GetApplication().closeDocument(otherName.c_str()));
    pause.resume();
    held.join();
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().entries, before);
}

TEST_F(PartPresentationCaptureTest, CaptureCacheInvalidatesShapePropertyWithSameTShape)
{
    auto* feature = dynamic_cast<Part::Feature*>(object);
    ASSERT_NE(feature, nullptr);
    const auto shape = BRepBuilderAPI_MakeVertex(gp_Pnt(1, 2, 3)).Vertex();
    Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] { feature->Shape.setValue(shape); });
    const std::string identity = document->collaborationObjectIdentity(*object);
    auto capture = [&] {
        return Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
            Gui::PresentationRenderBuffer buffer;
            EXPECT_TRUE(Gui::PresentationCaptureRegistry::tryCapture(*object, identity, buffer));
            return buffer;
        });
    };
    const auto original = capture();
    ASSERT_FALSE(original.vertices.empty());
    const auto populated = PartGui::presentationCaptureCacheStatistics();
    Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
        BRep_Builder().UpdateVertex(shape, gp_Pnt(7, 8, 9), 1e-7);
        feature->Shape.setValue(shape);
        EXPECT_TRUE(feature->Shape.getValue().IsEqual(shape));
    });
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().entries, populated.entries - 1);
    EXPECT_NE(capture().vertices, original.vertices);
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations,
              populated.tessellations + 1);
}

TEST_F(PartPresentationCaptureTest, CaptureCacheDoesNotPublishInvalidatedMiss)
{
    auto* feature = dynamic_cast<Part::Feature*>(object);
    ASSERT_NE(feature, nullptr);
    const auto shape = feature->Shape.getValue();
    const std::string identity = document->collaborationObjectIdentity(*object);
    // The fixture's presentation boundary may already have cached this object.
    Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] { feature->Shape.setValue(shape); });
    const auto before = PartGui::presentationCaptureCacheStatistics().entries;
    CaptureMeshPause pause(identity);
    std::thread held([&] {
        Gui::PresentationRenderBuffer buffer;
        EXPECT_TRUE(PartGui::capturePartPresentationRenderBuffer(
            *document, identity, shape, 0.2, 28.65, false, buffer));
    });
    EXPECT_TRUE(pause.wait());
    Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] { feature->Shape.setValue(shape); });
    pause.resume();
    held.join();
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().entries, before);
    const auto meshed = PartGui::presentationCaptureCacheStatistics().tessellations;
    Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
        Gui::PresentationRenderBuffer buffer;
        EXPECT_TRUE(Gui::PresentationCaptureRegistry::tryCapture(*object, identity, buffer));
    });
    EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, meshed + 1);
}

TEST_F(PartPresentationCaptureTest, CaptureCacheEvictsDeletedObject)
{
    const auto identity = document->collaborationObjectIdentity(*object);
    Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
        Gui::PresentationRenderBuffer buffer;
        EXPECT_TRUE(Gui::PresentationCaptureRegistry::tryCapture(*object, identity, buffer));
    });
    const auto populated = PartGui::presentationCaptureCacheStatistics();
    Base::Interpreter().runString("doc.removeObject('Plane')\n");
    const auto removed = PartGui::presentationCaptureCacheStatistics();
    EXPECT_EQ(removed.entries, populated.entries - 1);
    EXPECT_LT(removed.packedBytes, populated.packedBytes);
}

TEST_F(PartPresentationCaptureTest, CaptureCacheEvictsDeletedObjectAfterDeferredNotification)
{
    const auto identity = document->collaborationObjectIdentity(*object);
    Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
        Gui::PresentationRenderBuffer buffer;
        EXPECT_TRUE(Gui::PresentationCaptureRegistry::tryCapture(*object, identity, buffer));
    });
    const auto populated = PartGui::presentationCaptureCacheStatistics();
    bool replayed = false;
    auto connection = App::GetApplication().signalDeletedObject.connect(
        [&](const App::DocumentObject& removed) {
            if (&removed == object) {
                replayed = document->collaborationNotificationsReplaying();
            }
        });
    const auto outcome = Gui::Test::commitCompatibilityMutationWithoutBlockingGui(
        *document,
        {App::CollaborationCompatibilityScope::Structural, {}, {}, {}},
        [&] { document->removeObject("Plane"); });
    connection.disconnect();
    ASSERT_TRUE(outcome.committed()) << outcome.message;
    EXPECT_TRUE(replayed);
    const auto removed = PartGui::presentationCaptureCacheStatistics();
    EXPECT_EQ(removed.entries, populated.entries - 1);
    EXPECT_LT(removed.packedBytes, populated.packedBytes);
}

TEST_F(PartPresentationCaptureTest, CaptureCacheEvictsLeastRecentlyUsedEntriesAtBound)
{
    const auto shape = BRepPrimAPI_MakeBox(2, 3, 4).Shape();
    Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
        auto capture = [&](const std::string& identity) {
            Gui::PresentationRenderBuffer buffer;
            EXPECT_TRUE(PartGui::capturePartPresentationRenderBuffer(
                *document, identity, shape, 0.2, 28.65, false, buffer));
        };
        for (std::size_t i = 0; i < PartGui::PresentationCaptureCacheMaxEntries; ++i) {
            capture("bounded-" + std::to_string(i));
        }
        const auto full = PartGui::presentationCaptureCacheStatistics();
        EXPECT_EQ(full.entries, PartGui::PresentationCaptureCacheMaxEntries);
        EXPECT_LE(full.packedBytes, PartGui::PresentationCaptureCacheMaxBytes);
        capture("bounded-0");  // Touch the oldest entry before admitting one more.
        EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, full.tessellations);
        capture("bounded-extra");
        capture("bounded-0");
        EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations, full.tessellations + 1);
        capture("bounded-1");
        const auto evicted = PartGui::presentationCaptureCacheStatistics();
        EXPECT_EQ(evicted.tessellations, full.tessellations + 2);
        EXPECT_EQ(evicted.entries, PartGui::PresentationCaptureCacheMaxEntries);
        EXPECT_LE(evicted.packedBytes, PartGui::PresentationCaptureCacheMaxBytes);
    });
}

TEST_F(PartPresentationCaptureTest, CaptureCacheEnforcesByteBudget)
{
    const auto shape = BRepPrimAPI_MakeBox(2, 3, 4).Shape();
    Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
        // Large identities exercise byte eviction with a small, cheap shape.
        const std::string prefix(4U * 1024U * 1024U, 'x');
        constexpr std::size_t count = 24;
        for (std::size_t i = 0; i < count; ++i) {
            Gui::PresentationRenderBuffer buffer;
            EXPECT_TRUE(PartGui::capturePartPresentationRenderBuffer(
                *document, prefix + std::to_string(i), shape, 0.2, 28.65, false, buffer));
            EXPECT_LE(PartGui::presentationCaptureCacheStatistics().packedBytes,
                      PartGui::PresentationCaptureCacheMaxBytes);
        }
        EXPECT_LT(PartGui::presentationCaptureCacheStatistics().entries, count);
    });
}

TEST_F(PartPresentationCaptureTest, CaptureCacheRetriesFailuresWithoutReturningOldGeometry)
{
    const auto shape = BRepPrimAPI_MakeBox(2, 3, 4).Shape();
    TopoDS_Face invalid;
    BRep_Builder builder;
    builder.MakeFace(invalid);
    TopoDS_Compound broken;
    builder.MakeCompound(broken);
    builder.Add(broken, shape);  // Keep the compound non-empty (it has vertices).
    builder.Add(broken, invalid);
    Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, [&] {
        Gui::PresentationRenderBuffer buffer;
        EXPECT_TRUE(PartGui::capturePartPresentationRenderBuffer(
            *document, "failure", shape, 0.2, 28.65, false, buffer));
        ASSERT_FALSE(buffer.vertices.empty());
        const auto before = PartGui::presentationCaptureCacheStatistics();
        for (int attempt = 0; attempt < 2; ++attempt) {
            EXPECT_FALSE(PartGui::capturePartPresentationRenderBuffer(
                *document, "failure", broken, 0.2, 28.65, false, buffer));
            EXPECT_TRUE(buffer.vertices.empty());
            EXPECT_TRUE(buffer.normals.empty());
            EXPECT_TRUE(buffer.indices.empty());
            EXPECT_TRUE(buffer.topology.empty());
        }
        const auto failed = PartGui::presentationCaptureCacheStatistics();
        EXPECT_EQ(failed.entries, before.entries - 1);
        EXPECT_EQ(failed.tessellations, before.tessellations + 2);
        EXPECT_TRUE(PartGui::capturePartPresentationRenderBuffer(
            *document, "failure", shape, 0.2, 28.65, false, buffer));
        EXPECT_FALSE(buffer.vertices.empty());
        EXPECT_EQ(PartGui::presentationCaptureCacheStatistics().tessellations,
                  before.tessellations + 3);
    });
}

#endif

}  // namespace
