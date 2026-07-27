// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QtGlobal>

#include <App/Document.h>
#include <App/Link.h>
#include <App/Part.h>
#include <Base/Interpreter.h>
#include <Base/Placement.h>
#include <Base/Quantity.h>
#include <Base/Unit.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Inventor/So3DAnnotation.h>
#include <Inventor/SoDB.h>
#include <Inventor/SoInteraction.h>
#include <Inventor/nodes/SoGroup.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/InterferenceScan.h>
#include <Mod/Assembly/Gui/TaskInterferenceCheck.h>
#include <Mod/Part/App/FeaturePartBox.h>
#include <Mod/Part/App/InterferenceDetection.h>
#include <Mod/Part/Gui/SoBrepEdgeSet.h>
#include <Mod/Part/Gui/SoBrepFaceSet.h>
#include <Mod/Part/Gui/SoBrepPointSet.h>
#include <Mod/Part/Gui/SoFCShapeObject.h>
#include <Mod/Spreadsheet/App/Sheet.h>
#include <Mod/Part/Gui/ViewProviderPreviewExtension.h>
#include <src/App/InitApplication.h>

namespace
{

TopoDS_Shape makePlacedBox(double dx, double dy, double dz, double tx, double ty, double tz)
{
    TopoDS_Shape box = BRepPrimAPI_MakeBox(dx, dy, dz).Shape();
    gp_Trsf trsf;
    trsf.SetTranslation(gp_Vec(tx, ty, tz));
    box.Location(TopLoc_Location(trsf));
    return box;
}

Assembly::InterferenceScanResult makeResult(int penetrations, const char* statusTag)
{
    Assembly::InterferenceScanResult result;
    result.complete = true;
    result.counts.penetrations = penetrations;

    Assembly::InterferenceLeaf leafA;
    leafA.displayPath = std::string(statusTag) + "_A";
    leafA.occurrenceSubName = "A.";
    leafA.sourceId = "doc#A";
    leafA.shapeValid = true;
    leafA.visible = true;

    Assembly::InterferenceLeaf leafB;
    leafB.displayPath = std::string(statusTag) + "_B";
    leafB.occurrenceSubName = "B.";
    leafB.sourceId = "doc#B";
    leafB.shapeValid = true;
    leafB.visible = true;

    result.leaves.push_back(leafA);
    result.leaves.push_back(leafB);

    Assembly::InterferencePairResult pair;
    pair.leafIndexA = 0;
    pair.leafIndexB = 1;
    pair.detection.kind = Part::InterferenceKind::Penetration;
    pair.detection.minimumDistance = 0.0;
    pair.detection.overlapVolume = 1.0;
    result.pairs.push_back(pair);
    return result;
}

Assembly::InterferenceScanResult makePlacedPenetrationResult()
{
    auto result = makeResult(1, "P");
    // Leaf A at origin; leaf B translated so preview transform must be non-identity.
    result.leaves[0].worldShape = makePlacedBox(10, 10, 10, 0, 0, 0);
    result.leaves[1].worldShape = makePlacedBox(10, 10, 10, 40, 15, 7);
    return result;
}

void ensureDefaultOffscreenQtPlatform()
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
}

}  // namespace

class TaskInterferenceCheckTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        ensureDefaultOffscreenQtPlatform();
        static int argc = 1;
        static char arg0[] = "AssemblyGui_tests_run";
        static char* argv[] = {arg0, nullptr};
        if (!QApplication::instance()) {
            // Intentionally leaked: FreeCAD/Qt teardown order crashes in offscreen gtests.
            new QApplication(argc, argv);
        }
        tests::initApplication();
        // Minimal Inventor bootstrap for SoPreviewShape (no Gui::Application / MainWindow).
        if (!SoDB::isInitialized()) {
            SoDB::init();
            SoInteraction::init();
        }
        static bool previewNodeReady = false;
        if (!previewNodeReady) {
            PartGui::SoBrepFaceSet::initClass();
            PartGui::SoBrepEdgeSet::initClass();
            PartGui::SoBrepPointSet::initClass();
            PartGui::SoFCShape::initClass();
            Gui::So3DAnnotation::initClass();
            PartGui::SoPreviewShape::initClass();
            previewNodeReady = true;
        }
        Base::Interpreter().runString("import Part");
        Base::Interpreter().runString("import Material");
        Base::Interpreter().runString("import Spreadsheet");
        Base::Interpreter().runString("import AssemblyApp");
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("asmGuiInterference");
        App::DocumentInitFlags flags;
        flags.createView = false;
        _doc = App::GetApplication().newDocument(_docName.c_str(), "asmGuiInterferenceUser", flags);
        _assembly = _doc->addObject<Assembly::AssemblyObject>("Assembly");
    }

    void TearDown() override
    {
        if (App::GetApplication().getDocument(_docName.c_str())) {
            App::GetApplication().closeDocument(_docName.c_str());
        }
        _doc = nullptr;
        _assembly = nullptr;
    }

    App::Document* _doc = nullptr;
    Assembly::AssemblyObject* _assembly = nullptr;
    std::string _docName;
};

TEST_F(TaskInterferenceCheckTest, bThenALateFinishDoesNotMutateNewerUiState)
{
    if (qgetenv("ASSEMBLYGUI_REQUIRE_XCB") == QByteArray("1")) {
        ASSERT_TRUE(QApplication::instance());
        const QString platform = QApplication::platformName();
        ASSERT_EQ(platform, QStringLiteral("xcb"))
            << "QT_QPA_PLATFORM=" << qgetenv("QT_QPA_PLATFORM").constData();
        std::cerr << "ASSEMBLYGUI_PLATFORM_GUARD_OK platform=" << qPrintable(platform) << '\n';
    }

    AssemblyGui::TaskInterferenceCheck task(_assembly);

    auto& session = task.scanSession();
    const auto scanA = session.beginScan();
    const auto scanB = session.beginScan();
    ASSERT_NE(scanA.generation, scanB.generation);
    ASSERT_EQ(session.activeGeneration(), scanB.generation);

    const auto resultB = makeResult(2, "B");
    const auto resultA = makeResult(9, "A");

    // B completes first and owns the UI.
    task.testDeliverScanFinished(scanB.generation, resultB);
    EXPECT_FALSE(session.isBusy());
    EXPECT_EQ(session.activeGeneration(), scanB.generation);
    EXPECT_TRUE(task.hasResults());
    EXPECT_EQ(task.testResultPairCount(), 1u);
    EXPECT_EQ(task.testTableRowCount(), 1);
    EXPECT_EQ(task.testTableCellText(0, 1), QStringLiteral("B_A"));
    EXPECT_EQ(task.testTableCellText(0, 2), QStringLiteral("B_B"));
    const QString statusAfterB = task.testStatusText();
    const QString progressAfterB = task.testProgressText();
    EXPECT_TRUE(statusAfterB.contains(QStringLiteral("complete"), Qt::CaseInsensitive));
    EXPECT_FALSE(statusAfterB.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive));

    // Late A must be a pure no-op against status/progress/results/actions/table.
    task.testDeliverScanFinished(scanA.generation, resultA);
    EXPECT_EQ(session.activeGeneration(), scanB.generation);
    EXPECT_FALSE(session.isBusy());
    EXPECT_EQ(task.testResultPairCount(), 1u);
    EXPECT_EQ(task.testTableRowCount(), 1);
    EXPECT_EQ(task.testStatusText(), statusAfterB);
    EXPECT_EQ(task.testProgressText(), progressAfterB);
    EXPECT_EQ(task.testTableCellText(0, 1), QStringLiteral("B_A"));
    EXPECT_EQ(task.testTableCellText(0, 2), QStringLiteral("B_B"));
    EXPECT_NE(task.testTableCellText(0, 1), QStringLiteral("A_A"));
}

