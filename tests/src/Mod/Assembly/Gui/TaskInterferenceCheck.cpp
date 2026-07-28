// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <utility>

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
#include <App/PropertyLinks.h>
#include <QtGlobal>

#include <App/Document.h>
#include <App/Link.h>
#include <App/Property.h>
#include <Base/Writer.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
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
#include <Mod/Assembly/App/ReviewNote.h>
#include <Mod/Assembly/Gui/TaskInterferenceCheck.h>
#include <Mod/Part/App/FeaturePartBox.h>
#include <Mod/Part/App/InterferenceDetection.h>
#include <Mod/Part/Gui/SoBrepEdgeSet.h>
#include <Mod/Part/Gui/SoBrepFaceSet.h>
#include <Mod/Part/Gui/SoBrepPointSet.h>
#include <Mod/Part/Gui/SoFCShapeObject.h>
#include <Mod/Spreadsheet/App/Cell.h>
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

Assembly::InterferenceScanResult makeViolationResultForObjects(
    App::DocumentObject* sourceA,
    App::DocumentObject* sourceB
)
{
    auto result = makeResult(1, "Exclude");
    result.leaves[0].sourceId =
        std::string(sourceA->getDocument()->getName()) + "#" + sourceA->getNameInDocument();
    result.leaves[1].sourceId =
        std::string(sourceB->getDocument()->getName()) + "#" + sourceB->getNameInDocument();
    return result;
}

App::DocumentObject* makeLightweightReviewTarget(App::Document* doc, const char* name)
{
    // The fixture initializes Coin without a MainWindow. Suppress a view
    // provider for these synthetic anchors; only their App-side placement and
    // ownership are relevant to this task-panel test.
    return doc ? doc->addObject("Part::Feature", name, true, "None") : nullptr;
}

std::string exclusionPropertyXml(Assembly::AssemblyObject* assembly)
{
    auto* prop = assembly->getPropertyByName("InterferenceExcludedSources");
    if (!prop) {
        return {};
    }
    Base::StringWriter writer;
    prop->Save(writer);
    return writer.getString();
}

class ScopedExternalDocument
{
public:
    ScopedExternalDocument(const char* userName, const char* label)
    {
        _name = App::GetApplication().getUniqueDocumentName(userName);
        _doc = App::GetApplication().newDocument(_name.c_str(), label);
    }

    ~ScopedExternalDocument()
    {
        if (_doc && App::GetApplication().getDocument(_name.c_str())) {
            App::GetApplication().closeDocument(_name.c_str());
        }
    }

    App::Document* doc() const
    {
        return _doc;
    }

private:
    std::string _name;
    App::Document* _doc {};
};

class ScopedSavedOwnerPath
{
public:
    void assign(App::Document* doc, std::string path)
    {
        _path = std::move(path);
        doc->FileName.setValue(_path.c_str());
    }

    ~ScopedSavedOwnerPath()
    {
        if (!_path.empty()) {
            std::remove(_path.c_str());
        }
    }

private:
    std::string _path;
};

Gui::Document* ensureGuiDocumentForTest(App::Document* appDoc)
{
    static bool guiReady = false;
    if (!guiReady) {
        Gui::Application::initApplication();
        static Gui::Application* guiApp = new Gui::Application(false);
        (void)guiApp;
        guiReady = true;
    }
    if (!Gui::Application::Instance || !appDoc) {
        return nullptr;
    }
    if (Gui::Document* existing = Gui::Application::Instance->getDocument(appDoc)) {
        return existing;
    }
    return new Gui::Document(appDoc, Gui::Application::Instance);
}

Assembly::InterferenceScanResult makeClearPairOnlyResult(int clearPairs)
{
    Assembly::InterferenceScanResult result;
    result.complete = true;
    result.counts.clearPairs = clearPairs;
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

Assembly::InterferenceScanResult makeGoverningFacePreviewResult(
    const char* statusTag,
    const Base::Vector3d& pointOnFirst,
    const Base::Vector3d& pointOnSecond
)
{
    auto result = makeResult(0, statusTag);
    result.counts.penetrations = 0;
    result.counts.clearanceViolations = 1;
    result.leaves[0].worldShape = makePlacedBox(10, 10, 10, 0, 0, 0);
    result.leaves[1].worldShape = makePlacedBox(10, 10, 10, 40, 15, 7);

    auto& pair = result.pairs.front();
    pair.detection.kind = Part::InterferenceKind::ClearanceViolation;
    pair.detection.minimumDistance = 0.05;
    pair.detection.pointOnFirst = Base::Vector3d(1, 1, 1);
    pair.detection.pointOnSecond = Base::Vector3d(2, 2, 2);
    pair.detection.overlapVolume = 0.0;

    Assembly::InterferenceFaceHit globalClosest;
    globalClosest.facePathA = std::string(statusTag) + "_A.Face1";
    globalClosest.facePathB = std::string(statusTag) + "_B.Face1";
    globalClosest.distance = 0.05;
    globalClosest.appliedClearance = 0.0;
    globalClosest.classification = Part::InterferenceKind::Clear;
    globalClosest.closestPointsValid = true;
    globalClosest.pointOnFirst = Base::Vector3d(1, 1, 1);
    globalClosest.pointOnSecond = Base::Vector3d(2, 2, 2);

    Assembly::InterferenceFaceHit governed;
    governed.facePathA = std::string(statusTag) + "_A.Face5";
    governed.facePathB = std::string(statusTag) + "_B.Face3";
    governed.distance = 0.4;
    governed.appliedClearance = 0.5;
    governed.classification = Part::InterferenceKind::ClearanceViolation;
    governed.ruleKind = Assembly::InterferenceClearanceRuleKind::ExactPair;
    governed.sourceRows = {7};
    governed.sourceComments = {"governing exact rule"};
    governed.closestPointsValid = true;
    governed.pointOnFirst = pointOnFirst;
    governed.pointOnSecond = pointOnSecond;

    pair.faceHits = {std::move(globalClosest), std::move(governed)};
    pair.governingFaceHitIndex = 1;
    pair.detection.minimumDistance = pair.faceHits[1].distance;
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

TEST_F(TaskInterferenceCheckTest, obsoleteFaceResultDoesNotReplaceGoverningPreview)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testEnsureDetachedPreviewRoot();
    ASSERT_TRUE(task.testHasPreviewRoot());

    auto& session = task.scanSession();
    const auto scanA = session.beginScan();
    const auto scanB = session.beginScan();
    const Base::Vector3d governedFirst(110, 21, 6);
    const Base::Vector3d governedSecond(110.4, 21, 6);
    const auto resultB =
        makeGoverningFacePreviewResult("New", governedFirst, governedSecond);
    const auto resultA = makeGoverningFacePreviewResult(
        "Obsolete",
        Base::Vector3d(-50, -50, -50),
        Base::Vector3d(-40, -40, -40)
    );

    task.testDeliverScanFinished(scanB.generation, resultB);
    ASSERT_EQ(task.testTableRowCount(), 1);
    task.testSelectResultRow(0);
    Base::Vector3d beforeFirst;
    Base::Vector3d beforeSecond;
    ASSERT_TRUE(task.testPreviewMarkerPoints(beforeFirst, beforeSecond));
    EXPECT_NEAR(beforeFirst.x, governedFirst.x, 1e-4);
    EXPECT_NEAR(beforeSecond.x, governedSecond.x, 1e-4);

    task.testDeliverScanFinished(scanA.generation, resultA);
    EXPECT_EQ(task.testTableCellText(0, 1), QStringLiteral("New_A"));
    Base::Vector3d afterFirst;
    Base::Vector3d afterSecond;
    ASSERT_TRUE(task.testPreviewMarkerPoints(afterFirst, afterSecond));
    EXPECT_NEAR(afterFirst.x, beforeFirst.x, 1e-4);
    EXPECT_NEAR(afterFirst.y, beforeFirst.y, 1e-4);
    EXPECT_NEAR(afterSecond.x, beforeSecond.x, 1e-4);
    EXPECT_NEAR(afterSecond.y, beforeSecond.y, 1e-4);
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

TEST_F(TaskInterferenceCheckTest, tableAndPreviewUseTheSameGoverningFaceHit)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testEnsureDetachedPreviewRoot();

    const Base::Vector3d governedFirst(110, 21, 6);
    const Base::Vector3d governedSecond(110.4, 21, 6);
    auto result =
        makeGoverningFacePreviewResult("Governed", governedFirst, governedSecond);
    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, result);

    ASSERT_EQ(task.testTableRowCount(), 1);
    EXPECT_TRUE(task.testTableCellText(0, 0).contains(QStringLiteral("Clearance")));
    EXPECT_TRUE(task.testTableCellText(0, 3).contains(QStringLiteral("0.4")));
    EXPECT_TRUE(task.testTableCellText(0, 6).contains(QStringLiteral("Governed_A.Face5")));
    EXPECT_TRUE(task.testTableCellText(0, 6).contains(QStringLiteral("Governed_B.Face3")));
    EXPECT_FALSE(task.testTableCellText(0, 6).contains(QStringLiteral("Face1")));
    EXPECT_TRUE(task.testTableCellText(0, 7).contains(QStringLiteral("row 7")));
    EXPECT_TRUE(
        task.testTableCellText(0, 7).contains(QStringLiteral("governing exact rule"))
    );

    task.testSelectResultRow(0);
    Base::Vector3d previewFirst;
    Base::Vector3d previewSecond;
    ASSERT_TRUE(task.testPreviewMarkerPoints(previewFirst, previewSecond));
    EXPECT_NEAR(previewFirst.x, governedFirst.x, 1e-4);
    EXPECT_NEAR(previewFirst.y, governedFirst.y, 1e-4);
    EXPECT_NEAR(previewFirst.z, governedFirst.z, 1e-4);
    EXPECT_NEAR(previewSecond.x, governedSecond.x, 1e-4);
    EXPECT_NEAR(previewSecond.y, governedSecond.y, 1e-4);
    EXPECT_NEAR(previewSecond.z, governedSecond.z, 1e-4);
}

