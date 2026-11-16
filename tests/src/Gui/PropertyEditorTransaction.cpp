// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <QApplication>
#include <QScopeGuard>
#include <QSpinBox>

#include <App/Application.h>
#include <App/CollaborativeSetPropertyOperation.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentObject.h>
#include <App/Expression.h>
#include <App/FeatureTest.h>
#include <Base/Console.h>
#include <Gui/DocumentExecutionIngress.h>
#include <Gui/propertyeditor/PropertyEditor.h>
#include <src/App/InitApplication.h>

#include "CollaborationGuiTestHelpers.h"

#include <chrono>
#include <future>
#include <thread>

namespace
{
template<typename Predicate>
bool waitWhilePumpingGui(Predicate ready)
{
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        std::this_thread::sleep_for(1ms);
    }
    return ready();
}

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

TEST_F(PropertyEditorTransactionTest, untouchedDataEditorAllowsCollaboration)
{
    App::ensureCollaborativeSetPropertyOperationRegistered();
    auto* object = doc->addObject("App::VarSet", "Values");
    auto* property = object->addDynamicProperty("App::PropertyInteger", "Size", "Test");
    Gui::Test::recomputeWithoutBlockingGui(*doc);
    doc->clearUndos();
    const auto session = doc->collaborationService().beginEditSession("property-editor-test");
    ASSERT_FALSE(doc->isTouched());
    ASSERT_EQ(doc->getBookedTransactionID(), 0);
    const App::CollaborativeOperationIntent intent {
        std::string(App::CollaborativeSetPropertyOperationType),
        {{"object", "Values"}, {"property", "Size"},
         {"value_type", "integer"}, {"value", "42"}}};
    ASSERT_NO_THROW(Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] {
        return doc->collaborationService().prepareEdit(
            session.sessionId(), "before-editor-opens", intent, "property-editor-test");
    }));

    Gui::PropertyEditor::PropertyEditor editor;
    editor.setAutomaticDocumentUpdate(true);
    editor.buildUp({{"Size", {property}}});
    const auto index = editor.model()->index(0, 1, editor.model()->index(0, 0));
    editor.openEditor(index);
    ASSERT_NE(editor.indexWidget(index), nullptr);
    EXPECT_EQ(doc->getBookedTransactionID(), 0);
    EXPECT_NO_THROW(Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] {
        return doc->collaborationService().snapshotForEdit(
            session.sessionId(), {App::DocumentRevisionKey::objectModel("Values")});
    }));
    EXPECT_NO_THROW(Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] {
        return doc->collaborationService().prepareEdit(
            session.sessionId(), "while-editor-idle", intent, "property-editor-test");
    }));
    EXPECT_EQ(doc->getAvailableUndos(), 0);
    auto* spinBox = qobject_cast<QSpinBox*>(editor.indexWidget(index));
    ASSERT_NE(spinBox, nullptr);
    spinBox->setValue(42);
    ASSERT_TRUE(waitWhilePumpingGui([&] { return doc->executionLane()->isIdle(); }));
    EXPECT_EQ(static_cast<App::PropertyInteger*>(property)->getValue(), 42);
    EXPECT_EQ(doc->getAvailableUndoNames(), (std::vector<std::string> {"Edit Values.Size"}));
    editor.closeEditor();
    ASSERT_TRUE(waitWhilePumpingGui([&] { return doc->executionLane()->isIdle(); }));
    EXPECT_EQ(doc->getBookedTransactionID(), 0);
    EXPECT_FALSE(doc->hasPendingTransaction());
}

namespace
{
enum class CloseEditPhase { BeforeMutation, DuringMutation, AfterCompletion };

class PropertyEditorLaneEditTest:
    public PropertyEditorTransactionTest,
    public ::testing::WithParamInterface<CloseEditPhase>
{};
}