TEST_F(TaskInterferenceCheckTest, constructsWithoutMainWindow)
{
    // Headless/offscreen: no MainWindow. attachPreviewToViewer must no-op safely.
    ASSERT_EQ(Gui::getMainWindow(), nullptr);
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    EXPECT_FALSE(task.isScanning());
    EXPECT_FALSE(task.hasResults());
    EXPECT_FALSE(task.testStatusText().isEmpty());
    EXPECT_FALSE(task.testHasPreviewRoot());
}

TEST_F(TaskInterferenceCheckTest, defaultScopeIsAllVisibleComponentsWhenNothingSelected)
{
    ASSERT_EQ(Gui::getMainWindow(), nullptr);
    Gui::Selection().clearSelection();
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testRefreshScanScope();
    EXPECT_FALSE(task.testIsSelectedPairMode());
    EXPECT_TRUE(task.testScopeText().contains(QStringLiteral("Scan scope:"), Qt::CaseInsensitive));
    EXPECT_TRUE(
        task.testScopeText().contains(QStringLiteral("visible"), Qt::CaseInsensitive)
        || task.testScopeText().contains(QStringLiteral("component"), Qt::CaseInsensitive)
    );
}

TEST_F(TaskInterferenceCheckTest, lockedSelectedPairScopeShowsBothComponents)
{
    ASSERT_EQ(Gui::getMainWindow(), nullptr);
    Assembly::InterferenceComponentOccurrence a;
    a.component = _assembly;
    a.occurrencePrefix = "CompA.";
    a.displayPath = "CompA";
    Assembly::InterferenceComponentOccurrence b;
    b.component = _assembly;
    b.occurrencePrefix = "CompB.";
    b.displayPath = "CompB";
    AssemblyGui::TaskInterferenceCheck task(_assembly, a, b);
    EXPECT_TRUE(task.testIsSelectedPairMode());
    EXPECT_TRUE(task.testScopeText().contains(QStringLiteral("CompA")));
    EXPECT_TRUE(task.testScopeText().contains(QStringLiteral("CompB")));
    EXPECT_TRUE(task.testScopeText().contains(QStringLiteral("selected pair"), Qt::CaseInsensitive));

    // Locked dialog ignores selection-handle changes.
    task.testSetSelectionHandles(
        {{_assembly, "Other.Face1"}, {_assembly, "AlsoOther.Face1"}}
    );
    task.testNotifySelectionChanged();
    EXPECT_TRUE(task.testIsSelectedPairMode());
    EXPECT_TRUE(task.testScopeText().contains(QStringLiteral("CompA")));
}

TEST_F(TaskInterferenceCheckTest, selectionCardinalityDrivesScopeMode)
{
    auto* partA = _doc->addObject<App::Part>("CompA");
    auto* boxA = _doc->addObject<Part::Box>("BoxA");
    boxA->Length.setValue(10);
    boxA->Width.setValue(10);
    boxA->Height.setValue(10);
    partA->addObject(boxA);
    auto* partB = _doc->addObject<App::Part>("CompB");
    auto* boxB = _doc->addObject<Part::Box>("BoxB");
    boxB->Length.setValue(10);
    boxB->Width.setValue(10);
    boxB->Height.setValue(10);
    partB->addObject(boxB);
    auto* partC = _doc->addObject<App::Part>("CompC");
    auto* boxC = _doc->addObject<Part::Box>("BoxC");
    boxC->Length.setValue(10);
    boxC->Width.setValue(10);
    boxC->Height.setValue(10);
    partC->addObject(boxC);
    _assembly->addObject(partA);
    _assembly->addObject(partB);
    _assembly->addObject(partC);
    _doc->recompute();

    AssemblyGui::TaskInterferenceCheck task(_assembly);

    task.testClearSelectionHandles();
    task.testRefreshScanScope();
    EXPECT_FALSE(task.testIsSelectedPairMode());
    EXPECT_TRUE(task.testScopeText().contains(QStringLiteral("3")));

    task.testSetSelectionHandles({{_assembly, "CompA.BoxA.Face1"}});
    task.testRefreshScanScope();
    EXPECT_FALSE(task.testIsSelectedPairMode());

    task.testSetSelectionHandles(
        {{_assembly, "CompA.BoxA.Face1"}, {_assembly, "CompB.BoxB.Face1"}}
    );
    task.testRefreshScanScope();
    EXPECT_TRUE(task.testIsSelectedPairMode());
    EXPECT_TRUE(task.testScopeText().contains(QStringLiteral("CompA"), Qt::CaseInsensitive)
                || task.testScopeText().contains(QStringLiteral("selected pair"), Qt::CaseInsensitive));

    task.testSetSelectionHandles(
        {{_assembly, "CompA.BoxA.Face1"}, {_assembly, "CompA.BoxA.Face2"}}
    );
    task.testRefreshScanScope();
    EXPECT_FALSE(task.testIsSelectedPairMode());

    task.testSetSelectionHandles(
        {{_assembly, "CompA.BoxA.Face1"},
         {_assembly, "CompB.BoxB.Face1"},
         {_assembly, "CompC.BoxC.Face1"}}
    );
    task.testRefreshScanScope();
    EXPECT_FALSE(task.testIsSelectedPairMode());

    // Three faces that collapse to two occurrences still require exact two handles.
    task.testSetSelectionHandles(
        {{_assembly, "CompA.BoxA.Face1"},
         {_assembly, "CompA.BoxA.Face2"},
         {_assembly, "CompB.BoxB.Face1"}}
    );
    task.testRefreshScanScope();
    EXPECT_FALSE(task.testIsSelectedPairMode());

    // Whole-object tree selections of two distinct components are SelectedPair.
    task.testSetSelectionHandles({{partA, {}}, {partB, {}}});
    task.testRefreshScanScope();
    EXPECT_TRUE(task.testIsSelectedPairMode());
    EXPECT_FALSE(task.testIsIncludeHiddenEnabled());

    // A third whole-object endpoint disables selected-pair mode.
    task.testSetSelectionHandles({{partA, {}}, {partB, {}}, {partC, {}}});
    task.testRefreshScanScope();
    EXPECT_FALSE(task.testIsSelectedPairMode());

    // Unresolvable subnames are ignored safely.
    task.testSetSelectionHandles(
        {{_assembly, "NoSuch.Face1"}, {_assembly, "AlsoMissing.Face1"}}
    );
    task.testRefreshScanScope();
    EXPECT_FALSE(task.testIsSelectedPairMode());
}

TEST_F(TaskInterferenceCheckTest, selectionChangeWhileBusySyncsScopeAfterFinish)
{
    auto* partA = _doc->addObject<App::Part>("BusyA");
    auto* boxA = _doc->addObject<Part::Box>("BusyBoxA");
    boxA->Length.setValue(10);
    boxA->Width.setValue(10);
    boxA->Height.setValue(10);
    partA->addObject(boxA);
    auto* partB = _doc->addObject<App::Part>("BusyB");
    auto* boxB = _doc->addObject<Part::Box>("BusyBoxB");
    boxB->Length.setValue(10);
    boxB->Width.setValue(10);
    boxB->Height.setValue(10);
    partB->addObject(boxB);
    _assembly->addObject(partA);
    _assembly->addObject(partB);
    _doc->recompute();

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testClearSelectionHandles();
    task.testRefreshScanScope();
    EXPECT_FALSE(task.testIsSelectedPairMode());

    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    ASSERT_TRUE(session.isBusy());

    task.testSetSelectionHandles(
        {{_assembly, "BusyA.BusyBoxA.Face1"}, {_assembly, "BusyB.BusyBoxB.Face1"}}
    );
    task.testNotifySelectionChanged();
    // Still all-visible while busy; refresh is deferred.
    EXPECT_FALSE(task.testIsSelectedPairMode());

    task.testDeliverScanFinished(scan.generation, makeResult(0, "idle"));
    EXPECT_FALSE(session.isBusy());
    EXPECT_TRUE(task.testIsSelectedPairMode());
    EXPECT_TRUE(task.testScopeText().contains(QStringLiteral("selected pair"), Qt::CaseInsensitive));
}