TEST_F(TaskInterferenceCheckTest, governingContactCommonShapeDrivesPreview)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testEnsureDetachedPreviewRoot();
    auto result = makeGoverningFacePreviewResult(
        "ContactGoverned",
        Base::Vector3d(),
        Base::Vector3d()
    );
    auto& pair = result.pairs.front();
    pair.detection.kind = Part::InterferenceKind::Contact;
    pair.detection.commonShape = makePlacedBox(1, 1, 1, 90, 0, 0);
    auto& hit = pair.faceHits[pair.governingFaceHitIndex];
    hit.classification = Part::InterferenceKind::Contact;
    hit.closestPointsValid = false;
    hit.commonShape = makePlacedBox(1, 1, 1, 25, 12, 3);

    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    task.testSelectResultRow(0);

    ASSERT_EQ(task.testPreviewShapeCount(), 3);
    double x = 0, y = 0, z = 0;
    ASSERT_TRUE(task.testPreviewShapeTranslation(0, x, y, z));
    EXPECT_NEAR(x, 25.0, 1e-4);
    EXPECT_NEAR(y, 12.0, 1e-4);
    EXPECT_NEAR(z, 3.0, 1e-4);
    Base::Vector3d unusedFirst;
    Base::Vector3d unusedSecond;
    EXPECT_FALSE(task.testPreviewMarkerPoints(unusedFirst, unusedSecond));
}

TEST_F(TaskInterferenceCheckTest, inconclusiveFaceWithoutGeometryCreatesNoOriginMarker)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testEnsureDetachedPreviewRoot();
    auto result = makeGoverningFacePreviewResult(
        "NoGeometry",
        Base::Vector3d(),
        Base::Vector3d()
    );
    auto& pair = result.pairs.front();
    pair.detection.kind = Part::InterferenceKind::Inconclusive;
    pair.detection.minimumDistance = 0.0;
    pair.detection.pointOnFirst = Base::Vector3d();
    pair.detection.pointOnSecond = Base::Vector3d();
    auto& hit = pair.faceHits[pair.governingFaceHitIndex];
    hit.classification = Part::InterferenceKind::Inconclusive;
    hit.distance = -1.0;
    hit.diagnostic = "governing face geometry unavailable";
    hit.closestPointsValid = false;
    hit.commonShape.Nullify();

    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    ASSERT_EQ(task.testTableRowCount(), 1);
    EXPECT_TRUE(
        task.testTableCellText(0, 0).contains(QStringLiteral("Inconclusive"))
    );
    EXPECT_TRUE(
        task.testTableCellText(0, 7).contains(
            QStringLiteral("governing face geometry unavailable")
        )
    );
    task.testSelectResultRow(0);
    EXPECT_EQ(task.testPreviewShapeCount(), 2);
    Base::Vector3d unusedFirst;
    Base::Vector3d unusedSecond;
    EXPECT_FALSE(task.testPreviewMarkerPoints(unusedFirst, unusedSecond));
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

TEST_F(TaskInterferenceCheckTest, invalidClearanceSheetShowsRequiredHeaders)
{
    auto* sheet = _doc->addObject<Spreadsheet::Sheet>("Parameters");
    ASSERT_NE(sheet, nullptr);
    sheet->setCell("A1", "Parameter");
    sheet->setCell("B1", "Value");
    sheet->setCell("C1", "Unit");
    sheet->setCell("D1", "Notes");
    _assembly->setInterferenceClearanceSheet(sheet);

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    const QString status = task.testClearanceSheetLabel();
    EXPECT_TRUE(status.contains(QStringLiteral("Invalid"), Qt::CaseInsensitive));
    EXPECT_TRUE(status.contains(QStringLiteral("Face"), Qt::CaseInsensitive));
    EXPECT_TRUE(status.contains(QStringLiteral("Tolerance"), Qt::CaseInsensitive));

    sheet->setCell("A1", "Enabled");
    sheet->setCell("B1", "Face");
    sheet->setCell("C1", "FaceB");
    sheet->setCell("D1", "Tolerance");
    sheet->setCell("E1", "Comment");
    EXPECT_TRUE(
        task.testClearanceSheetLabel().contains(QStringLiteral("ready"), Qt::CaseInsensitive)
    );

    sheet->setCell("A2", "true");
    sheet->setCell("B2", "MissingOccurrence.Face1");
    sheet->setCell("D2", "0.1");
    task.testRefreshClearanceSheetUi();
    EXPECT_TRUE(
        task.testClearanceSheetLabel().contains(QStringLiteral("Invalid"), Qt::CaseInsensitive)
    );
    EXPECT_TRUE(
        task.testClearanceSheetLabel().contains(
            QStringLiteral("MissingOccurrence"),
            Qt::CaseInsensitive
        )
    );
}