TEST_P(PropertyEditorLaneEditTest, closingEditorPreservesNamedEditUndoAndRecompute)
{
    using namespace std::chrono_literals;
    auto* object = doc->addObject<App::FeatureTest>("Values");
    object->Integer.setValue(3);
    object->setExpression(App::ObjectIdentifier(object->Float),
                          App::Expression::parse(object, "Integer * 2"));
    Gui::Test::recomputeWithoutBlockingGui(*doc);
    doc->clearUndos();
    ASSERT_EQ(object->Float.getValue(), 6.0);
    ASSERT_FALSE(doc->isTouched());

    Gui::PropertyEditor::PropertyEditor editor;
    editor.setAutomaticDocumentUpdate(true);
    editor.buildUp({{"Integer", {&object->Integer}}});
    const auto index = editor.model()->index(0, 1, editor.model()->index(0, 0));
    editor.openEditor(index);
    auto* spinBox = qobject_cast<QSpinBox*>(editor.indexWidget(index));
    ASSERT_NE(spinBox, nullptr);

    // Pause the owner at deterministic boundaries, without reading live model
    // state on the GUI thread while mutation is in progress.
    std::promise<void> entered;
    auto paused = entered.get_future();
    std::promise<void> release;
    auto resume = release.get_future().share();
    bool released = false;
    const auto unblock = [&] {
        if (!released) {
            release.set_value();
            released = true;
        }
    };
    std::future<void> ownerPause;
    fastsignals::scoped_connection beforeChange;
    bool pausedMutation = false;
    const auto cleanup = qScopeGuard([&] {
        unblock();
        if (ownerPause.valid()) {
            ownerPause.wait();
        }
        EXPECT_TRUE(waitWhilePumpingGui([&] { return doc->executionLane()->isIdle(); }));
    });
    if (GetParam() == CloseEditPhase::BeforeMutation) {
        ownerPause = doc->executionLane()->postToOwner([&] {
            entered.set_value();
            resume.wait();
        });
        ASSERT_EQ(paused.wait_for(5s), std::future_status::ready);
    }
    else if (GetParam() == CloseEditPhase::DuringMutation) {
        beforeChange = object->signalBeforeChange.connect(
            [&](const App::DocumentObject&, const App::Property& property) {
                if (&property == &object->Integer && !pausedMutation) {
                    pausedMutation = true;
                    entered.set_value();
                    resume.wait();
                }
            });
    }
    // Real delegate -> PropertyModel -> PropertyItem -> submitted lane Edit.
    spinBox->setValue(17);
    if (GetParam() == CloseEditPhase::DuringMutation) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (paused.wait_for(0s) != std::future_status::ready
               && std::chrono::steady_clock::now() < deadline) {
            QApplication::processEvents();
            std::this_thread::sleep_for(1ms);
        }
        ASSERT_EQ(paused.wait_for(0s), std::future_status::ready);
    }
    if (GetParam() != CloseEditPhase::AfterCompletion) {
        editor.closeEditor();
        EXPECT_EQ(editor.indexWidget(index), nullptr);
        unblock();
        if (ownerPause.valid()) {
            ownerPause.get();
        }
    }
    ASSERT_TRUE(waitWhilePumpingGui([&] { return doc->executionLane()->isIdle(); }));
    beforeChange.disconnect();
    if (GetParam() == CloseEditPhase::AfterCompletion) {
        editor.closeEditor();
    }
    ASSERT_TRUE(waitWhilePumpingGui([&] { return doc->executionLane()->isIdle(); }));

    EXPECT_EQ(object->Integer.getValue(), 17);
    EXPECT_EQ(object->Float.getValue(), 34.0);
    EXPECT_FALSE(doc->isTouched());
    EXPECT_EQ(doc->getAvailableUndos(), 1);
    EXPECT_EQ(doc->getAvailableUndoNames(), (std::vector<std::string> {"Edit Values.Integer"}));
    EXPECT_EQ(doc->getBookedTransactionID(), 0);
    EXPECT_FALSE(doc->hasPendingTransaction());

    const auto undo = Gui::submitDocumentKindCommand(*doc, App::DocumentCommandKind::Undo);
    ASSERT_TRUE(undo.accepted()) << undo.diagnostic;
    App::DocumentCommandHandle undoHandle(undo.commandId, doc->executionHandle().identity());
    ASSERT_TRUE(waitWhilePumpingGui([&] { return undoHandle.status().terminal(); }));
    ASSERT_EQ(undoHandle.status().state, App::DocumentCommandState::Completed)
        << undoHandle.status().diagnostic;
    EXPECT_EQ(object->Integer.getValue(), 3);
    EXPECT_EQ(object->Float.getValue(), 6.0);
    EXPECT_EQ(doc->getAvailableUndos(), 0);
    EXPECT_EQ(doc->getAvailableRedos(), 1);
    EXPECT_EQ(doc->getBookedTransactionID(), 0);
    EXPECT_EQ(warnings.count, 0);
}

INSTANTIATE_TEST_SUITE_P(ClosePhases, PropertyEditorLaneEditTest,
    ::testing::Values(CloseEditPhase::BeforeMutation, CloseEditPhase::DuringMutation,
                      CloseEditPhase::AfterCompletion));