TEST_F(TaskInterferenceCheckTest, mutatePropertyDuringPreparationInvalidatesGeneration)
{
    auto* part = _doc->addObject<App::Part>("MutPart");
    auto* box = _doc->addObject<Part::Box>("MutBox");
    box->Length.setValue(10);
    box->Width.setValue(10);
    box->Height.setValue(10);
    part->addObject(box);
    _assembly->addObject(part);
    _doc->recompute();

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    auto& session = task.scanSession();
    const auto seed = session.beginScan();
    task.testDeliverScanFinished(seed.generation, makeResult(1, "seed"));
    ASSERT_TRUE(task.hasResults());

    task.testSetPreparationBarrier([part]() {
        part->Placement.setValue(Base::Placement(Base::Vector3d(5, 0, 0), Base::Rotation()));
    });
    task.testRunScan();
    EXPECT_FALSE(task.hasResults());
    EXPECT_TRUE(
        session.isStale()
        || task.testStatusText().contains(QStringLiteral("stale"), Qt::CaseInsensitive)
        || task.testStatusText().contains(QStringLiteral("cancel"), Qt::CaseInsensitive)
    );
    task.testClearPreparationBarrier();
}

TEST_F(TaskInterferenceCheckTest, includeHiddenToggleUpdatesScopeAndMarksStale)
{
    auto* visible = _doc->addObject<App::Part>("VisibleP");
    auto* boxV = _doc->addObject<Part::Box>("BoxV");
    boxV->Length.setValue(10);
    boxV->Width.setValue(10);
    boxV->Height.setValue(10);
    visible->addObject(boxV);
    auto* hidden = _doc->addObject<App::Part>("HiddenP");
    hidden->Visibility.setValue(false);
    auto* boxH = _doc->addObject<Part::Box>("BoxH");
    boxH->Length.setValue(10);
    boxH->Width.setValue(10);
    boxH->Height.setValue(10);
    hidden->addObject(boxH);
    _assembly->addObject(visible);
    _assembly->addObject(hidden);
    _doc->recompute();

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    Gui::Selection().clearSelection();
    task.testSetIncludeHidden(false);
    task.testRefreshScanScope();
    EXPECT_TRUE(task.testScopeText().contains(QStringLiteral("1")));
    EXPECT_FALSE(task.testScopeText().contains(QStringLiteral("HiddenP")));

    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, makeResult(1, "T"));
    ASSERT_TRUE(task.hasResults());

    task.testSetIncludeHidden(true);
    EXPECT_FALSE(task.hasResults());
    EXPECT_TRUE(
        task.testStatusText().contains(QStringLiteral("stale"), Qt::CaseInsensitive)
        || task.testScopeText().contains(QStringLiteral("HiddenP"))
    );
    task.testRefreshScanScope();
    EXPECT_TRUE(task.testScopeText().contains(QStringLiteral("2")));
    EXPECT_TRUE(task.testScopeText().contains(QStringLiteral("HiddenP")));
}

TEST_F(TaskInterferenceCheckTest, hostDeleteDuringPreparationAbortsWithoutResults)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testSetPreparationBarrier([&task, this]() {
        EXPECT_TRUE(task.testIsPreparing());
        EXPECT_FALSE(task.testIsCancelEnabled());
        EXPECT_TRUE(
            task.testStatusText().contains(QStringLiteral("Preparing"), Qt::CaseInsensitive)
        );
        // Cancel before destroying the document so prepare never touches a
        // dangling host pointer after this barrier returns.
        task.scanSession().requestCancel();
        App::GetApplication().closeDocument(_docName.c_str());
        _doc = nullptr;
        _assembly = nullptr;
        EXPECT_FALSE(task.testHasHost());
    });
    task.testRunScan();
    EXPECT_FALSE(task.testIsPreparing());
    EXPECT_FALSE(task.hasResults());
    EXPECT_FALSE(task.testHasHost());
    task.testClearPreparationBarrier();
}

TEST_F(TaskInterferenceCheckTest, preparingStateDisablesCancelUntilWorkerStarts)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    bool sawPreparing = false;
    bool cancelDisabledWhilePreparing = false;
    task.testSetPreparationBarrier([&]() {
        sawPreparing = task.testIsPreparing();
        cancelDisabledWhilePreparing = !task.testIsCancelEnabled();
        EXPECT_TRUE(
            task.testStatusText().contains(QStringLiteral("Preparing"), Qt::CaseInsensitive)
        );
        // Abort before expensive extract so the test stays deterministic.
        task.scanSession().requestCancel();
    });
    task.testRunScan();
    EXPECT_TRUE(sawPreparing);
    EXPECT_TRUE(cancelDisabledWhilePreparing);
    EXPECT_FALSE(task.testIsPreparing());
    EXPECT_FALSE(task.hasResults());
    task.testClearPreparationBarrier();
}

TEST_F(TaskInterferenceCheckTest, clearanceEditorUsesLengthQuantitySchema)
{
    _assembly->setInterferenceClearance(2.5);
    AssemblyGui::TaskInterferenceCheck task(_assembly);

    EXPECT_EQ(task.testClearanceUnit(), Base::Unit::Length);
    EXPECT_NEAR(task.testClearanceRawMm(), 2.5, 1e-12);

    // Accept unit-bearing quantities; internal storage remains millimetres.
    task.testSetClearanceQuantity(Base::Quantity::parse("1 in"));
    EXPECT_NEAR(task.testClearanceRawMm(), 25.4, 1e-6);

    task.testSetClearanceQuantity(Base::Quantity(0.0, Base::Unit::Length));
    EXPECT_NEAR(task.testClearanceRawMm(), 0.0, 1e-12);
}

TEST_F(TaskInterferenceCheckTest, placedLeafPreviewRestoresWorldTransform)
{
    // Without MainWindow, attach a detached Inventor root and prove placed shapes
    // keep their world translation on SoPreviewShape::transform (setupCoinGeometry strips location).
    ASSERT_EQ(Gui::getMainWindow(), nullptr);
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testEnsureDetachedPreviewRoot();
    ASSERT_TRUE(task.testHasPreviewRoot());

    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, makePlacedPenetrationResult());
    ASSERT_EQ(task.testTableRowCount(), 1);
    task.testSelectResultRow(0);

    // Two leaf previews (no common shape in the synthetic result).
    ASSERT_EQ(task.testPreviewShapeCount(), 2);

    double x0 = 0, y0 = 0, z0 = 0;
    double x1 = 0, y1 = 0, z1 = 0;
    ASSERT_TRUE(task.testPreviewShapeTranslation(0, x0, y0, z0));
    ASSERT_TRUE(task.testPreviewShapeTranslation(1, x1, y1, z1));
    EXPECT_NEAR(x0, 0.0, 1e-4);
    EXPECT_NEAR(y0, 0.0, 1e-4);
    EXPECT_NEAR(z0, 0.0, 1e-4);
    EXPECT_NEAR(x1, 40.0, 1e-4);
    EXPECT_NEAR(y1, 15.0, 1e-4);
    EXPECT_NEAR(z1, 7.0, 1e-4);

    // Teardown path: reject clears preview children via discard/detach.
    EXPECT_TRUE(task.reject());
    EXPECT_FALSE(task.hasResults());
    EXPECT_FALSE(task.testHasPreviewRoot());
}

