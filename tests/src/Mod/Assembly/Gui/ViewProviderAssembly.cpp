// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cstdlib>
#include <stdexcept>
#include <QApplication>
#include <App/Document.h>
#include <Base/Interpreter.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoSwitch.h>
#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/Gui/ViewProviderAssembly.h>
#include <Mod/Part/App/FeaturePartBox.h>
#include <src/App/InitApplication.h>

class AssemblyDragGuiTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        static int argc = 1;
        static char name[] = "AssemblyDragGui_tests_run";
        static char* argv[] = {name, nullptr};
        new QApplication(argc, argv);
        tests::initApplication();
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        new Gui::Application(false);
        Base::Interpreter().runString("import Part, Material, AssemblyApp, AssemblyGui");
    }

    void SetUp() override
    {
        App::DocumentInitFlags flags;
        flags.createView = false;
        document = App::GetApplication().newDocument("DragExit", "Drag exit", flags);
        guiDocument = new Gui::Document(document, Gui::Application::Instance);
        assembly = document->addObject<Assembly::AssemblyObject>("Assembly");
        part = assembly->addObject<Part::Box>("DraggedBox");
    }

    void TearDown() override
    {
        delete guiDocument;
        App::GetApplication().closeDocument("DragExit");
    }

    class DragViewProvider: public AssemblyGui::ViewProviderAssembly
    {
    public:
        explicit DragViewProvider(Gui::Document* document) { pcDocument = document; }
    };

    void startMove(DragViewProvider& provider)
    {
        provider.attach(assembly);
        // A placeholder edit node is enough: no rendered view or mouse events.
        provider.asmDraggerSwitch = new SoSwitch;
        provider.asmDraggerSwitch->ref();
        provider.getRoot()->addChild(provider.asmDraggerSwitch);
        assembly->saveDragState();
        guiDocument->openCommand("Move part");
        provider.partMoving = true;
        provider.canStartDragging = true;
        part->Placement.setValue(Base::Placement(Base::Vector3d(12, 3, 4), Base::Rotation()));
        ASSERT_TRUE(document->hasPendingTransaction());
    }

    void finishMove()
    {
        // Permit assertion failures on the pre-fix tree to clean up normally.
        assembly->postDrag(true);
        if (document->hasPendingTransaction()) {
            guiDocument->abortCommand();
        }
        assembly->finishDrag();
    }

    App::Document* document = nullptr;
    Gui::Document* guiDocument = nullptr;
    Assembly::AssemblyObject* assembly = nullptr;
    Part::Box* part = nullptr;
};

TEST_F(AssemblyDragGuiTest, leavingEditRejectsActiveDrag)
{
    const auto original = part->Placement.getValue();
    DragViewProvider provider(guiDocument);
    startMove(provider);

    provider.unsetEdit(Gui::ViewProvider::Default);

    EXPECT_TRUE(part->Placement.getValue().isSame(original));
    EXPECT_FALSE(document->hasPendingTransaction());
    EXPECT_FALSE(provider.partMoving);
    EXPECT_FALSE(provider.canStartDragging);
    finishMove();
}

TEST_F(AssemblyDragGuiTest, dragNotificationFailureStillAbortsCommand)
{
    const auto original = part->Placement.getValue();
    DragViewProvider provider(guiDocument);
    startMove(provider);
    auto connection = assembly->signalSolverUpdate.connect([] {
        throw std::runtime_error("drag completion observer failure");
    });

    EXPECT_THROW(provider.endMove(true), std::runtime_error);

    connection.disconnect();
    provider.unsetDragger();
    EXPECT_TRUE(part->Placement.getValue().isSame(original));
    EXPECT_FALSE(document->hasPendingTransaction());
    EXPECT_FALSE(provider.partMoving);
    EXPECT_FALSE(provider.canStartDragging);
    finishMove();
}

int main(int argc, char** argv)
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    // Avoid FreeCAD/Qt static teardown after the lifecycle assertions finish.
    _Exit(result);
}