TEST_F(TaskInterferenceCheckTest, openingPanelDoesNotCreatePropertiesOrAutoLinkSoleSheet)
{
    auto* plainRoot = _doc->addObject<App::Part>("PlainAssemblyRoot");
    ASSERT_NE(plainRoot, nullptr);
    auto* sheet = _doc->addObject<Spreadsheet::Sheet>("OnlySpreadsheet");
    ASSERT_NE(sheet, nullptr);
    sheet->setCell("A1", "Parameter");
    sheet->setCell("B1", "Value");
    _doc->recompute();
    const bool touchedBefore = _doc->isTouched();

    ASSERT_EQ(plainRoot->getPropertyByName("InterferenceClearance"), nullptr);
    ASSERT_EQ(plainRoot->getPropertyByName("InterferenceClearanceSheet"), nullptr);
    ASSERT_EQ(plainRoot->getPropertyByName("InterferenceExcludedSources"), nullptr);

    AssemblyGui::TaskInterferenceCheck task(plainRoot);

    EXPECT_EQ(plainRoot->getPropertyByName("InterferenceClearance"), nullptr);
    EXPECT_EQ(plainRoot->getPropertyByName("InterferenceClearanceSheet"), nullptr);
    EXPECT_EQ(plainRoot->getPropertyByName("InterferenceExcludedSources"), nullptr);
    EXPECT_EQ(Assembly::getInterferenceClearanceSheet(plainRoot), nullptr);
    EXPECT_EQ(_doc->isTouched(), touchedBefore);
    EXPECT_TRUE(
        task.testClearanceSheetLabel().contains(
            QStringLiteral("No clearance sheet"),
            Qt::CaseInsensitive
        )
    );
}

TEST_F(TaskInterferenceCheckTest, createClearanceSheetAddsHeadersAndLinksIt)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    QString error;
    ASSERT_TRUE(task.testCreateClearanceSheet(&error)) << error.toStdString();
    EXPECT_TRUE(error.isEmpty());

    auto* sheet = freecad_cast<Spreadsheet::Sheet*>(
        Assembly::getInterferenceClearanceSheet(_assembly)
    );
    ASSERT_NE(sheet, nullptr);
    EXPECT_EQ(std::string(sheet->getNameInDocument()).rfind("InterferenceClearance", 0), 0);

    const std::vector<std::pair<const char*, const char*>> expected {
        {"A1", "Enabled"},
        {"B1", "Face"},
        {"C1", "FaceB"},
        {"D1", "Tolerance"},
        {"E1", "Comment"},
    };
    for (const auto& [address, expectedContent] : expected) {
        const auto* cell = sheet->getCell(App::CellAddress(address));
        ASSERT_NE(cell, nullptr) << address;
        std::string content;
        ASSERT_TRUE(cell->getStringContent(content)) << address;
        EXPECT_EQ(content, std::string("'") + expectedContent) << address;
    }
    EXPECT_EQ(sheet->getCell(App::CellAddress("A2")), nullptr);

    const auto table = Assembly::parseInterferenceClearanceSheet(sheet, _assembly);
    EXPECT_EQ(table.invalidRuleCount, 0);
    EXPECT_TRUE(table.rules.empty());
    EXPECT_TRUE(
        task.testClearanceSheetLabel().contains(QStringLiteral("ready"), Qt::CaseInsensitive)
    );

    const std::string sheetName = sheet->getNameInDocument();
    _doc->undo();
    EXPECT_EQ(Assembly::getInterferenceClearanceSheet(_assembly), nullptr);
    EXPECT_EQ(_doc->getObject(sheetName.c_str()), nullptr);
}

TEST_F(TaskInterferenceCheckTest, completeZeroRowClearPairResultIsRetainedAndSummarized)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, makeClearPairOnlyResult(4));

    EXPECT_TRUE(task.hasResults());
    EXPECT_EQ(task.testTableRowCount(), 0);
    EXPECT_EQ(task.testResultPairCount(), 0u);
    EXPECT_TRUE(task.testSummaryText().contains(QStringLiteral("Clear pairs: 4")));
    EXPECT_TRUE(task.testStatusText().contains(QStringLiteral("complete"), Qt::CaseInsensitive));
}

TEST_F(TaskInterferenceCheckTest, stricterClearanceSheetClearsZeroRowAcceptedResult)
{
    auto* lenientSheet = _doc->addObject<Spreadsheet::Sheet>("LenientClearances");
    lenientSheet->setCell("A1", "Enabled");
    lenientSheet->setCell("B1", "Face");
    lenientSheet->setCell("C1", "Tolerance");
    lenientSheet->setCell("A2", "true");
    lenientSheet->setCell("B2", "*");
    lenientSheet->setCell("C2", "1");
    auto* strictSheet = _doc->addObject<Spreadsheet::Sheet>("StrictClearances");
    strictSheet->setCell("A1", "Enabled");
    strictSheet->setCell("B1", "Face");
    strictSheet->setCell("C1", "Tolerance");
    strictSheet->setCell("A2", "true");
    strictSheet->setCell("B2", "*");
    strictSheet->setCell("C2", "10");
    _doc->recompute();
    _assembly->setInterferenceClearanceSheet(lenientSheet);

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, makeClearPairOnlyResult(2));
    ASSERT_TRUE(task.hasResults());
    EXPECT_EQ(task.testTableRowCount(), 0);

    task.testSelectClearanceSheetByName(QStringLiteral("StrictClearances"));
    _doc->recompute();

    EXPECT_EQ(Assembly::getInterferenceClearanceSheet(_assembly), strictSheet);
    EXPECT_TRUE(session.isStale());
    EXPECT_FALSE(task.hasResults());
    EXPECT_EQ(task.testTableRowCount(), 0);
    EXPECT_TRUE(task.testStatusText().contains(QStringLiteral("stale"), Qt::CaseInsensitive));
    EXPECT_TRUE(task.testSummaryText().contains(QStringLiteral("No results"), Qt::CaseInsensitive));
}