TEST_F(TaskInterferenceCheckTest, documentCloseDiscardsResultsAndClosesManageExclusions)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, makeResult(1, "T"));
    ASSERT_TRUE(task.hasResults());

    task.testOpenManageExclusions();
    QApplication::processEvents();
    ASSERT_TRUE(task.testManageExclusionsOpen());
    ASSERT_TRUE(task.testHasHost());

    App::GetApplication().closeDocument(_docName.c_str());
    _doc = nullptr;
    _assembly = nullptr;
    QApplication::processEvents();

    EXPECT_FALSE(task.testHasHost());
    EXPECT_FALSE(task.hasResults());
    EXPECT_FALSE(task.testManageExclusionsOpen());
    EXPECT_TRUE(task.testStatusText().contains(QStringLiteral("Document closed"), Qt::CaseInsensitive)
                || task.testStatusText().contains(QStringLiteral("stale"), Qt::CaseInsensitive));
}

TEST_F(TaskInterferenceCheckTest, linkedDocumentCloseMarksResultsStaleAndClosesManageExclusions)
{
    const std::string linkedDocName =
        App::GetApplication().getUniqueDocumentName("asmGuiLinkedExt");
    const std::string unrelatedDocName =
        App::GetApplication().getUniqueDocumentName("asmGuiUnrelated");

    const std::string ownerPath = std::string("/tmp/") + _docName + "_linkedDocClose.FCStd";
    const std::string linkedPath =
        std::string("/tmp/") + linkedDocName + "_linkedDocClose.FCStd";

    App::Document* linkedDoc = nullptr;
    App::Document* unrelatedDoc = nullptr;

    auto cleanupExtraDocumentsAndFiles = [&]() {
        if (unrelatedDoc && App::GetApplication().getDocument(unrelatedDocName.c_str())) {
            App::GetApplication().closeDocument(unrelatedDocName.c_str());
        }
        unrelatedDoc = nullptr;
        if (linkedDoc && App::GetApplication().getDocument(linkedDocName.c_str())) {
            App::GetApplication().closeDocument(linkedDocName.c_str());
        }
        linkedDoc = nullptr;
        std::remove(ownerPath.c_str());
        std::remove(linkedPath.c_str());
    };

    struct LinkedDocCloseCleanup
    {
        std::function<void()> cleanup;
        ~LinkedDocCloseCleanup()
        {
            if (cleanup) {
                cleanup();
            }
        }
    } linkedDocCloseCleanup {cleanupExtraDocumentsAndFiles};

    linkedDoc = App::GetApplication().newDocument(
        linkedDocName.c_str(),
        "asmGuiLinkedExtUser");
    auto* foreignPart = linkedDoc->addObject<App::Part>("LinkedForeignPart");
    auto* linkedBox = linkedDoc->addObject<Part::Box>("LinkedPhysicalBox");
    linkedBox->Length.setValue(10);
    linkedBox->Width.setValue(10);
    linkedBox->Height.setValue(10);
    foreignPart->addObject(linkedBox);
    linkedDoc->recompute();

    _doc->FileName.setValue(ownerPath.c_str());
    linkedDoc->FileName.setValue(linkedPath.c_str());
    ASSERT_TRUE(_doc->save()) << "Failed to save owner at " << ownerPath;
    ASSERT_TRUE(linkedDoc->save()) << "Failed to save linked document at " << linkedPath;

    auto* extLink = freecad_cast<App::Link*>(_doc->addObject("App::Link", "LinkedExtPart"));
    extLink->setLink(-1, foreignPart);
    _assembly->addObject(extLink);
    _doc->recompute();

    unrelatedDoc = App::GetApplication().newDocument(
        unrelatedDocName.c_str(),
        "asmGuiUnrelatedUser");

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, makeResult(1, "LinkedClose"));
    ASSERT_TRUE(task.hasResults());

    task.testOpenManageExclusions();
    QApplication::processEvents();
    ASSERT_TRUE(task.testManageExclusionsOpen());
    ASSERT_TRUE(task.testHasHost());

    App::GetApplication().closeDocument(unrelatedDocName.c_str());
    unrelatedDoc = nullptr;
    QApplication::processEvents();

    EXPECT_TRUE(task.testHasHost());
    EXPECT_TRUE(task.hasResults());
    EXPECT_TRUE(task.testManageExclusionsOpen());
    EXPECT_FALSE(task.testStatusText().contains(
        QStringLiteral("Linked document closed"),
        Qt::CaseInsensitive));

    App::GetApplication().closeDocument(linkedDocName.c_str());
    linkedDoc = nullptr;
    QApplication::processEvents();

    ASSERT_NE(_doc, nullptr);
    EXPECT_TRUE(App::GetApplication().getDocument(_docName.c_str()) != nullptr);
    EXPECT_TRUE(task.testHasHost());
    EXPECT_FALSE(task.hasResults());
    EXPECT_FALSE(task.testManageExclusionsOpen());
    EXPECT_TRUE(
        task.testStatusText().contains(QStringLiteral("Linked document closed"), Qt::CaseInsensitive)
        || task.testStatusText().contains(QStringLiteral("stale"), Qt::CaseInsensitive));
}

TEST_F(TaskInterferenceCheckTest, previewAttachesAndDetachesFromSceneGraph)
{
    // Proves the shared attachPreviewToScene / detach path used by attachPreviewToViewer,
    // without requiring FreeCADCmd / MainWindow / View3DInventor.
    ASSERT_EQ(Gui::getMainWindow(), nullptr);
    AssemblyGui::TaskInterferenceCheck task(_assembly);

    auto* scene = new SoSeparator;
    scene->ref();
    const int before = scene->getNumChildren();

    task.testAttachPreviewToScene(scene);
    ASSERT_TRUE(task.testHasPreviewRoot());
    EXPECT_EQ(scene->getNumChildren(), before + 1);
    EXPECT_GE(task.testPreviewIndexInScene(scene), 0);

    task.testDetachPreview();
    EXPECT_FALSE(task.testHasPreviewRoot());
    EXPECT_EQ(scene->getNumChildren(), before);
    EXPECT_EQ(task.testPreviewIndexInScene(scene), -1);

    scene->unref();
}

