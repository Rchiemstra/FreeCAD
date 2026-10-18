// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Inventor/SoDB.h>
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

#include "CollaborationGuiTestHelpers.h"

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

}  // namespace