TEST_F(TaskInterferenceCheckTest, cancelledOrObsoleteResultsCannotCreatePhantomAcceptedState)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    auto& session = task.scanSession();

    const auto cancelledScan = session.beginScan();
    auto cancelledResult = makeClearPairOnlyResult(6);
    cancelledResult.cancelled = true;
    task.testDeliverScanFinished(cancelledScan.generation, cancelledResult);
    EXPECT_FALSE(task.hasResults());
    EXPECT_EQ(task.testTableRowCount(), 0);
    EXPECT_TRUE(task.testSummaryText().contains(QStringLiteral("No results"), Qt::CaseInsensitive));

    const auto scanA = session.beginScan();
    const auto scanB = session.beginScan();
    ASSERT_EQ(session.activeGeneration(), scanB.generation);

    Assembly::InterferenceScanResult emptyComplete;
    emptyComplete.complete = true;
    task.testDeliverScanFinished(scanB.generation, emptyComplete);
    ASSERT_TRUE(task.hasResults());
    EXPECT_EQ(task.testTableRowCount(), 0);
    EXPECT_TRUE(task.testSummaryText().contains(QStringLiteral("Clear pairs: 0")));
    const QString summaryAfterCurrent = task.testSummaryText();

    task.testDeliverScanFinished(scanA.generation, makeClearPairOnlyResult(9));
    EXPECT_TRUE(task.hasResults());
    EXPECT_EQ(task.testTableRowCount(), 0);
    EXPECT_EQ(task.testSummaryText(), summaryAfterCurrent);
    EXPECT_FALSE(task.testSummaryText().contains(QStringLiteral("Clear pairs: 9")));
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
    pair.governingFaceHitIndex = 1;
    result.pairs.push_back(pair);

    Assembly::InterferencePairResult clearPair;
    clearPair.leafIndexA = 0;
    clearPair.leafIndexB = 1;
    clearPair.detection.kind = Part::InterferenceKind::Clear;
    clearPair.detection.minimumDistance = 0.2;
    clearPair.faceHits = {clearHit};
    clearPair.governingFaceHitIndex = 0;
    result.pairs.push_back(clearPair);

    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    ASSERT_EQ(task.testTableRowCount(), 1);
    EXPECT_FALSE(task.testTableCellText(0, 6).contains(QStringLiteral("A.Face1")));
    EXPECT_TRUE(task.testTableCellText(0, 6).contains(QStringLiteral("[Clearance]")));
    EXPECT_TRUE(task.testTableCellText(0, 7).contains(QStringLiteral("row 3")));
    EXPECT_TRUE(task.testTableCellText(0, 7).contains(QStringLiteral("c1")));
    EXPECT_TRUE(task.testTableCellText(0, 7).contains(QStringLiteral("c2")));

    task.testSetShowClearFaceChecks(true);
    ASSERT_EQ(task.testTableRowCount(), 2);
    EXPECT_TRUE(task.testTableCellText(1, 6).contains(QStringLiteral("[Clear]")));
    EXPECT_TRUE(task.testTableCellText(1, 6).contains(QStringLiteral("A.Face1")));
}

TEST_F(TaskInterferenceCheckTest, faceOnlyViolationEnablesExcludeAndCountsPairOnce)
{
    auto* sourceA = _doc->addObject<Part::Box>("FaceOnlySourceA");
    auto* sourceB = _doc->addObject<Part::Box>("FaceOnlySourceB");
    const std::string idA =
        std::string(_doc->getName()) + "#" + sourceA->getNameInDocument();
    const std::string idB =
        std::string(_doc->getName()) + "#" + sourceB->getNameInDocument();

    Assembly::InterferenceScanResult result;
    result.complete = true;
    Assembly::InterferenceLeaf leafA;
    leafA.displayPath = "FaceOnlyA";
    leafA.sourceId = idA;
    Assembly::InterferenceLeaf leafB;
    leafB.displayPath = "FaceOnlyB";
    leafB.sourceId = idB;
    result.leaves = {leafA, leafB};

    Assembly::InterferencePairResult pair;
    pair.leafIndexA = 0;
    pair.leafIndexB = 1;
    pair.detection.kind = Part::InterferenceKind::Clear;
    Assembly::InterferenceFaceHit clearance;
    clearance.facePathA = "FaceOnlyA.Face2";
    clearance.facePathB = "FaceOnlyB.Face4";
    clearance.classification = Part::InterferenceKind::ClearanceViolation;
    auto contact = clearance;
    contact.facePathA = "FaceOnlyA.Face3";
    contact.facePathB = "FaceOnlyB.Face5";
    contact.classification = Part::InterferenceKind::Contact;
    pair.faceHits = {clearance, contact};
    pair.governingFaceHitIndex = 0;
    result.pairs.push_back(pair);

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    ASSERT_EQ(task.testTableRowCount(), 1);
    task.testSelectResultRow(0);

    EXPECT_TRUE(task.isExcludePairEnabled());
    EXPECT_EQ(task.testAffectedViolationPairCount(), 1u);
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
    pair.governingFaceHitIndex = 0;
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
    pair.governingFaceHitIndex = 0;
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
    pair.governingFaceHitIndex = 0;
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

TEST_F(TaskInterferenceCheckTest, excludePairCommandHelperCommitsAndClearsPendingTransaction)
{
    Gui::Document* guiDoc = ensureGuiDocumentForTest(_doc);
    ASSERT_NE(guiDoc, nullptr);

    auto* sourceA = _doc->addObject<App::DocumentObject>("App::Feature", "CmdSrcA");
    auto* sourceB = _doc->addObject<App::DocumentObject>("App::Feature", "CmdSrcZ");
    ASSERT_NE(sourceA, nullptr);
    ASSERT_NE(sourceB, nullptr);

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    QString error;
    EXPECT_TRUE(task.testExecuteExcludePairCommand(sourceA, sourceB, guiDoc, &error));
    EXPECT_TRUE(error.isEmpty());
    EXPECT_FALSE(guiDoc->hasPendingCommand());
    EXPECT_TRUE(_assembly->hasInterferenceExclusion(sourceA, sourceB));

    _doc->undo();
    EXPECT_FALSE(_assembly->hasInterferenceExclusion(sourceA, sourceB));
    _doc->redo();
    EXPECT_TRUE(_assembly->hasInterferenceExclusion(sourceA, sourceB));
}

TEST_F(TaskInterferenceCheckTest, resultCreatesStableReviewNoteAndLinksExclusionReason)
{
    auto* sourceA = makeLightweightReviewTarget(_doc, "ReviewResultA");
    auto* sourceB = makeLightweightReviewTarget(_doc, "ReviewResultB");
    ASSERT_NE(sourceA, nullptr);
    ASSERT_NE(sourceB, nullptr);
    _assembly->addObject(sourceA);
    _assembly->addObject(sourceB);
    ASSERT_TRUE(_assembly->hasObject(sourceA, true));
    ASSERT_TRUE(_assembly->hasObject(sourceB, true));

    auto result = makeViolationResultForObjects(sourceA, sourceB);
    result.leaves[0].occurrenceSubName = "ReviewResultA.";
    result.leaves[0].displayPath = "ReviewResultA";
    result.leaves[0].worldShape = makePlacedBox(10, 10, 10, 0, 0, 0);
    result.leaves[0].worldBoundBox = Base::BoundBox3d(0, 0, 0, 10, 10, 10);
    result.leaves[1].occurrenceSubName = "ReviewResultB.";
    result.leaves[1].displayPath = "ReviewResultB";
    result.leaves[1].worldShape = makePlacedBox(10, 10, 10, 5, 0, 0);
    result.leaves[1].worldBoundBox = Base::BoundBox3d(5, 0, 0, 15, 10, 10);

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    const auto scan = task.scanSession().beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    task.testSelectResultRow(0);
    EXPECT_TRUE(task.isCreateReviewNoteEnabled());

    QString error;
    ASSERT_TRUE(task.testCreateReviewNoteForSelectedRow(&error)) << error.toStdString();
    EXPECT_TRUE(error.isEmpty());
    EXPECT_TRUE(task.hasResults());
    EXPECT_EQ(task.testTableRowCount(), 1);
    EXPECT_TRUE(task.isExcludePairEnabled());
    EXPECT_TRUE(task.isCreateReviewNoteEnabled());
    const auto notes = _doc->getObjectsOfType(Assembly::ReviewNote::getClassTypeId());
    ASSERT_EQ(notes.size(), 1u);
    auto* note = freecad_cast<Assembly::ReviewNote*>(notes.front());
    ASSERT_NE(note, nullptr);
    EXPECT_TRUE(note->Target.getSubValues().empty());
    EXPECT_EQ(note->InterferenceSourceA.getValue(), result.leaves[0].sourceId);
    EXPECT_EQ(note->InterferenceSourceB.getValue(), result.leaves[1].sourceId);

    // Simulate Save As/reopen changing the document prefix in both metadata
    // identities. Selected-row exclusion must still resolve the current objects.
    note->InterferenceSourceA.setValue("oldDocument#ReviewResultA");
    note->InterferenceSourceB.setValue("oldDocument#ReviewResultB");
    Gui::Document* guiDoc = ensureGuiDocumentForTest(_doc);
    ASSERT_NE(guiDoc, nullptr);
    ASSERT_TRUE(task.testExecuteExcludePairForSelectedRow(guiDoc, &error))
        << error.toStdString();
    auto rules = Assembly::getInterferenceExclusionRules(_assembly);
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules.front().reason, note);
    EXPECT_EQ(
        rules.front().reasonIdentity,
        std::string(_doc->getName()) + "#" + note->getNameInDocument()
    );

    // Stored reason provenance may also carry the pre-reopen document prefix.
    auto* reasons = dynamic_cast<App::PropertyStringList*>(
        _assembly->getPropertyByName("InterferenceExclusionReasons")
    );
    ASSERT_NE(reasons, nullptr);
    reasons->setValues(
        std::vector<std::string> {"oldDocument#" + std::string(note->getNameInDocument())}
    );
    rules = Assembly::getInterferenceExclusionRules(_assembly);
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules.front().reason, note);
}