TEST_F(TaskInterferenceCheckTest, commandPathWholeObjectHiddenPairFindsPenetration)
{
    // Command-path seam for Assembly_CheckSelectedComponents:
    // whole-object tree handles (identical to SelectionEx with empty subNames) →
    // resolveInterferenceSelectedPairRequest (same gate as command isActive/activated) →
    // TaskInterferenceCheck(host, first, second) as activated() constructs the dialog.
    // Gui::Selection::addSelection is not used here: it requires Gui::Application::macroManager
    // which this headless AssemblyGui_tests_run binary does not bootstrap.
    auto* casePart = _doc->addObject<App::Part>("AssemblyCase");
    auto* caseBox = _doc->addObject<Part::Box>("CaseBox");
    caseBox->Length.setValue(20);
    caseBox->Width.setValue(20);
    caseBox->Height.setValue(20);
    casePart->addObject(caseBox);
    casePart->Visibility.setValue(false);

    auto* spool = _doc->addObject<App::Part>("AssemblySpool");
    spool->Placement.setValue(Base::Placement(Base::Vector3d(10, 0, 0), Base::Rotation()));
    auto* spoolBox = _doc->addObject<Part::Box>("SpoolBox");
    spoolBox->Length.setValue(20);
    spoolBox->Width.setValue(20);
    spoolBox->Height.setValue(20);
    spool->addObject(spoolBox);

    auto* gear = _doc->addObject<App::Part>("GearMeshAssembly");
    auto* gearBox = _doc->addObject<Part::Box>("GearBox");
    gearBox->Length.setValue(5);
    gearBox->Width.setValue(5);
    gearBox->Height.setValue(5);
    gear->Placement.setValue(Base::Placement(Base::Vector3d(100, 0, 0), Base::Rotation()));
    gear->addObject(gearBox);

    _assembly->addObject(casePart);
    _assembly->addObject(spool);
    _assembly->addObject(gear);
    _doc->recompute();

    // Whole-object SelectionEx handles (empty subName), as produced by tree Ctrl-select.
    std::vector<Assembly::InterferenceSelectionHandle> handles {
        {casePart, {}},
        {spool, {}}
    };

    auto request = Assembly::resolveInterferenceSelectedPairRequest(handles, _assembly);
    ASSERT_TRUE(request.valid()) << "Check Selected Components must be active";
    EXPECT_EQ(request.host, _assembly);
    EXPECT_TRUE(
        (request.first.occurrencePrefix == "AssemblyCase."
         && request.second.occurrencePrefix == "AssemblySpool.")
        || (request.first.occurrencePrefix == "AssemblySpool."
            && request.second.occurrencePrefix == "AssemblyCase.")
    );

    // Third whole-object endpoint must keep the command inactive (no all-components fallback).
    {
        auto invalid = Assembly::resolveInterferenceSelectedPairRequest(
            {{casePart, {}}, {spool, {}}, {gear, {}}},
            _assembly
        );
        EXPECT_FALSE(invalid.valid());
    }

    AssemblyGui::TaskInterferenceCheck task(request.host, request.first, request.second);
    EXPECT_TRUE(task.testIsSelectedPairMode());
    EXPECT_TRUE(task.testScopeText().contains(QStringLiteral("selected pair"), Qt::CaseInsensitive));
    EXPECT_TRUE(
        task.testScopeText().contains(QStringLiteral("Case"), Qt::CaseInsensitive)
        || task.testScopeText().contains(QStringLiteral("AssemblyCase"))
    );
    EXPECT_FALSE(task.testIsIncludeHiddenEnabled());

    task.testSetIncludeHidden(false);
    task.testRunScan();

    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(60000);
    while (task.isScanning() && timeout.isActive()) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    for (int i = 0; i < 50 && !task.hasResults() && !task.isScanning(); ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }

    EXPECT_FALSE(task.isScanning());
    EXPECT_TRUE(task.hasResults());
    EXPECT_GE(task.testPenetrationCount(), 1);
    EXPECT_FALSE(casePart->Visibility.getValue());

    for (std::size_t i = 0; i < task.testResultPairCount(); ++i) {
        EXPECT_FALSE(task.testTableCellText(static_cast<int>(i), 1)
                         .contains(QStringLiteral("GearMesh"), Qt::CaseInsensitive));
        EXPECT_FALSE(task.testTableCellText(static_cast<int>(i), 2)
                         .contains(QStringLiteral("GearMesh"), Qt::CaseInsensitive));
    }
}

TEST_F(TaskInterferenceCheckTest, clearanceSheetChangeMarksResultsStale)
{
    auto* sheet = _doc->addObject<Spreadsheet::Sheet>("FaceClearances");
    ASSERT_NE(sheet, nullptr);
    sheet->setCell("A1", "Enabled");
    sheet->setCell("B1", "Face");
    sheet->setCell("C1", "Tolerance");
    sheet->setCell("A2", "true");
    sheet->setCell("B2", "*");
    sheet->setCell("C2", "0.05");
    _doc->recompute();
    _assembly->setInterferenceClearanceSheet(sheet);

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    EXPECT_TRUE(task.testClearanceSheetLabel().contains(QStringLiteral("FaceClearances"))
                || task.testClearanceSheetLabel().contains(QStringLiteral("sheet"), Qt::CaseInsensitive));

    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, makeResult(1, "SheetStale"));
    ASSERT_TRUE(task.hasResults());
    EXPECT_FALSE(session.isStale());

    sheet->setCell("C2", "0.20");
    _doc->recompute();

    EXPECT_TRUE(session.isStale());
    EXPECT_TRUE(task.testStatusText().contains(QStringLiteral("stale"), Qt::CaseInsensitive));
}

TEST_F(TaskInterferenceCheckTest, defaultAllComponentsPathConsumesLinkedClearanceSheet)
{
    auto* boxA = _doc->addObject<Part::Box>("SheetBoxA");
    boxA->Length.setValue(10);
    boxA->Width.setValue(10);
    boxA->Height.setValue(10);
    boxA->Placement.setValue(Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()));
    auto* boxB = _doc->addObject<Part::Box>("SheetBoxB");
    boxB->Length.setValue(10);
    boxB->Width.setValue(10);
    boxB->Height.setValue(10);
    boxB->Placement.setValue(Base::Placement(Base::Vector3d(10.4, 0, 0), Base::Rotation()));
    _assembly->addObject(boxA);
    _assembly->addObject(boxB);

    auto* sheet = _doc->addObject<Spreadsheet::Sheet>("LinkedClearances");
    sheet->setCell("A1", "Enabled");
    sheet->setCell("B1", "Face");
    sheet->setCell("C1", "Tolerance");
    sheet->setCell("D1", "Comment");
    sheet->setCell("A2", "true");
    sheet->setCell("B2", "*");
    sheet->setCell("C2", "0.5");
    sheet->setCell("D2", "linked-star");
    _doc->recompute();
    _assembly->setInterferenceClearanceSheet(sheet);
    Assembly::setInterferenceClearance(_assembly, 0.0);

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    EXPECT_FALSE(task.testIsSelectedPairMode());
    EXPECT_TRUE(task.testClearanceSheetLabel().contains(QStringLiteral("LinkedClearances"))
                || task.testClearanceSheetLabel().contains(QStringLiteral("sheet"), Qt::CaseInsensitive));

    task.testRunScan();
    auto& session = task.scanSession();
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(60000);
    while ((session.isBusy() || task.isScanning()) && timeout.isActive()) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    for (int i = 0; i < 50 && !task.hasResults() && !task.isScanning(); ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    ASSERT_TRUE(task.hasResults());
    ASSERT_GE(task.testResultPairCount(), 1u);

    bool sawStar = false;
    for (int row = 0; row < task.testTableRowCount(); ++row) {
        const QString faces = task.testTableCellText(row, 6);
        const QString rules = task.testTableCellText(row, 7);
        if (rules.contains(QStringLiteral("Default"))
            || rules.contains(QStringLiteral("row 2"))
            || rules.contains(QStringLiteral("linked-star"))) {
            sawStar = true;
        }
        if (faces.contains(QStringLiteral("[Clearance]"))) {
            EXPECT_TRUE(rules.contains(QStringLiteral("row 2"))
                        || rules.contains(QStringLiteral("linked-star"))
                        || rules.contains(QStringLiteral("Default")));
        }
    }
    EXPECT_TRUE(sawStar);
    EXPECT_GE(task.testTableRowCount(), 1);
}

