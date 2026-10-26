// SPDX-License-Identifier: LGPL-2.1-or-later
#include <gtest/gtest.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>

#include <App/Application.h>
#include <App/Document.h>
#include <App/MutationClassification.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/DocumentExecutionIngress.h>
#include <Gui/MainWindow.h>
#include <Gui/Tree.h>
#include <src/App/InitApplication.h>
#include "CollaborationGuiTestHelpers.h"

namespace
{
class TreeDocumentRenameTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        if (!QApplication::instance()) {
            static int argc = 1;
            static char name[] = "TreeDocumentRename";
            static char* argv[] = {name, nullptr};
            new QApplication(argc, argv);
        }
        tests::initApplication();
        if (!Gui::Application::Instance) {
            Gui::Application::initApplication();
            Gui::Application::initOpenInventor();
            new Gui::Application(true);
        }
        if (!Gui::MainWindow::getInstance()) {
            new Gui::MainWindow();
        }
    }

    void SetUp() override
    {
        tree = std::make_unique<Gui::TreeWidget>("F2 test");
        App::DocumentInitFlags flags;
        flags.createView = false;
        name = App::GetApplication().getUniqueDocumentName("F2Selected");
        doc = App::GetApplication().newDocument(name.c_str(), "testUser", flags);
        otherName = App::GetApplication().getUniqueDocumentName("F2Other");
        other = App::GetApplication().newDocument(otherName.c_str(), "testUser", flags);
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(otherName.c_str());
        App::GetApplication().closeDocument(name.c_str());
        tree.reset();
    }

    void pressF2(Gui::TreeWidget& tree, const std::function<void(QFileDialog*)>& choose)
    {
        auto* item = tree.getDocumentItem(Gui::Application::Instance->getDocument(doc));
        ASSERT_NE(item, nullptr);
        tree.setCurrentItem(item);
        item->setSelected(true);
        bool opened = false;
        QTimer::singleShot(0, &tree, [&] {
            auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            opened = dialog != nullptr;
            if (dialog) {
                choose(dialog);
            }
        });
        QKeyEvent key(QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier);
        QApplication::sendEvent(&tree, &key);
        EXPECT_TRUE(opened);
    }

    App::Document* doc = nullptr;
    App::Document* other = nullptr;
    std::unique_ptr<Gui::TreeWidget> tree;
    std::string name, otherName;
};

TEST_F(TreeDocumentRenameTest, cancelLeavesUnsavedDocumentIdentityIntact)
{
    const auto label = doc->Label.getStrValue();
    pressF2(*tree, [](QFileDialog* dialog) { dialog->reject(); });
    EXPECT_EQ(doc->Label.getStrValue(), label);
    EXPECT_TRUE(doc->FileName.getStrValue().empty());
    EXPECT_TRUE(other->FileName.getStrValue().empty());
}

TEST_F(TreeDocumentRenameTest, selectedDocumentGetsNewDiskFilenameAndLabel)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());
    const auto oldPath = folder.filePath("original.FCStd").toStdString();
    const auto newPath = folder.filePath("renamed.FCStd");
    const auto saved = Gui::Test::runOnDocumentOwnerWhilePumpingGui(*doc, [&] {
        return doc->saveAsWithOutcome(oldPath.c_str(), false);
    });
    ASSERT_TRUE(saved.succeeded());
    pressF2(*tree, [&](QFileDialog* dialog) {
        dialog->selectFile(newPath);
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    });
    QElapsedTimer timer;
    timer.start();
    while (Gui::documentExecutionLaneBusy(*doc) && timer.elapsed() < 30000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    ASSERT_FALSE(Gui::documentExecutionLaneBusy(*doc));
    EXPECT_TRUE(QFileInfo::exists(newPath));
    // The native Save As workflow retains the previous file as a recoverable copy.
    EXPECT_TRUE(QFileInfo::exists(QString::fromStdString(oldPath)));
    EXPECT_EQ(doc->FileName.getStrValue(), newPath.toStdString());
    EXPECT_EQ(doc->Label.getStrValue(), "renamed");
    EXPECT_TRUE(other->FileName.getStrValue().empty());
}
} // namespace