TEST_F(TaskInterferenceCheckTest, compactLinkArrayResultExplainsUnsupportedReviewNoteAnchor)
{
    auto result = makeResult(1, "Array");
    result.leaves[0].occurrenceSubName = "ArrayLink.0.";
    result.leaves[0].displayPath = "ArrayLink.0";
    result.leaves[0].worldBoundBox = Base::BoundBox3d(0, 0, 0, 10, 10, 10);
    result.leaves[1].occurrenceSubName = "OtherArray.1.";
    result.leaves[1].displayPath = "OtherArray.1";
    result.leaves[1].worldBoundBox = Base::BoundBox3d(5, 0, 0, 15, 10, 10);

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    const auto scan = task.scanSession().beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    task.testSelectResultRow(0);

    QString error;
    EXPECT_FALSE(task.testCreateReviewNoteForSelectedRow(&error));
    EXPECT_TRUE(error.contains(QStringLiteral("compact Link array"), Qt::CaseInsensitive))
        << error.toStdString();
    EXPECT_TRUE(_doc->getObjectsOfType(Assembly::ReviewNote::getClassTypeId()).empty());
}

TEST_F(TaskInterferenceCheckTest, compactLinkArrayResultFallsBackToDurableSecondAnchor)
{
    auto* sourceA = makeLightweightReviewTarget(_doc, "ArraySource");
    auto* sourceB = makeLightweightReviewTarget(_doc, "DurableAnchor");
    ASSERT_NE(sourceA, nullptr);
    ASSERT_NE(sourceB, nullptr);
    _assembly->addObject(sourceA);
    _assembly->addObject(sourceB);
    ASSERT_TRUE(_assembly->hasObject(sourceA, true));
    ASSERT_TRUE(_assembly->hasObject(sourceB, true));

    auto result = makeViolationResultForObjects(sourceA, sourceB);
    result.leaves[0].occurrenceSubName = "ArrayLink.0.";
    result.leaves[0].displayPath = "ArrayLink.0";
    result.leaves[0].worldBoundBox = Base::BoundBox3d(0, 0, 0, 10, 10, 10);
    result.leaves[1].occurrenceSubName = "DurableAnchor.";
    result.leaves[1].displayPath = "DurableAnchor";
    result.leaves[1].worldBoundBox = Base::BoundBox3d(5, 0, 0, 15, 10, 10);

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    const auto scan = task.scanSession().beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    task.testSelectResultRow(0);

    QString error;
    ASSERT_TRUE(task.testCreateReviewNoteForSelectedRow(&error)) << error.toStdString();
    EXPECT_TRUE(error.isEmpty());
    EXPECT_TRUE(task.hasResults());

    const auto notes = _doc->getObjectsOfType(Assembly::ReviewNote::getClassTypeId());
    ASSERT_EQ(notes.size(), 1u);
    auto* note = freecad_cast<Assembly::ReviewNote*>(notes.front());
    ASSERT_NE(note, nullptr);
    EXPECT_EQ(note->Target.getValue(), sourceB);
    EXPECT_TRUE(note->Target.getSubValues().empty());
    EXPECT_EQ(note->AnchorSourceIdentity.getValue(), result.leaves[1].sourceId);
}