TEST_F(TaskInterferenceCheckTest, defaultAllComponentsExpandedArraySheetShowsClearanceNotSilentClear)
{
    auto* source = _doc->addObject<Part::Box>("GuiArrSrc");
    source->Length.setValue(10);
    source->Width.setValue(10);
    source->Height.setValue(10);
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "GuiExpArr"));
    link->setLink(-1, source);
    link->ShowElement.setValue(true);
    link->ElementCount.setValue(2);
    _assembly->addObject(link);
    _doc->recompute();
    ASSERT_EQ(link->ElementList.getSize(), 2);
    auto* elt0 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[0]);
    auto* elt1 = freecad_cast<App::LinkElement*>(link->ElementList.getValues()[1]);
    ASSERT_NE(elt0, nullptr);
    ASSERT_NE(elt1, nullptr);
    elt0->Placement.setValue(Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()));
    elt1->Placement.setValue(Base::Placement(Base::Vector3d(10.4, 0, 0), Base::Rotation()));
    _doc->recompute();

    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    const double sepX = leaves[1].worldBoundBox.MinX - leaves[0].worldBoundBox.MaxX;
    ASSERT_NEAR(sepX, 0.4, 0.05);

    const auto [closeA, closeB] = [&]() {
        TopTools_IndexedMapOfShape mapA;
        TopTools_IndexedMapOfShape mapB;
        TopExp::MapShapes(leaves[0].worldShape, TopAbs_FACE, mapA);
        TopExp::MapShapes(leaves[1].worldShape, TopAbs_FACE, mapB);
        for (int i = 1; i <= mapA.Extent(); ++i) {
            for (int j = 1; j <= mapB.Extent(); ++j) {
                BRepExtrema_DistShapeShape dist(mapA(i), mapB(j));
                if (dist.IsDone() && dist.NbSolution() >= 1 && dist.Value() < 0.6) {
                    return std::pair {
                        leaves[0].occurrenceSubName + "Face" + std::to_string(i),
                        leaves[1].occurrenceSubName + "Face" + std::to_string(j)};
                }
            }
        }
        return std::pair<std::string, std::string> {};
    }();
    ASSERT_FALSE(closeA.empty());
    ASSERT_FALSE(closeB.empty());

    const std::string aliasInput = std::string(link->getNameInDocument()) + ".0."
        + closeA.substr(closeA.find_last_of('.') + 1);

    auto* sheet = _doc->addObject<Spreadsheet::Sheet>("GuiExpArrSheet");
    sheet->setCell("A1", "Enabled");
    sheet->setCell("B1", "Face");
    sheet->setCell("C1", "Tolerance");
    sheet->setCell("D1", "Comment");
    sheet->setCell("A2", "true");
    sheet->setCell("B2", aliasInput.c_str());
    sheet->setCell("C2", "0.5");
    sheet->setCell("D2", "gui-exp-face-rule");
    _doc->recompute();
    _assembly->setInterferenceClearanceSheet(sheet);
    Assembly::setInterferenceClearance(_assembly, 0.0);

    const auto table = Assembly::snapshotInterferenceClearanceRules(_assembly);
    ASSERT_EQ(table.invalidRuleCount, 0);
    ASSERT_FALSE(table.rules.empty());
    EXPECT_TRUE(table.rules[0].valid);
    EXPECT_EQ(table.rules[0].faceA, closeA);

    AssemblyGui::TaskInterferenceCheck taskAll(_assembly);
    taskAll.testRunScan();
    auto& sessionAll = taskAll.scanSession();
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(60000);
    while ((sessionAll.isBusy() || taskAll.isScanning()) && timeout.isActive()) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    for (int i = 0; i < 50 && !taskAll.hasResults() && !taskAll.isScanning(); ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    ASSERT_TRUE(taskAll.hasResults());

    bool sawViolation = false;
    int penetrationRows = 0;
    for (int row = 0; row < taskAll.testTableRowCount(); ++row) {
        const QString faces = taskAll.testTableCellText(row, 6);
        const QString rules = taskAll.testTableCellText(row, 7);
        if (faces.contains(QStringLiteral("[Penetration]"))) {
            ++penetrationRows;
        }
        if (faces.contains(QStringLiteral("[Clearance]"))) {
            sawViolation = true;
            EXPECT_TRUE(rules.contains(QStringLiteral("gui-exp-face-rule"))
                        || rules.contains(QStringLiteral("row 2")));
        }
    }
    EXPECT_TRUE(sawViolation);
    EXPECT_EQ(penetrationRows, 0);
    EXPECT_FALSE(
        taskAll.testStatusText().contains(QStringLiteral("incomplete"), Qt::CaseInsensitive)
    );
    ASSERT_GE(taskAll.testResultPairCount(), 1u);

    const std::string prefix0 =
        std::string(link->getNameInDocument()) + "." + elt0->getNameInDocument() + ".";
    const std::string prefix1 =
        std::string(link->getNameInDocument()) + "." + elt1->getNameInDocument() + ".";
    Assembly::InterferenceComponentOccurrence occA;
    Assembly::InterferenceComponentOccurrence occB;
    ASSERT_TRUE(Assembly::resolveInterferenceComponentOccurrence(
        _assembly,
        _assembly,
        prefix0 + "Face1",
        occA
    ));
    ASSERT_TRUE(Assembly::resolveInterferenceComponentOccurrence(
        _assembly,
        _assembly,
        prefix1 + "Face1",
        occB
    ));
    EXPECT_EQ(occA.occurrencePrefix, prefix0);
    EXPECT_EQ(occB.occurrencePrefix, prefix1);

    AssemblyGui::TaskInterferenceCheck taskPair(_assembly, occA, occB);
    EXPECT_TRUE(taskPair.testIsSelectedPairMode());
    taskPair.testRunScan();
    auto& sessionPair = taskPair.scanSession();
    QTimer timeoutPair;
    timeoutPair.setSingleShot(true);
    timeoutPair.start(60000);
    while ((sessionPair.isBusy() || taskPair.isScanning()) && timeoutPair.isActive()) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    for (int i = 0; i < 50 && !taskPair.hasResults() && !taskPair.isScanning(); ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    ASSERT_TRUE(taskPair.hasResults());
    EXPECT_EQ(taskPair.testResultPairCount(), taskAll.testResultPairCount());
    EXPECT_EQ(taskPair.testTableRowCount(), taskAll.testTableRowCount());
    bool sawPairViolation = false;
    for (int row = 0; row < taskPair.testTableRowCount(); ++row) {
        if (taskPair.testTableCellText(row, 6).contains(QStringLiteral("[Clearance]"))) {
            sawPairViolation = true;
        }
    }
    EXPECT_TRUE(sawPairViolation);
}

TEST_F(TaskInterferenceCheckTest, defaultAllComponentsLinkedSheetShowsClearanceNotSilentClear)
{
    auto* boxA = _doc->addObject<Part::Box>("GuiSheetBoxA");
    boxA->Length.setValue(10);
    boxA->Width.setValue(10);
    boxA->Height.setValue(10);
    boxA->Placement.setValue(Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()));
    auto* boxB = _doc->addObject<Part::Box>("GuiSheetBoxB");
    boxB->Length.setValue(10);
    boxB->Width.setValue(10);
    boxB->Height.setValue(10);
    boxB->Placement.setValue(Base::Placement(Base::Vector3d(10.4, 0, 0), Base::Rotation()));
    _assembly->addObject(boxA);
    _assembly->addObject(boxB);
    _doc->recompute();

    auto leaves = Assembly::collectInterferenceLeaves(_assembly, false);
    ASSERT_EQ(leaves.size(), 2u);
    TopTools_IndexedMapOfShape mapA;
    TopTools_IndexedMapOfShape mapB;
    TopExp::MapShapes(leaves[0].worldShape, TopAbs_FACE, mapA);
    TopExp::MapShapes(leaves[1].worldShape, TopAbs_FACE, mapB);
    std::string closeA;
    for (int i = 1; i <= mapA.Extent() && closeA.empty(); ++i) {
        for (int j = 1; j <= mapB.Extent(); ++j) {
            BRepExtrema_DistShapeShape dist(mapA(i), mapB(j));
            if (dist.IsDone() && dist.NbSolution() >= 1 && dist.Value() < 0.6) {
                closeA = leaves[0].occurrenceSubName + "Face" + std::to_string(i);
                break;
            }
        }
    }
    ASSERT_FALSE(closeA.empty());

    auto* sheet = _doc->addObject<Spreadsheet::Sheet>("GuiArrAliasSheet");
    sheet->setCell("A1", "Enabled");
    sheet->setCell("B1", "Face");
    sheet->setCell("C1", "Tolerance");
    sheet->setCell("D1", "Comment");
    sheet->setCell("A2", "true");
    sheet->setCell("B2", closeA.c_str());
    sheet->setCell("C2", "0.5");
    sheet->setCell("D2", "gui-face-rule");
    _doc->recompute();
    _assembly->setInterferenceClearanceSheet(sheet);
    Assembly::setInterferenceClearance(_assembly, 0.0);

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testRunScan();
    auto& session = task.scanSession();
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(60000);
    while ((session.isBusy() || task.isScanning()) && timeout.isActive()) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    for (int i = 0; i < 50 && !task.hasResults() && !task.isScanning(); ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    ASSERT_TRUE(task.hasResults());

    const auto table = Assembly::snapshotInterferenceClearanceRules(_assembly);
    ASSERT_EQ(table.invalidRuleCount, 0);
    ASSERT_FALSE(table.rules.empty());
    EXPECT_TRUE(table.rules[0].valid);
    EXPECT_EQ(table.rules[0].faceA, closeA);

    bool sawViolation = false;
    for (int row = 0; row < task.testTableRowCount(); ++row) {
        const QString faces = task.testTableCellText(row, 6);
        const QString rules = task.testTableCellText(row, 7);
        if (faces.contains(QStringLiteral("[Clearance]"))) {
            sawViolation = true;
            EXPECT_TRUE(rules.contains(QStringLiteral("gui-face-rule"))
                        || rules.contains(QStringLiteral("row 2")));
        }
    }
    EXPECT_TRUE(sawViolation);
    EXPECT_FALSE(
        task.testStatusText().contains(QStringLiteral("incomplete"), Qt::CaseInsensitive)
    );
}

TEST_F(TaskInterferenceCheckTest, defaultAllComponentsInvalidSheetShowsDiagnostics)
{
    auto* link = freecad_cast<App::Link*>(_doc->addObject("App::Link", "BadLinkedArr"));
    auto* source = _doc->addObject<Part::Box>("BadLinkedSrc");
    source->Length.setValue(10);
    source->Width.setValue(10);
    source->Height.setValue(10);
    link->setLink(-1, source);
    link->ShowElement.setValue(true);
    link->ElementCount.setValue(1);
    link->PlacementList.setValues({Base::Placement()});
    _assembly->addObject(link);
    auto* boxB = _doc->addObject<Part::Box>("BadSheetB");
    boxB->Length.setValue(10);
    boxB->Width.setValue(10);
    boxB->Height.setValue(10);
    boxB->Placement.setValue(Base::Placement(Base::Vector3d(10.4, 0, 0), Base::Rotation()));
    _assembly->addObject(boxB);

    auto* sheet = _doc->addObject<Spreadsheet::Sheet>("BadLinkedSheet");
    sheet->setCell("A1", "Enabled");
    sheet->setCell("B1", "Face");
    sheet->setCell("C1", "Tolerance");
    sheet->setCell("A2", "true");
    sheet->setCell(
        "B2",
        (std::string(link->getNameInDocument()) + ".notAnElement.Face1").c_str()
    );
    sheet->setCell("C2", "0.5");
    _doc->recompute();
    _assembly->setInterferenceClearanceSheet(sheet);
    Assembly::setInterferenceClearance(_assembly, 0.0);

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testRunScan();
    auto& session = task.scanSession();
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(60000);
    while ((session.isBusy() || task.isScanning()) && timeout.isActive()) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    for (int i = 0; i < 50 && !task.hasResults() && !task.isScanning(); ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    ASSERT_TRUE(task.hasResults());
    EXPECT_TRUE(
        task.testStatusText().contains(QStringLiteral("incomplete"), Qt::CaseInsensitive)
        || task.testStatusText().contains(QStringLiteral("invalid"), Qt::CaseInsensitive)
    );
    bool sawInvalidRule = false;
    for (int row = 0; row < task.testTableRowCount(); ++row) {
        if (task.testTableCellText(row, 0).contains(QStringLiteral("rule"), Qt::CaseInsensitive)
            || task.testTableCellText(row, 7).contains(QStringLiteral("Face"), Qt::CaseInsensitive)) {
            sawInvalidRule = true;
        }
    }
    EXPECT_TRUE(sawInvalidRule);
}

TEST_F(TaskInterferenceCheckTest, showClearFaceChecksRevealsClearStatusAndHidesByDefault)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    EXPECT_FALSE(task.testShowClearFaceChecks());

    Assembly::InterferenceScanResult result;
    Assembly::InterferenceLeaf leafA;
    leafA.displayPath = "A";
    leafA.occurrenceSubName = "A.";
    leafA.sourceId = "srcA";
    Assembly::InterferenceLeaf leafB;
    leafB.displayPath = "B";
    leafB.occurrenceSubName = "B.";
    leafB.sourceId = "srcB";
    result.leaves = {leafA, leafB};

    Assembly::InterferencePairResult pair;
    pair.leafIndexA = 0;
    pair.leafIndexB = 1;
    pair.detection.kind = Part::InterferenceKind::ClearanceViolation;
    Assembly::InterferenceFaceHit clearHit;
    clearHit.facePathA = "A.Face1";
    clearHit.facePathB = "B.Face1";
    clearHit.classification = Part::InterferenceKind::Clear;
    clearHit.appliedClearance = 0.1;
    clearHit.ruleKind = Assembly::InterferenceClearanceRuleKind::DefaultStar;
    clearHit.sourceRows = {2};
    clearHit.sourceComments = {"ok"};
    Assembly::InterferenceFaceHit violHit = clearHit;
    violHit.facePathA = "A.Face2";
    violHit.facePathB = "B.Face2";
    violHit.classification = Part::InterferenceKind::ClearanceViolation;
    violHit.sourceRows = {3, 4};
    violHit.sourceComments = {"c1", "c2"};
    Assembly::InterferenceFaceHit contactHit = clearHit;
    contactHit.facePathA = "A.Face3";
    contactHit.facePathB = "B.Face3";
    contactHit.classification = Part::InterferenceKind::Contact;
    pair.faceHits = {clearHit, violHit, contactHit};
    result.pairs.push_back(pair);

    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    ASSERT_EQ(task.testTableRowCount(), 1);
    EXPECT_FALSE(task.testTableCellText(0, 6).contains(QStringLiteral("A.Face1")));
    EXPECT_TRUE(task.testTableCellText(0, 6).contains(QStringLiteral("[Clearance]")));
    EXPECT_TRUE(task.testTableCellText(0, 6).contains(QStringLiteral("[Contact]")));
    EXPECT_TRUE(task.testTableCellText(0, 7).contains(QStringLiteral("row 3")));
    EXPECT_TRUE(task.testTableCellText(0, 7).contains(QStringLiteral("c1")));
    EXPECT_TRUE(task.testTableCellText(0, 7).contains(QStringLiteral("c2")));

    task.testSetShowClearFaceChecks(true);
    EXPECT_TRUE(task.testTableCellText(0, 6).contains(QStringLiteral("[Clear]")));
    EXPECT_TRUE(task.testTableCellText(0, 6).contains(QStringLiteral("A.Face1")));
}

TEST_F(TaskInterferenceCheckTest, issueKindsHaveDistinctLabels)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    Assembly::InterferenceScanResult result;
    Assembly::InterferenceComponentIssue leafIssue;
    leafIssue.kind = Assembly::InterferenceComponentIssue::Kind::InvalidLeaf;
    leafIssue.diagnostic = "bad leaf";
    Assembly::InterferenceComponentIssue ruleIssue;
    ruleIssue.kind = Assembly::InterferenceComponentIssue::Kind::InvalidRule;
    ruleIssue.diagnostic = "bad rule";
    Assembly::InterferenceComponentIssue capIssue;
    capIssue.kind = Assembly::InterferenceComponentIssue::Kind::FaceEnumerationCapped;
    capIssue.diagnostic = "capped";
    result.componentIssues = {leafIssue, ruleIssue, capIssue};

    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    ASSERT_EQ(task.testTableRowCount(), 3);
    EXPECT_TRUE(task.testTableCellText(0, 0).contains(QStringLiteral("geometry"), Qt::CaseInsensitive));
    EXPECT_TRUE(task.testTableCellText(1, 0).contains(QStringLiteral("rule"), Qt::CaseInsensitive));
    EXPECT_TRUE(task.testTableCellText(2, 0).contains(QStringLiteral("capped"), Qt::CaseInsensitive));
}

