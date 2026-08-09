// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QScrollBar>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentObjectGroup.h>
#include <App/DocumentRevisionIndex.h>
#include <Gui/Application.h>
#include <Gui/Camera.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Tree.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <src/App/InitApplication.h>

namespace
{

void initializeHeadlessGui()
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    static int argc = 1;
    static char executable[] = "Gui_tests_run";
    static char* argv[] = {executable, nullptr};
    if (!QApplication::instance()) {
        // FreeCAD and Qt have process-lifetime registries. Keeping the application alive avoids
        // static teardown ordering failures in the shared Gui gtest binary.
        new QApplication(argc, argv);
    }

    tests::initApplication();
    if (!Gui::Application::Instance) {
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        // GUIenabled=true installs the App document signal connections. A false application would
        // make this fixture depend on suite order and silently omit Gui::Document registration.
        new Gui::Application(true);
    }
    if (!Gui::MainWindow::getInstance()) {
        // View3DInventor's navigation cube queries the active MDI view while it builds its menu.
        // Keep the process-lifetime shell alive for the same reason as QApplication above.
        new Gui::MainWindow();
    }
}

class CollaborationViewIsolationTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        initializeHeadlessGui();
    }

    void SetUp() override
    {
        Gui::Selection().clearCompleteSelection();
        _tree = std::make_unique<Gui::TreeWidget>("collaboration-view-isolation");
        _tree->resize(240, 80);

        App::DocumentInitFlags flags;
        flags.createView = false;
        _documentName = App::GetApplication().getUniqueDocumentName("viewIsolation");
        _document = App::GetApplication().newDocument(
            _documentName.c_str(),
            "viewIsolationUser",
            flags
        );
        ASSERT_NE(_document, nullptr);
        ASSERT_NE(Gui::Application::Instance, nullptr);
        _guiDocument = Gui::Application::Instance->getDocument(_document);
        ASSERT_NE(_guiDocument, nullptr);

        _group = _document->addObject<App::DocumentObjectGroup>("PersonalTreeGroup");
        ASSERT_NE(_group, nullptr);
        for (int index = 0; index < 16; ++index) {
            const std::string name = "TreeChild" + std::to_string(index);
            auto* child = _document->addObject("App::FeatureTest", name.c_str());
            ASSERT_NE(child, nullptr);
            _group->addObject(child);
            if (index == 15) {
                _selectedObject = child;
            }
        }
        ASSERT_NE(_selectedObject, nullptr);
        Gui::TreeWidget::updateStatus(false);
        QApplication::processEvents();
    }

    void TearDown() override
    {
        Gui::Selection().clearCompleteSelection();
        if (_document && App::GetApplication().getDocument(_documentName.c_str())) {
            App::GetApplication().closeDocument(_documentName.c_str());
            QApplication::processEvents();
        }
        _tree.reset();
        _guiDocument = nullptr;
        _document = nullptr;
        _group = nullptr;
        _selectedObject = nullptr;
    }

    std::vector<App::DocumentRevisionKey> modelAndDocumentKeys() const
    {
        std::vector<App::DocumentRevisionKey> keys;
        for (const auto* object : {static_cast<App::DocumentObject*>(_group), _selectedObject}) {
            const std::string name = object->getNameInDocument();
            keys.push_back(App::DocumentRevisionKey::objectExistence(name));
            keys.push_back(App::DocumentRevisionKey::objectModel(name));
            keys.push_back(App::DocumentRevisionKey::objectStructure(name));
        }
        keys.push_back(App::DocumentRevisionKey::documentStructure());
        keys.push_back(App::DocumentRevisionKey::unknownModelMutation());
        return keys;
    }

    std::vector<App::DocumentRevisionObservation> captureModelAndDocumentRevisions() const
    {
        return _document->collaborationRevisions().capture(modelAndDocumentKeys());
    }

    std::pair<bool, bool> captureDirtyState() const
    {
        return {_document->isTouched(), _guiDocument->isModified()};
    }

    Gui::TreeWidget* tree() const
    {
        return _tree.get();
    }

    App::DocumentObject* selectedObject() const
    {
        return _selectedObject;
    }

    Gui::Document* guiDocument() const
    {
        return _guiDocument;
    }

    const std::string& documentName() const
    {
        return _documentName;
    }

private:
    std::unique_ptr<Gui::TreeWidget> _tree;
    std::string _documentName;
    App::Document* _document {};
    Gui::Document* _guiDocument {};
    App::DocumentObjectGroup* _group {};
    App::DocumentObject* _selectedObject {};
};

}  // namespace

