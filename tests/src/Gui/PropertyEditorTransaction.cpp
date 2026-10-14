// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <QApplication>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Console.h>
#include <Gui/propertyeditor/PropertyEditor.h>
#include <src/App/InitApplication.h>

namespace
{
class TransactionWarnings: public Base::ILogger
{
public:
    void sendLog(const std::string&, const std::string& message, Base::LogStyle,
                 Base::IntendedRecipient, Base::ContentType) override
    {
        if (message.find("Cannot commit transaction while transacting") != std::string::npos) {
            ++count;
        }
    }
    int count = 0;
};

class PropertyEditorTransactionTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!QApplication::instance()) {
            static int argc = 1;
            static char name[] = "Gui_tests_run";
            static char* argv[] = {name, nullptr};
            new QApplication(argc, argv);
        }
        tests::initApplication();
        Gui::PropertyEditor::PropertyIntegerItem::init();
    }

    void SetUp() override
    {
        App::DocumentInitFlags flags;
        flags.createView = false;
        name = App::GetApplication().getUniqueDocumentName("editor_transaction");
        doc = App::GetApplication().newDocument(name.c_str(), "testUser", flags);
        App::GetApplication().setActiveDocument(doc);
        appLogLevel = Base::Console().getLogLevel("App");
        previousLevel = *appLogLevel;
        *appLogLevel = FC_LOGLEVEL_LOG;
        Base::Console().attachObserver(&warnings);
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(name.c_str());
        Base::Console().detachObserver(&warnings);
        *appLogLevel = previousLevel;
    }

    App::Document* doc = nullptr;
    std::string name;
    TransactionWarnings warnings;
    int* appLogLevel = nullptr;
    int previousLevel = 0;
};
}

TEST_F(PropertyEditorTransactionTest, resetPreservesUnrelatedBookedTransaction)
{
    Gui::PropertyEditor::PropertyEditor editor;
    const int transaction = doc->openTransaction("Unrelated creation");
    ASSERT_NE(transaction, 0);
    editor.reset();
    EXPECT_EQ(doc->getBookedTransactionID(), transaction);
    doc->addObject("App::VarSet", "Object");
    doc->commitTransaction();
    EXPECT_EQ(doc->getAvailableUndos(), 1);
    EXPECT_EQ(warnings.count, 0);
}

TEST_F(PropertyEditorTransactionTest, undoDeletionResetsIdleEditorsWithoutCommit)
{
    doc->openTransaction("Create selected object");
    auto* object = doc->addObject("App::VarSet", "Selected");
    auto* property = object->addDynamicProperty("App::PropertyInteger", "Size", "Test");
    doc->commitTransaction();
    ASSERT_EQ(doc->getBookedTransactionID(), 0);
    Gui::PropertyEditor::PropertyEditor dataEditor;
    Gui::PropertyEditor::PropertyEditor viewEditor;
    dataEditor.buildUp({{"Size", {property}}});
    bool resetDuringUndo = false;
    fastsignals::scoped_connection deleted = doc->signalDeletedObject.connect(
        [&](const App::DocumentObject& removed) {
            if (&removed == object) {
                // This is PropertyView::slotDeletedObject's reset path.
                resetDuringUndo = doc->isPerformingTransaction();
                EXPECT_EQ(doc->getBookedTransactionID(), 0);
                viewEditor.buildUp();
                dataEditor.buildUp();
                // Qt may defer modelReset delivery in this headless fixture.
                // Deliver the same reset entry point while undo is still active.
                viewEditor.reset();
                dataEditor.reset();
            }
        });
    doc->undo();
    EXPECT_TRUE(resetDuringUndo);
    EXPECT_EQ(warnings.count, 0);
    EXPECT_EQ(doc->getObject("Selected"), nullptr);
    EXPECT_EQ(doc->getAvailableUndos(), 0);
    EXPECT_EQ(doc->getAvailableRedos(), 1);
}