TEST_F(TaskInterferenceCheckTest, mixedExcludedPairKeepsInvalidVisibleByDefault)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    Assembly::InterferenceScanResult result;
    Assembly::InterferenceLeaf leafA;
    leafA.displayPath = "A";
    leafA.sourceId = "srcA";
    Assembly::InterferenceLeaf leafB;
    leafB.displayPath = "B";
    leafB.sourceId = "srcB";
    result.leaves = {leafA, leafB};

    Assembly::InterferencePairResult pair;
    pair.leafIndexA = 0;
    pair.leafIndexB = 1;
    pair.detection.kind = Part::InterferenceKind::Contact;
    pair.excluded = true;
    Assembly::InterferenceFaceHit contact;
    contact.facePathA = "A.Face1";
    contact.facePathB = "B.Face1";
    contact.classification = Part::InterferenceKind::Contact;
    contact.suppressedByExclusion = true;
    Assembly::InterferenceFaceHit invalid;
    invalid.facePathA = "A.Face2";
    invalid.facePathB = "B.Face2";
    invalid.classification = Part::InterferenceKind::InvalidInput;
    invalid.suppressedByExclusion = false;
    pair.faceHits = {contact, invalid};
    result.pairs.push_back(pair);
    result.counts.excludedViolations = 1;
    result.counts.invalidInputs = 1;
    result.complete = false;

    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    ASSERT_EQ(task.testTableRowCount(), 1);
    EXPECT_TRUE(task.testTableCellText(0, 0).contains(QStringLiteral("Invalid"), Qt::CaseInsensitive));
    EXPECT_TRUE(task.testTableCellText(0, 6).contains(QStringLiteral("[Invalid]")));
    EXPECT_FALSE(task.testTableCellText(0, 6).contains(QStringLiteral("A.Face1")));

    task.testSetShowExcluded(true);
    EXPECT_TRUE(task.testTableCellText(0, 6).contains(QStringLiteral("A.Face1")));
    EXPECT_TRUE(task.testTableCellText(0, 6).contains(QStringLiteral("Excluded"))
                || task.testTableCellText(0, 6).contains(QStringLiteral("[Contact]")));
    task.testSelectResultRow(0);
    EXPECT_TRUE(task.testIsRestorePairEnabled());
}