TEST_F(CollaborationViewIsolationTest, personalCameraChangesDoNotAdvanceAppRevisions)
{
    Gui::View3DInventor view(guiDocument(), nullptr);
    auto* viewer = view.getViewer();
    ASSERT_NE(viewer, nullptr);
    viewer->setAnimationEnabled(false);
    view.resize(240, 180);
    view.show();
    QApplication::processEvents();
    const auto initialOrientation = viewer->getCameraOrientation();
    auto targetOrientation = Gui::Camera::isometric();
    if (Gui::Camera::rotationsMatch(initialOrientation, targetOrientation)) {
        targetOrientation = Gui::Camera::front();
    }
    ASSERT_FALSE(Gui::Camera::rotationsMatch(initialOrientation, targetOrientation));
    const auto before = captureModelAndDocumentRevisions();
    const auto dirtyBefore = captureDirtyState();

    viewer->setCameraOrientation(targetOrientation);
    QApplication::processEvents();

    EXPECT_TRUE(Gui::Camera::rotationsMatch(viewer->getCameraOrientation(), targetOrientation));
    EXPECT_FALSE(Gui::Camera::rotationsMatch(viewer->getCameraOrientation(), initialOrientation));
    EXPECT_EQ(captureModelAndDocumentRevisions(), before);
    EXPECT_EQ(captureDirtyState(), dirtyBefore);
}

TEST_F(CollaborationViewIsolationTest, selectionAndPreselectionDoNotAdvanceAppRevisions)
{
    const auto before = captureModelAndDocumentRevisions();
    const auto dirtyBefore = captureDirtyState();
    const char* objectName = selectedObject()->getNameInDocument();

    ASSERT_TRUE(Gui::Selection().addSelection(documentName().c_str(), objectName));
    Gui::Selection().setPreselect(
        documentName().c_str(),
        objectName,
        "",
        1.0F,
        2.0F,
        3.0F,
        Gui::SelectionChanges::MsgSource::TreeView
    );
    EXPECT_TRUE(Gui::Selection().isSelected(documentName().c_str(), objectName));
    EXPECT_TRUE(Gui::Selection().hasPreselection());
    EXPECT_EQ(captureModelAndDocumentRevisions(), before);
    EXPECT_EQ(captureDirtyState(), dirtyBefore);

    Gui::Selection().clearCompleteSelection();
    EXPECT_EQ(captureModelAndDocumentRevisions(), before);
    EXPECT_EQ(captureDirtyState(), dirtyBefore);
}

TEST_F(CollaborationViewIsolationTest, treeExpansionAndScrollDoNotAdvanceAppRevisions)
{
    tree()->show();
    (void)tree()->winId();
    tree()->updateGeometry();
    tree()->update();
    tree()->viewport()->update();
    QApplication::processEvents();

    auto* documentItem = tree()->getDocumentItem(guiDocument());
    ASSERT_NE(documentItem, nullptr);
    const auto before = captureModelAndDocumentRevisions();
    const auto dirtyBefore = captureDirtyState();
    tree()->expandItem(documentItem);
    QApplication::processEvents();
    ASSERT_TRUE(documentItem->isExpanded());
    EXPECT_EQ(captureModelAndDocumentRevisions(), before);
    EXPECT_EQ(captureDirtyState(), dirtyBefore);

    const auto groupItems = tree()->findItems(
        QStringLiteral("PersonalTreeGroup"),
        Qt::MatchExactly | Qt::MatchRecursive,
        0
    );
    ASSERT_FALSE(groupItems.empty());
    auto* groupItem = groupItems.front();
    ASSERT_EQ(groupItem->type(), Gui::TreeWidget::ObjectType);

    tree()->expandItem(groupItem);
    QApplication::processEvents();
    ASSERT_TRUE(groupItem->isExpanded());
    EXPECT_EQ(captureModelAndDocumentRevisions(), before);
    EXPECT_EQ(captureDirtyState(), dirtyBefore);

    tree()->collapseItem(groupItem);
    QApplication::processEvents();
    ASSERT_FALSE(groupItem->isExpanded());
    EXPECT_EQ(captureModelAndDocumentRevisions(), before);
    EXPECT_EQ(captureDirtyState(), dirtyBefore);

    tree()->expandItem(groupItem);
    tree()->updateGeometry();
    tree()->update();
    tree()->viewport()->update();
    QApplication::processEvents();
    EXPECT_TRUE(groupItem->isExpanded());

    const auto childItems = tree()->findItems(
        QString::fromUtf8(selectedObject()->Label.getValue()),
        Qt::MatchExactly | Qt::MatchRecursive,
        0
    );
    ASSERT_FALSE(childItems.empty());
    auto* scrollBar = tree()->verticalScrollBar();
    scrollBar->setValue(scrollBar->minimum());
    QApplication::processEvents();
    const int scrollBefore = scrollBar->value();
    ASSERT_EQ(scrollBefore, scrollBar->minimum());
    ASSERT_GT(scrollBar->maximum(), 0)
        << "the realized 80px tree must overflow after expanding its document and group";
    ASSERT_GT(scrollBar->maximum(), scrollBefore);
    tree()->scrollToItem(childItems.front(), QAbstractItemView::PositionAtBottom);
    scrollBar->setValue(scrollBar->maximum());
    QApplication::processEvents();
    EXPECT_GT(scrollBar->value(), 0);
    EXPECT_EQ(scrollBar->value(), scrollBar->maximum());

    EXPECT_EQ(captureModelAndDocumentRevisions(), before);
    EXPECT_EQ(captureDirtyState(), dirtyBefore);
}