TEST_F(TaskInterferenceCheckTest, excludePairCommandHelperAbortsOnUnsavedExternalInsertion)
{
    Gui::Document* guiDoc = ensureGuiDocumentForTest(_doc);
    ASSERT_NE(guiDoc, nullptr);

    auto* seedA = _doc->addObject<App::DocumentObject>("App::Feature", "GuiSeedA");
    auto* seedB = _doc->addObject<App::DocumentObject>("App::Feature", "GuiSeedZ");
    _assembly->addInterferenceExclusion(seedA, seedB);
    const std::string xmlBefore = exclusionPropertyXml(_assembly);

    ScopedExternalDocument other("zzz_asmGuiExclOther", "zzz_asmGuiExclOtherUser");
    auto* local = _doc->addObject<App::DocumentObject>("App::Feature", "GuiLocalA");
    auto* remote = other.doc()->addObject<App::DocumentObject>("App::Feature", "GuiRemoteZ");
    ASSERT_NE(local, nullptr);
    ASSERT_NE(remote, nullptr);
    const std::string idLocal =
        std::string(local->getDocument()->getName()) + "#" + local->getNameInDocument();
    const std::string idRemote =
        std::string(remote->getDocument()->getName()) + "#" + remote->getNameInDocument();
    ASSERT_LT(idLocal, idRemote);

    ScopedSavedOwnerPath ownerPath;
    ownerPath.assign(_doc, std::string("/tmp/") + _docName + "_guiExclFail.FCStd");
    ASSERT_TRUE(_doc->save());

    AssemblyGui::TaskInterferenceCheck task(_assembly);
    auto result = makeViolationResultForObjects(local, remote);
    auto& session = task.scanSession();
    const auto scan = session.beginScan();
    task.testDeliverScanFinished(scan.generation, result);
    task.testSelectResultRow(0);

    const auto undoBefore = _doc->getAvailableUndoNames();
    QString error;
    EXPECT_FALSE(task.testExecuteExcludePairForSelectedRow(guiDoc, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_NE(error.indexOf(QStringLiteral("Linked document not saved")), -1) << error.toStdString();
    EXPECT_FALSE(guiDoc->hasPendingCommand());
    EXPECT_EQ(_doc->getAvailableUndoNames().size(), undoBefore.size());
    EXPECT_EQ(exclusionPropertyXml(_assembly), xmlBefore);

    auto* prop = dynamic_cast<App::PropertyXLinkSubList*>(
        _assembly->getPropertyByName("InterferenceExcludedSources")
    );
    ASSERT_NE(prop, nullptr);
    EXPECT_EQ(prop->getSize(), 2);

    auto* okA = _doc->addObject<App::DocumentObject>("App::Feature", "GuiLaterOkA");
    auto* okB = _doc->addObject<App::DocumentObject>("App::Feature", "GuiLaterOkB");
    EXPECT_TRUE(task.testExecuteExcludePairCommand(okA, okB, guiDoc, &error));
    EXPECT_TRUE(_assembly->hasInterferenceExclusion(okA, okB));
}

static void waitUntilScanIdle(AssemblyGui::TaskInterferenceCheck& task, bool allowNoResults = false)
{
    auto& session = task.scanSession();
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(60000);
    while ((session.isBusy() || task.isScanning()) && timeout.isActive()) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    if (!allowNoResults) {
        for (int i = 0; i < 50 && !task.hasResults() && task.isScanning(); ++i) {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        }
    }
    else {
        for (int i = 0; i < 50 && task.isScanning(); ++i) {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        }
    }
    ASSERT_TRUE(timeout.isActive()) << "Timed out waiting for scan";
}

static bool waitWithEvents(const std::function<bool()>& predicate, int timeoutMs = 60000)
{
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(timeoutMs);
    while (!predicate() && timeout.isActive()) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return predicate();
}

static bool waitUntilWorkerStarted(const AssemblyGui::InterferenceWorkerInjectionControl& control)
{
    return waitWithEvents([&]() {
        return control.workerStarted.load(std::memory_order_acquire);
    });
}

static bool waitUntilInjectWatcherDelivered(
    const AssemblyGui::InterferenceWorkerInjectionControl& control
)
{
    return waitWithEvents([&]() {
        return control.injectWatcherDelivered.load(std::memory_order_acquire);
    });
}

static void addOverlappingPairForAsyncScan(App::Document* doc, Assembly::AssemblyObject* assembly)
{
    auto* boxA = doc->addObject<Part::Box>("WorkerFailA");
    boxA->Length.setValue(10);
    boxA->Width.setValue(10);
    boxA->Height.setValue(10);
    boxA->Placement.setValue(Base::Placement(Base::Vector3d(0, 0, 0), Base::Rotation()));
    auto* boxB = doc->addObject<Part::Box>("WorkerFailB");
    boxB->Length.setValue(10);
    boxB->Width.setValue(10);
    boxB->Height.setValue(10);
    boxB->Placement.setValue(Base::Placement(Base::Vector3d(5, 0, 0), Base::Rotation()));
    assembly->addObject(boxA);
    assembly->addObject(boxB);
    doc->recompute();
}

/** Ensures a held inject worker is released if the test fails or the fixture tears down. */
struct ScopedWorkerInjectionHoldRelease
{
    std::shared_ptr<AssemblyGui::InterferenceWorkerInjectionControl> control;

    explicit ScopedWorkerInjectionHoldRelease(
        std::shared_ptr<AssemblyGui::InterferenceWorkerInjectionControl> injectionControl
    )
        : control(std::move(injectionControl))
    {}

    ~ScopedWorkerInjectionHoldRelease()
    {
        if (control) {
            control->holdInWorker.store(false, std::memory_order_release);
        }
    }
};

struct ScopedGuiThreadScanFailureInjection
{
    AssemblyGui::TaskInterferenceCheck& task;

    explicit ScopedGuiThreadScanFailureInjection(AssemblyGui::TaskInterferenceCheck& taskIn)
        : task(taskIn)
    {}

    ~ScopedGuiThreadScanFailureInjection()
    {
        task.testClearGuiThreadScanFailureInjection();
    }
};

static void assertGuiThreadSyncFailureRecoveryState(
    AssemblyGui::TaskInterferenceCheck& task,
    Assembly::InterferenceScanSession& session,
    const QString& diagnosticSubstring,
    const QString& previousPenetrationMarker = QStringLiteral("P_A")
)
{
    EXPECT_FALSE(task.isScanning());
    EXPECT_FALSE(session.isBusy());
    EXPECT_FALSE(task.testIsPreparing());
    EXPECT_TRUE(task.testIsRunEnabled());
    EXPECT_FALSE(task.testIsCancelEnabled());
    EXPECT_EQ(task.testOwnedScanWatcherCount(), 0);
    EXPECT_TRUE(task.hasResults());
    EXPECT_TRUE(
        task.testStatusText().contains(QStringLiteral("incomplete"), Qt::CaseInsensitive)
        || task.testStatusText().contains(QStringLiteral("failed"), Qt::CaseInsensitive)
    );
    EXPECT_TRUE(task.testProgressText().isEmpty());
    EXPECT_EQ(task.testPreviewShapeCount(), 0);
    EXPECT_EQ(task.testPenetrationCount(), 0);
    EXPECT_EQ(task.testResultPairCount(), 0u);
    ASSERT_EQ(task.testTableRowCount(), 1);
    EXPECT_TRUE(
        task.testTableCellText(0, 0).contains(QStringLiteral("Diagnostic"), Qt::CaseInsensitive)
    );
    if (!previousPenetrationMarker.isEmpty()) {
        EXPECT_FALSE(task.testTableCellText(0, 1).contains(previousPenetrationMarker));
    }
    EXPECT_TRUE(task.testTableCellText(0, 7).contains(diagnosticSubstring));
}

static std::unique_ptr<AssemblyGui::TaskInterferenceCheck> makeOverlappingSelectedPairTask(
    App::Document* doc,
    Assembly::AssemblyObject* assembly
)
{
    addOverlappingPairForAsyncScan(doc, assembly);
    Assembly::InterferenceComponentOccurrence a;
    a.component = assembly;
    a.occurrencePrefix = "WorkerFailA.";
    a.displayPath = "WorkerFailA";
    Assembly::InterferenceComponentOccurrence b;
    b.component = assembly;
    b.occurrencePrefix = "WorkerFailB.";
    b.displayPath = "WorkerFailB";
    return std::make_unique<AssemblyGui::TaskInterferenceCheck>(assembly, a, b);
}

TEST_F(TaskInterferenceCheckTest, allComponentsPreparationFailureRecoversSynchronously)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testEnsureDetachedPreviewRoot();
    ASSERT_TRUE(task.testHasPreviewRoot());

    auto& session = task.scanSession();
    const auto seed = session.beginScan();
    task.testDeliverScanFinished(seed.generation, makePlacedPenetrationResult());
    ASSERT_TRUE(task.hasResults());
    ASSERT_EQ(task.testTableRowCount(), 1);
    EXPECT_TRUE(task.testTableCellText(0, 1).contains(QStringLiteral("P_A")));
    task.testSelectResultRow(0);
    ASSERT_EQ(task.testPreviewShapeCount(), 2);

    addOverlappingPairForAsyncScan(_doc, _assembly);
    ScopedGuiThreadScanFailureInjection clearInjection(task);
    AssemblyGui::GuiThreadScanFailureInjection injection;
    injection.stage = AssemblyGui::GuiThreadScanFailureStage::AllComponentsPreparation;
    injection.kind = AssemblyGui::GuiThreadScanFailureKind::BaseException;
    injection.message = "Injected all-components preparation failure";
    task.testSetGuiThreadScanFailureInjection(injection);

    EXPECT_NO_THROW(task.testRunScan());
    assertGuiThreadSyncFailureRecoveryState(
        task,
        session,
        QStringLiteral("Injected all-components preparation failure")
    );
}

TEST_F(TaskInterferenceCheckTest, selectedPairPreparationFailureRecoversSynchronously)
{
    auto pairTask = makeOverlappingSelectedPairTask(_doc, _assembly);
    AssemblyGui::TaskInterferenceCheck& task = *pairTask;
    EXPECT_TRUE(task.testIsSelectedPairMode());
    EXPECT_TRUE(
        task.testScopeText().contains(QStringLiteral("selected pair"), Qt::CaseInsensitive)
    );

    ScopedGuiThreadScanFailureInjection clearInjection(task);
    AssemblyGui::GuiThreadScanFailureInjection injection;
    injection.stage = AssemblyGui::GuiThreadScanFailureStage::SelectedPairPreparation;
    injection.kind = AssemblyGui::GuiThreadScanFailureKind::StdException;
    injection.message = "Injected selected-pair preparation failure";
    task.testSetGuiThreadScanFailureInjection(injection);

    auto& session = task.scanSession();
    EXPECT_NO_THROW(task.testRunScan());
    assertGuiThreadSyncFailureRecoveryState(
        task,
        session,
        QStringLiteral("Injected selected-pair preparation failure"),
        QString()
    );
}

TEST_F(TaskInterferenceCheckTest, workerLaunchSetupFailureRecoversWithoutOrphanWatcher)
{
    addOverlappingPairForAsyncScan(_doc, _assembly);
    auto control = std::make_shared<AssemblyGui::InterferenceWorkerInjectionControl>();
    ScopedWorkerInjectionHoldRelease releaseHeldWorker(control);

    auto runLaunchFailureScan = [&](AssemblyGui::TaskInterferenceCheck& task) {
        task.testSetWorkerInjectionControl(control);
        ScopedGuiThreadScanFailureInjection clearInjection(task);
        AssemblyGui::GuiThreadScanFailureInjection injection;
        injection.stage = AssemblyGui::GuiThreadScanFailureStage::WorkerLaunchSetup;
        injection.kind = AssemblyGui::GuiThreadScanFailureKind::Unknown;
        task.testSetGuiThreadScanFailureInjection(injection);

        EXPECT_NO_THROW(task.testRunScan());
        EXPECT_FALSE(control->workerStarted.load(std::memory_order_acquire));
        EXPECT_EQ(task.testOwnedScanWatcherCount(), 0);

        const QString statusAfterRecovery = task.testStatusText();
        const QString progressAfterRecovery = task.testProgressText();
        const int rowsAfterRecovery = task.testTableRowCount();

        for (int i = 0; i < 30; ++i) {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        EXPECT_EQ(task.testOwnedScanWatcherCount(), 0);
        EXPECT_EQ(task.testStatusText(), statusAfterRecovery);
        EXPECT_EQ(task.testProgressText(), progressAfterRecovery);
        EXPECT_EQ(task.testTableRowCount(), rowsAfterRecovery);
        EXPECT_TRUE(task.testTableCellText(0, 7).contains(QStringLiteral("Unknown")));

        task.testClearWorkerInjectionControl();
    };

    {
        AssemblyGui::TaskInterferenceCheck task(_assembly);
        runLaunchFailureScan(task);
    }

    auto heapTask = std::make_unique<AssemblyGui::TaskInterferenceCheck>(_assembly);
    runLaunchFailureScan(*heapTask);
    heapTask.reset();
    QApplication::processEvents(QEventLoop::AllEvents, 50);
}

TEST_F(TaskInterferenceCheckTest, obsoleteGuiThreadPreparationFailureDoesNotOverwriteNewerScan)
{
    addOverlappingPairForAsyncScan(_doc, _assembly);
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    bool obsoleteFailureThrown = false;

    task.testSetPreparationBarrier([&]() {
        task.testClearPreparationBarrier();
        task.testClearGuiThreadScanFailureInjection();
        task.testRunScan();
        obsoleteFailureThrown = true;
        throw Base::RuntimeError("Injected obsolete synchronous preparation failure");
    });

    task.testRunScan();
    waitUntilScanIdle(task);

    EXPECT_TRUE(obsoleteFailureThrown);
    EXPECT_FALSE(task.isScanning());
    EXPECT_TRUE(task.hasResults());
    EXPECT_EQ(task.testOwnedScanWatcherCount(), 0);
    EXPECT_GE(task.testPenetrationCount(), 1);
    EXPECT_TRUE(
        task.testStatusText().contains(QStringLiteral("complete"), Qt::CaseInsensitive)
    );
    EXPECT_FALSE(
        task.testStatusText().contains(QStringLiteral("incomplete"), Qt::CaseInsensitive)
    );
    for (int row = 0; row < task.testTableRowCount(); ++row) {
        EXPECT_FALSE(
            task.testTableCellText(row, 7)
                .contains(QStringLiteral("Injected obsolete synchronous preparation failure"))
        );
    }
}

TEST_F(TaskInterferenceCheckTest, asyncScansCompleteWhenGuiThreadFailureInjectionDisabled)
{
    addOverlappingPairForAsyncScan(_doc, _assembly);
    AssemblyGui::TaskInterferenceCheck taskAll(_assembly);
    taskAll.testClearGuiThreadScanFailureInjection();
    taskAll.testClearInjectWorkerFailure();
    taskAll.testClearWorkerInjectionControl();
    taskAll.testRunScan();
    waitUntilScanIdle(taskAll);

    EXPECT_FALSE(taskAll.isScanning());
    EXPECT_TRUE(taskAll.hasResults());
    EXPECT_GE(taskAll.testPenetrationCount(), 1);
    EXPECT_TRUE(
        taskAll.testStatusText().contains(QStringLiteral("complete"), Qt::CaseInsensitive)
    );
    EXPECT_FALSE(
        taskAll.testStatusText().contains(QStringLiteral("Injected"), Qt::CaseInsensitive)
    );
    EXPECT_EQ(taskAll.testOwnedScanWatcherCount(), 0);

    Assembly::InterferenceComponentOccurrence occA;
    occA.component = _assembly;
    occA.occurrencePrefix = "WorkerFailA.";
    occA.displayPath = "WorkerFailA";
    Assembly::InterferenceComponentOccurrence occB;
    occB.component = _assembly;
    occB.occurrencePrefix = "WorkerFailB.";
    occB.displayPath = "WorkerFailB";
    AssemblyGui::TaskInterferenceCheck taskPair(_assembly, occA, occB);
    taskPair.testClearGuiThreadScanFailureInjection();
    taskPair.testRunScan();
    waitUntilScanIdle(taskPair);

    EXPECT_FALSE(taskPair.isScanning());
    EXPECT_TRUE(taskPair.hasResults());
    EXPECT_GE(taskPair.testPenetrationCount(), 1);
    EXPECT_TRUE(
        taskPair.testStatusText().contains(QStringLiteral("complete"), Qt::CaseInsensitive)
    );
    EXPECT_EQ(taskPair.testOwnedScanWatcherCount(), 0);
}

TEST_F(TaskInterferenceCheckTest, workerInjectedFailureRecoversThroughOnScanFinished)
{
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testEnsureDetachedPreviewRoot();
    ASSERT_TRUE(task.testHasPreviewRoot());

    auto& session = task.scanSession();
    const auto seed = session.beginScan();
    task.testDeliverScanFinished(seed.generation, makePlacedPenetrationResult());
    ASSERT_TRUE(task.hasResults());
    ASSERT_EQ(task.testTableRowCount(), 1);
    EXPECT_TRUE(task.testTableCellText(0, 1).contains(QStringLiteral("P_A")));
    task.testSelectResultRow(0);
    ASSERT_EQ(task.testPreviewShapeCount(), 2);

    addOverlappingPairForAsyncScan(_doc, _assembly);
    task.testSetPreparationBarrier([&]() {
        task.testSetInjectWorkerFailureForGeneration(task.scanSession().activeGeneration());
        task.testClearPreparationBarrier();
    });
    task.testRunScan();
    waitUntilScanIdle(task, /*allowNoResults=*/true);
    ASSERT_TRUE(waitWithEvents([&]() {
        return task.hasResults();
    }));

    EXPECT_FALSE(task.isScanning());
    EXPECT_FALSE(session.isBusy());
    EXPECT_FALSE(task.testIsPreparing());
    EXPECT_TRUE(task.testIsRunEnabled());
    EXPECT_FALSE(task.testIsCancelEnabled());
    EXPECT_TRUE(task.hasResults());
    EXPECT_TRUE(
        task.testStatusText().contains(QStringLiteral("incomplete"), Qt::CaseInsensitive)
        || task.testStatusText().contains(QStringLiteral("failed"), Qt::CaseInsensitive)
    );
    EXPECT_TRUE(task.testProgressText().isEmpty());
    EXPECT_EQ(task.testPreviewShapeCount(), 0);
    EXPECT_EQ(task.testPenetrationCount(), 0);
    EXPECT_EQ(task.testResultPairCount(), 0u);

    ASSERT_EQ(task.testTableRowCount(), 1);
    EXPECT_TRUE(
        task.testTableCellText(0, 0).contains(QStringLiteral("Diagnostic"), Qt::CaseInsensitive)
    );
    EXPECT_FALSE(task.testTableCellText(0, 1).contains(QStringLiteral("P_A")));
    EXPECT_TRUE(
        task.testTableCellText(0, 7).contains(QStringLiteral("Injected worker failure"))
    );
    task.testClearInjectWorkerFailure();
}

TEST_F(TaskInterferenceCheckTest, obsoleteWorkerFailureDoesNotOverwriteNewerSuccessfulResult)
{
    addOverlappingPairForAsyncScan(_doc, _assembly);
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    auto control = std::make_shared<AssemblyGui::InterferenceWorkerInjectionControl>();
    control->holdInWorker.store(true, std::memory_order_release);
    control->injectWatcherDelivered.store(false, std::memory_order_relaxed);
    ScopedWorkerInjectionHoldRelease releaseHeldWorker(control);
    task.testSetWorkerInjectionControl(control);

    task.testSetPreparationBarrier([&]() {
        const std::uint64_t generationA = task.scanSession().activeGeneration();
        task.testSetInjectWorkerFailureForGeneration(generationA);
        control->injectGeneration.store(generationA, std::memory_order_relaxed);
        task.testClearPreparationBarrier();
    });
    task.testRunScan();
    ASSERT_TRUE(waitUntilWorkerStarted(*control));

    task.testClearInjectWorkerFailure();
    control->injectGeneration.store(0, std::memory_order_relaxed);

    task.testRunScan();
    waitUntilScanIdle(task);

    EXPECT_TRUE(control->holdInWorker.load(std::memory_order_acquire));
    EXPECT_FALSE(control->injectWatcherDelivered.load(std::memory_order_acquire));
    EXPECT_FALSE(task.isScanning());
    EXPECT_TRUE(task.hasResults());
    EXPECT_GE(task.testPenetrationCount(), 1);
    EXPECT_TRUE(
        task.testStatusText().contains(QStringLiteral("complete"), Qt::CaseInsensitive)
    );
    EXPECT_FALSE(
        task.testStatusText().contains(QStringLiteral("incomplete"), Qt::CaseInsensitive)
    );

    const QString statusAfterB = task.testStatusText();
    const QString summaryAfterB = task.testSummaryText();
    const QString progressAfterB = task.testProgressText();
    const int tableRowsAfterB = task.testTableRowCount();
    const int penetrationAfterB = task.testPenetrationCount();
    const bool hasResultsAfterB = task.hasResults();
    const std::size_t pairCountAfterB = task.testResultPairCount();
    QStringList tableSnapshotAfterB;
    for (int row = 0; row < tableRowsAfterB; ++row) {
        QStringList cells;
        for (int col = 0; col < 8; ++col) {
            cells << task.testTableCellText(row, col);
        }
        tableSnapshotAfterB << cells.join(QLatin1Char('|'));
    }

    control->holdInWorker.store(false, std::memory_order_release);
    ASSERT_TRUE(waitUntilInjectWatcherDelivered(*control));

    EXPECT_EQ(task.testStatusText(), statusAfterB);
    EXPECT_EQ(task.testSummaryText(), summaryAfterB);
    EXPECT_EQ(task.testProgressText(), progressAfterB);
    EXPECT_EQ(task.testTableRowCount(), tableRowsAfterB);
    EXPECT_EQ(task.testPenetrationCount(), penetrationAfterB);
    EXPECT_EQ(task.hasResults(), hasResultsAfterB);
    EXPECT_EQ(task.testResultPairCount(), pairCountAfterB);
    for (int row = 0; row < tableRowsAfterB; ++row) {
        QStringList cells;
        for (int col = 0; col < 8; ++col) {
            cells << task.testTableCellText(row, col);
        }
        EXPECT_EQ(cells.join(QLatin1Char('|')), tableSnapshotAfterB[row]);
        EXPECT_FALSE(
            task.testTableCellText(row, 7).contains(QStringLiteral("Injected worker failure"))
        );
    }

    task.testClearWorkerInjectionControl();
    task.testClearInjectWorkerFailure();
}

TEST_F(TaskInterferenceCheckTest, asyncScanCompletesWhenWorkerFailureInjectionDisabled)
{
    addOverlappingPairForAsyncScan(_doc, _assembly);
    AssemblyGui::TaskInterferenceCheck task(_assembly);
    task.testClearInjectWorkerFailure();
    task.testClearWorkerInjectionControl();
    task.testRunScan();
    waitUntilScanIdle(task);

    EXPECT_FALSE(task.isScanning());
    EXPECT_TRUE(task.hasResults());
    EXPECT_GE(task.testPenetrationCount(), 1);
    EXPECT_TRUE(
        task.testStatusText().contains(QStringLiteral("complete"), Qt::CaseInsensitive)
    );
    EXPECT_FALSE(
        task.testStatusText().contains(QStringLiteral("Injected worker failure"))
    );
}

int main(int argc, char** argv)
{
    ensureDefaultOffscreenQtPlatform();
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    // Skip static FreeCAD/Qt destructors that SIGSEGV after offscreen QApplication.
    _Exit(result);
}