TEST_F(TaskInterferenceCheckTest, mixedExcludedPairKeepsInconclusiveVisibleByDefault)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    Assembly::InterferenceScanResult result;
    Assembly::InterferenceLeaf leafA;
    leafA.displayPath = "A";
    leafA.sourceId = "srcA";
    Assembly::InterferenceLeaf leafB;
    leafB.displayPath = "B";
    leafB.sourceId = "srcB";
    result.leaves = {leafA, leafB};

    Assembly::InterferencePairResult pair;
    pair.leafIndexA = 0;
    pair.leafIndexB = 1;
    pair.detection.kind = Part::InterferenceKind::ClearanceViolation;
    pair.excluded = true;
    Assembly::InterferenceFaceHit viol;
    viol.facePathA = "A.Face1";
    viol.facePathB = "B.Face1";
    viol.classification = Part::InterferenceKind::ClearanceViolation;
    viol.suppressedByExclusion = true;
    Assembly::InterferenceFaceHit incon;
    incon.facePathA = "A.Face2";
    incon.facePathB = "B.Face2";
    incon.classification = Part::InterferenceKind::Inconclusive;
    pair.faceHits = {viol, incon};
    result.pairs.push_back(pair);
    result.counts.excludedViolations = 1;
    result.counts.inconclusivePairs = 1;
    result.complete = false;

    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    ASSERT_EQ(task.testTableRowCount(), 1);
    EXPECT_TRUE(task.testTableCellText(0, 0).contains(QStringLiteral("Inconclusive"), Qt::CaseInsensitive));
    EXPECT_TRUE(task.testTableCellText(0, 6).contains(QStringLiteral("[Inconclusive]")));
    EXPECT_FALSE(task.testTableCellText(0, 6).contains(QStringLiteral("A.Face1")));

    task.testSetShowExcluded(true);
    EXPECT_TRUE(task.testTableCellText(0, 6).contains(QStringLiteral("A.Face1")));
}

TEST_F(TaskInterferenceCheckTest, excludedViolationsOnlyHiddenByDefault)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    Assembly::InterferenceScanResult result;
    Assembly::InterferenceLeaf leafA;
    leafA.displayPath = "A";
    leafA.sourceId = "srcA";
    Assembly::InterferenceLeaf leafB;
    leafB.displayPath = "B";
    leafB.sourceId = "srcB";
    result.leaves = {leafA, leafB};

    Assembly::InterferencePairResult pair;
    pair.leafIndexA = 0;
    pair.leafIndexB = 1;
    pair.detection.kind = Part::InterferenceKind::Contact;
    pair.excluded = true;
    Assembly::InterferenceFaceHit contact;
    contact.classification = Part::InterferenceKind::Contact;
    contact.facePathA = "A.Face1";
    contact.facePathB = "B.Face1";
    contact.suppressedByExclusion = true;
    pair.faceHits = {contact};
    result.pairs.push_back(pair);

    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    EXPECT_EQ(task.testTableRowCount(), 0);
    task.testSetShowExcluded(true);
    EXPECT_EQ(task.testTableRowCount(), 1);
    EXPECT_TRUE(task.testTableCellText(0, 0).contains(QStringLiteral("Excluded"), Qt::CaseInsensitive));
}

TEST_F(TaskInterferenceCheckTest, clearanceSheetSelectionSupportsUndoRedo)
{
    auto* sheet = _doc->addObject<Spreadsheet::Sheet>("UndoSheet");
    sheet->setCell("A1", "Enabled");
    sheet->setCell("B1", "Face");
    sheet->setCell("C1", "Tolerance");
    sheet->setCell("A2", "true");
    sheet->setCell("B2", "*");
    sheet->setCell("C2", "0.1");
    _doc->recompute();

    {
        AssemblyGui::TaskInterferenceCheck task(_assembly);
        task.testRefreshClearanceSheetUi();
        EXPECT_EQ(Assembly::getInterferenceClearanceSheet(_assembly), nullptr);

        auto& session = task.scanSession();
        const auto scan = session.beginScan();
        task.testDeliverScanFinished(scan.generation, makeResult(1, "BeforeSheet"));
        ASSERT_TRUE(task.hasResults());

        task.testSelectClearanceSheetByName(QStringLiteral("UndoSheet"));
        EXPECT_EQ(Assembly::getInterferenceClearanceSheet(_assembly), sheet);
        EXPECT_TRUE(session.isStale());
    }

    // Document transaction must support undo/redo after the dialog is gone.
    _doc->undo();
    EXPECT_EQ(Assembly::getInterferenceClearanceSheet(_assembly), nullptr);
    _doc->redo();
    EXPECT_EQ(Assembly::getInterferenceClearanceSheet(_assembly), sheet);
}

int main(int argc, char** argv)
{
    ensureDefaultOffscreenQtPlatform();
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    // Skip static FreeCAD/Qt destructors that SIGSEGV after offscreen QApplication.
    _Exit(result);
}
