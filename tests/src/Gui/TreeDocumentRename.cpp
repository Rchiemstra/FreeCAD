// SPDX-License-Identifier: LGPL-2.1-or-later
#include <gtest/gtest.h>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QTest>

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
        tree->resize(640, 480);
        tree->show();
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(otherName.c_str());
        App::GetApplication().closeDocument(name.c_str());
        tree.reset();
    }

    QLineEdit* pressF2()
    {
        auto* item = tree->getDocumentItem(Gui::Application::Instance->getDocument(doc));
        if (!item) {
            return nullptr;
        }
        tree->clearSelection();
        tree->setCurrentItem(item, 0);
        item->setSelected(true);
        tree->scrollToItem(item);
        tree->setFocus();
        // A regression to Save As must fail this test without leaving a modal
        // file dialog blocking the test process.
        bool unexpectedDialog = false;
        QTimer dialogGuard;
        dialogGuard.setSingleShot(true);
        QObject::connect(&dialogGuard, &QTimer::timeout, [&] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                unexpectedDialog = true;
                dialog->reject();
            }
        });
        dialogGuard.start(0);
        QKeyEvent key(QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier);
        QApplication::sendEvent(tree.get(), &key);
        dialogGuard.stop();
        QApplication::processEvents();
        EXPECT_FALSE(unexpectedDialog);
        EXPECT_EQ(QApplication::activeModalWidget(), nullptr);
        for (auto* editor : tree->viewport()->findChildren<QLineEdit*>()) {
            if (editor->isVisible()) {
                return editor;
            }
        }
        return nullptr;
    }

    bool waitForLabel(const QString& label)
    {
        return QTest::qWaitFor([&] {
            return !Gui::documentExecutionLaneBusy(*doc)
                && QString::fromUtf8(doc->Label.getValue()) == label;
        }, 30000);
    }

    App::Document* doc = nullptr;
    App::Document* other = nullptr;
    std::unique_ptr<Gui::TreeWidget> tree;
    std::string name, otherName;
};

TEST_F(TreeDocumentRenameTest, escapeCancelsInlineRename)
{
    const auto label = doc->Label.getStrValue();
    auto* editor = pressF2();
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->selectedText(), QString::fromStdString(label));
    editor->setText(QStringLiteral("Discarded rename"));
    QTest::keyClick(editor, Qt::Key_Escape);
    QApplication::processEvents();
    EXPECT_EQ(doc->Label.getStrValue(), label);
    EXPECT_EQ(tree->getDocumentItem(Gui::Application::Instance->getDocument(doc))->text(0),
              QString::fromStdString(label));
    EXPECT_TRUE(doc->FileName.getStrValue().empty());
    EXPECT_TRUE(other->FileName.getStrValue().empty());
}

TEST_F(TreeDocumentRenameTest, enterRenamesUnsavedDocumentInline)
{
    const auto otherLabel = other->Label.getStrValue();
    auto* editor = pressF2();
    ASSERT_NE(editor, nullptr);
    const QString label = QStringLiteral("Renamed winch");
    editor->setText(label);
    QTest::keyClick(editor, Qt::Key_Return);
    ASSERT_TRUE(waitForLabel(label));
    EXPECT_EQ(tree->getDocumentItem(Gui::Application::Instance->getDocument(doc))->text(0), label);
    EXPECT_TRUE(doc->FileName.getStrValue().empty());
    EXPECT_EQ(other->Label.getStrValue(), otherLabel);
    EXPECT_TRUE(other->FileName.getStrValue().empty());
}

TEST_F(TreeDocumentRenameTest, savedDocumentInlineRenamePreservesFile)
{
    QTemporaryDir folder;
    ASSERT_TRUE(folder.isValid());
    const auto oldPath = folder.filePath("original.FCStd").toStdString();
    const auto saved = Gui::Test::runOnDocumentOwnerWhilePumpingGui(*doc, [&] {
        return doc->saveAsWithOutcome(oldPath.c_str(), false);
    });
    ASSERT_TRUE(saved.succeeded());
    QFile file(QString::fromStdString(oldPath));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto originalBytes = file.readAll();
    file.close();

    const auto otherLabel = other->Label.getStrValue();
    auto* editor = pressF2();
    ASSERT_NE(editor, nullptr);
    const QString label = QStringLiteral("Renamed \"winch\" \u03a9");
    editor->setText(label);
    QTest::keyClick(editor, Qt::Key_Return);
    ASSERT_TRUE(waitForLabel(label));
    EXPECT_EQ(doc->FileName.getStrValue(), oldPath);
    EXPECT_EQ(tree->getDocumentItem(Gui::Application::Instance->getDocument(doc))->text(0), label);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), originalBytes);
    EXPECT_EQ(other->Label.getStrValue(), otherLabel);
    EXPECT_TRUE(other->FileName.getStrValue().empty());
}
} // namespace
