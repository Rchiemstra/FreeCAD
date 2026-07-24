// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>

#include <QApplication>

#include <App/Document.h>
#include <Base/Interpreter.h>
#include <Base/Quantity.h>
#include <Base/Unit.h>
#include <Gui/MainWindow.h>
#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/InterferenceScan.h>
#include <Mod/Assembly/Gui/TaskInterferenceCheck.h>
#include <Mod/Part/App/InterferenceDetection.h>
#include <src/App/InitApplication.h>

namespace
{

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

}  // namespace

class TaskInterferenceCheckTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        static int argc = 1;
        static char arg0[] = "AssemblyGui_tests_run";
        static char* argv[] = {arg0, nullptr};
        if (!QApplication::instance()) {
            // Intentionally leaked: FreeCAD/Qt teardown order crashes in offscreen gtests.
            new QApplication(argc, argv);
        }
        tests::initApplication();
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

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    // Skip static FreeCAD/Qt destructors that SIGSEGV after offscreen QApplication.
    _Exit(result);
}
