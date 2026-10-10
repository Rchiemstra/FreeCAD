// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <Inventor/nodes/SoTransform.h>

#include <QApplication>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentObject.h>
#include <App/DocumentRevisionIndex.h>
#include <App/Expression.h>
#include <App/PropertyGeo.h>
#include <App/PropertyUnits.h>
#include <Base/Placement.h>
#include <App/DocumentWouldBlock.h>
#include <App/MainThreadSignal.h>
#include <App/MergeDocuments.h>
#include <App/MutationClassification.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/Parameter.h>
#include <Base/Stream.h>
#include <Base/Tools.h>
#include <Gui/Application.h>
#include <Gui/CollaborationCompatibilityAdapter.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/MergeDocuments.h>
#include <Gui/ViewProviderDocumentObject.h>
#include <Gui/ViewProviderGeometryObject.h>
#include "CollaborationGuiTestHelpers.h"
#include <src/App/InitApplication.h>

namespace App::Internal
{

class DocumentCollaborationConcurrencyTestAccess
{
public:
    static std::recursive_mutex& commitMutex(App::Document& document) noexcept
    {
        return document.collaborationCommitMutex();
    }
};

}  // namespace App::Internal

namespace
{

using Kind = Gui::CollaborationCompatibilityMutationKind;
using Outcome = Gui::CollaborationCompatibilityMutationOutcome;
using Status = Gui::CollaborationCompatibilityMutationStatus;

Gui::CollaborationCompatibilityMutationDeclaration unknownModel()
{
    return {Kind::UnknownModel, {}, {}};
}

Gui::CollaborationCompatibilityCommit completingCommit(int& calls)
{
    return [&calls](const auto&, Gui::CollaborationCompatibilityMutationCallback&& callback) {
        ++calls;
        callback();
        return Outcome {Status::Completed, {}};
    };
}

void initializeCompatibilityGui()
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
}

class CollaborationCompatibilityIntegrationTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        initializeCompatibilityGui();
    }

    void SetUp() override
    {
        App::DocumentInitFlags flags;
        flags.createView = false;
        _documentName =
            App::GetApplication().getUniqueDocumentName("compatibilityIntegration");
        _document = App::GetApplication().newDocument(
            _documentName.c_str(), "compatibility integration", flags);
        ASSERT_NE(_document, nullptr);
        _object = _document->addObject("App::FeatureTest", "Target");
        ASSERT_NE(_object, nullptr);
        _object->Label.setValue("before");
        Gui::Test::recomputeWithoutBlockingGui(*_document);
        _guiDocument = Gui::Application::Instance->getDocument(_document);
        ASSERT_NE(_guiDocument, nullptr);
    }

    void TearDown() override
    {
        if (_document && App::GetApplication().getDocument(_documentName.c_str())) {
            App::GetApplication().closeDocument(_documentName.c_str());
            QApplication::processEvents();
        }
    }

    std::vector<App::DocumentRevisionObservation> captureRevisions() const
    {
        return _document->collaborationRevisions().capture(
            {App::DocumentRevisionKey::objectModel(_object->getNameInDocument()),
             App::DocumentRevisionKey::unknownModelMutation()});
    }

    App::Document* _document {nullptr};
    App::DocumentObject* _object {nullptr};
    Gui::Document* _guiDocument {nullptr};
    std::string _documentName;
};

}  // namespace

TEST(CollaborationCompatibilityAdapterTest, everyPersonalActionBypassesCommitAndCallback)
{
    constexpr std::array personalKinds {
        Kind::PersonalCamera,
        Kind::PersonalSelection,
        Kind::PersonalTree,
        Kind::PersonalActiveView,
        Kind::PersonalContext,
    };

    Gui::CollaborationCompatibilityAdapter adapter;
    for (const auto kind : personalKinds) {
        int commitCalls = 0;
        int callbackCalls = 0;
        const auto outcome = adapter.execute(
            {kind, {}, {}},
            completingCommit(commitCalls),
            [&callbackCalls] { ++callbackCalls; }
        );

        EXPECT_EQ(outcome.status, Status::RejectedPersonalContext);
        EXPECT_EQ(commitCalls, 0);
        EXPECT_EQ(callbackCalls, 0);
    }
}

TEST_F(CollaborationCompatibilityIntegrationTest,
       canonicalSaveDoesNotRequireMainWindowWithThumbnailEnabledOrDisabled)
{
    ASSERT_EQ(Gui::MainWindow::getInstance(), nullptr);
    ASSERT_TRUE(_guiDocument->getMDIViews().empty());

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto preferences = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document");
    const bool previousSaveThumbnail = preferences->GetBool("SaveThumbnail", true);
    auto restorePreference = qScopeGuard(
        [&] { preferences->SetBool("SaveThumbnail", previousSaveThumbnail); });

    for (const bool saveThumbnail : {false, true}) {
        preferences->SetBool("SaveThumbnail", saveThumbnail);
        _object->Label.setValue(saveThumbnail ? "thumbnail enabled" : "thumbnail disabled");
        const auto target = directory.filePath(
            saveThumbnail ? QStringLiteral("thumbnail-enabled.FCStd")
                          : QStringLiteral("thumbnail-disabled.FCStd"));

        const auto outcome = Gui::Test::saveAsWithOutcomeWithoutBlockingGui(
            *_document,
            target.toUtf8().constData(),
            false);
        EXPECT_TRUE(outcome.succeeded()) << outcome.errorCode << ": " << outcome.message;
        EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
        EXPECT_TRUE(outcome.fileWritten);
        EXPECT_TRUE(outcome.durabilityVerified);
    }
}

TEST(CollaborationCompatibilityAdapterTest, malformedAndContradictoryScopesRejectBeforeCommit)
{
    const std::array invalidDeclarations {
        Gui::CollaborationCompatibilityMutationDeclaration {Kind::Model, {}, {}},
        Gui::CollaborationCompatibilityMutationDeclaration {Kind::Model, "Object", {}},
        Gui::CollaborationCompatibilityMutationDeclaration {Kind::Model, {}, "stable-id"},
        Gui::CollaborationCompatibilityMutationDeclaration {
            Kind::UnknownModel, "Object", {}},
        Gui::CollaborationCompatibilityMutationDeclaration {
            Kind::UnknownModel, {}, "stable-id"},
        Gui::CollaborationCompatibilityMutationDeclaration {
            Kind::SharedPresentation, "Object", {}},
        Gui::CollaborationCompatibilityMutationDeclaration {
            Kind::SharedPresentation, {}, "stable-id"},
    };

    Gui::CollaborationCompatibilityAdapter adapter;
    for (const auto& declaration : invalidDeclarations) {
        int commitCalls = 0;
        int callbackCalls = 0;
        const auto outcome = adapter.execute(
            declaration,
            completingCommit(commitCalls),
            [&callbackCalls] { ++callbackCalls; }
        );

        EXPECT_EQ(outcome.status, Status::InvalidDeclaration);
        EXPECT_FALSE(outcome.diagnostic.empty());
        EXPECT_EQ(commitCalls, 0);
        EXPECT_EQ(callbackCalls, 0);
    }
}

TEST(CollaborationCompatibilityAdapterTest, missingDelegateAndCallbackAreDistinct)
{
    Gui::CollaborationCompatibilityAdapter adapter;
    int callbackCalls = 0;
    const auto noDelegate = adapter.execute(
        unknownModel(),
        {},
        [&callbackCalls] { ++callbackCalls; }
    );
    EXPECT_EQ(noDelegate.status, Status::MissingCommitDelegate);
    EXPECT_EQ(callbackCalls, 0);

    int commitCalls = 0;
    const auto noCallback = adapter.execute(
        unknownModel(), completingCommit(commitCalls), {});
    EXPECT_EQ(noCallback.status, Status::MissingMutationCallback);
    EXPECT_EQ(commitCalls, 0);
}

TEST(CollaborationCompatibilityAdapterTest, validModelIsDelegatedExactlyOnceWithValueIdentity)
{
    Gui::CollaborationCompatibilityAdapter adapter;
    int commitCalls = 0;
    int callbackCalls = 0;
    const auto outcome = adapter.execute(
        {Kind::Model, "Box", "object-17"},
        [&](const auto& declaration, Gui::CollaborationCompatibilityMutationCallback&& callback) {
            ++commitCalls;
            EXPECT_EQ(declaration.kind, Kind::Model);
            EXPECT_EQ(declaration.objectName, "Box");
            EXPECT_EQ(declaration.stableObjectIdentity, "object-17");
            callback();
            return Outcome {Status::Completed, "committed"};
        },
        [&callbackCalls] { ++callbackCalls; }
    );

    EXPECT_EQ(commitCalls, 1);
    EXPECT_EQ(callbackCalls, 1);
    EXPECT_EQ(outcome.status, Status::Completed);
    EXPECT_EQ(outcome.diagnostic, "committed");
}

TEST(CollaborationCompatibilityAdapterTest, delegateFailureOutcomeIsReturnedUnchanged)
{
    Gui::CollaborationCompatibilityAdapter adapter;
    const Outcome failure {Status::CommitFailed, "rollback completed"};
    int calls = 0;

    const auto outcome = adapter.execute(
        unknownModel(),
        [&](const auto&, Gui::CollaborationCompatibilityMutationCallback&&) {
            ++calls;
            return failure;
        },
        [] {}
    );

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(outcome.status, failure.status);
    EXPECT_EQ(outcome.diagnostic, failure.diagnostic);
}

TEST(CollaborationCompatibilityAdapterTest, delegateExceptionPropagatesAndReentrancyGuardReleases)
{
    Gui::CollaborationCompatibilityAdapter adapter;
    EXPECT_THROW(
        adapter.execute(
            unknownModel(),
            [](const auto&, Gui::CollaborationCompatibilityMutationCallback&&) -> Outcome {
                throw std::runtime_error("native commit failed");
            },
            [] {}
        ),
        std::runtime_error
    );

    int calls = 0;
    EXPECT_TRUE(adapter.execute(unknownModel(), completingCommit(calls), [] {}).completed());
    EXPECT_EQ(calls, 1);
}

TEST(CollaborationCompatibilityAdapterTest, nestedCallIsRejectedWhileDelegateIsActive)
{
    Gui::CollaborationCompatibilityAdapter adapter;
    Outcome nested;
    int outerCalls = 0;
    const auto outer = adapter.execute(
        unknownModel(),
        [&](const auto&, Gui::CollaborationCompatibilityMutationCallback&& callback) {
            ++outerCalls;
            nested = adapter.execute(
                {Kind::SharedPresentation, {}, {}},
                [](const auto&, Gui::CollaborationCompatibilityMutationCallback&&) {
                    return Outcome {Status::Completed, {}};
                },
                [] {}
            );
            callback();
            return Outcome {Status::Completed, {}};
        },
        [] {}
    );

    EXPECT_TRUE(outer.completed());
    EXPECT_EQ(outerCalls, 1);
    EXPECT_EQ(nested.status, Status::RejectedReentrant);
}

TEST(CollaborationCompatibilityAdapterTest, callFromAnotherThreadRejectsBeforeDelegate)
{
    Gui::CollaborationCompatibilityAdapter adapter;
    Outcome outcome;
    int commitCalls = 0;
    std::thread other([&] {
        outcome = adapter.execute(unknownModel(), completingCommit(commitCalls), [] {});
    });
    other.join();

    EXPECT_EQ(outcome.status, Status::RejectedWrongThread);
    EXPECT_EQ(commitCalls, 0);
}

TEST(CollaborationCompatibilityAdapterTest, sharedPresentationIsSerializationOnlyAndRevisionNeutral)
{
    Gui::CollaborationCompatibilityAdapter adapter;
    int commitCalls = 0;
    int callbackCalls = 0;

    const auto outcome = adapter.execute(
        {Kind::SharedPresentation, {}, {}},
        [&](const auto& declaration, Gui::CollaborationCompatibilityMutationCallback&& callback) {
            ++commitCalls;
            EXPECT_EQ(declaration.kind, Kind::SharedPresentation);
            EXPECT_TRUE(declaration.objectName.empty());
            EXPECT_TRUE(declaration.stableObjectIdentity.empty());
            callback();
            return Outcome {Status::Completed, "serialized without revision publication"};
        },
        [&] {
            ++callbackCalls;
        }
    );

    EXPECT_TRUE(outcome.completed());
    EXPECT_EQ(commitCalls, 1);
    EXPECT_EQ(callbackCalls, 1);
    // There is no publication/effects field in the Gui adapter contract. The
    // empty model scope above is the complete Phase 4/5 shared-presentation
    // declaration; the App-owned delegate performs serialization only.
}

TEST_F(CollaborationCompatibilityIntegrationTest,
       modelCallbackCommitsOnceAndPublishesObjectAndWildcardAtomically)
{
    const auto before = captureRevisions();
    int callbackCalls = 0;
    const auto outcome = Gui::Test::executeCompatibilityMutationWithoutBlockingGui(
        *_guiDocument,
        {Kind::Model,
         _object->getNameInDocument(),
         _document->collaborationObjectIdentity(*_object)},
        [&] {
            ++callbackCalls;
            _object->Label.setValue("committed");
        });

    ASSERT_TRUE(outcome.completed()) << outcome.diagnostic;
    EXPECT_EQ(callbackCalls, 1);
    EXPECT_STREQ(_object->Label.getValue(), "committed");
    const auto after = captureRevisions();
    ASSERT_EQ(before.size(), after.size());
    EXPECT_EQ(after[0].revision, before[0].revision + 1);
    EXPECT_EQ(after[1].revision, before[1].revision + 1);
    EXPECT_FALSE(_document->hasPendingTransaction());
}

TEST(GuiPythonCommandBridgeTest, preservesFileEvalAndErrorSemanticsWithoutGuiBootstrap)
{
    tests::initApplication();
    Base::PyGILStateLocker gil;

    PyObject* fileResult = Gui::Command::runPythonCommand(
        "__cc_wp04_bridge_value = 41",
        Gui::Command::PythonCommandMode::File);
    ASSERT_NE(fileResult, nullptr);
    EXPECT_EQ(fileResult, Py_None);
    Py_DECREF(fileResult);

    PyObject* evalResult = Gui::Command::runPythonCommand(
        "__cc_wp04_bridge_value + 1",
        Gui::Command::PythonCommandMode::Eval);
    ASSERT_NE(evalResult, nullptr);
    EXPECT_EQ(PyLong_AsLong(evalResult), 42);
    Py_DECREF(evalResult);

    PyObject* invalidResult = Gui::Command::runPythonCommand(
        "1 / 0",
        Gui::Command::PythonCommandMode::Eval);
    EXPECT_EQ(invalidResult, nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ZeroDivisionError));
    PyErr_Clear();

    PyObject* systemExit = Gui::Command::runPythonCommand(
        "raise SystemExit(7)", Gui::Command::PythonCommandMode::File);
    EXPECT_EQ(systemExit, nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_SystemExit));
    PyErr_Clear();
}

TEST(GuiCommandCoordinatorContractTest,
     longLivedPublicTransactionMakesCompetingCompatibilityCommitBusy)
{
    initializeCompatibilityGui();
    App::DocumentInitFlags flags;
    flags.createView = false;
    const auto documentName =
        App::GetApplication().getUniqueDocumentName("guiCommandCoordinatorContract");
    auto* document = App::GetApplication().newDocument(
        documentName.c_str(), "GUI command coordinator contract", flags);
    ASSERT_NE(document, nullptr);
    int transactionId = App::NullTransaction;
    auto closeDocument = qScopeGuard([&] {
        if (App::GetApplication().getDocument(documentName.c_str())) {
            if (document->hasPendingTransaction() && transactionId != App::NullTransaction) {
                App::GetApplication().abortTransaction(transactionId);
            }
            App::GetApplication().closeDocument(documentName.c_str());
        }
    });

    auto* object = document->addObject("App::FeatureTest", "Target");
    ASSERT_NE(object, nullptr);
    object->Label.setValue("before");
    Gui::Test::recomputeWithoutBlockingGui(*document);
    App::GetApplication().setActiveDocument(document);

    transactionId = App::GetApplication().setActiveTransaction(
        App::TransactionName {.name = "long-lived GUI task", .temporary = false});
    ASSERT_NE(transactionId, App::NullTransaction);

    int callbackCalls = 0;
    const auto attemptCommit = [&] {
        App::CollaborationCompatibilityMutation mutation;
        mutation.scope = App::CollaborationCompatibilityScope::UnknownModel;
        return document->collaborationService().commitCompatibilityMutation(
            std::move(mutation),
            [&] {
                ++callbackCalls;
                object->Label.setValue("after GUI task");
            });
    };

    const auto busy = Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, attemptCommit);
    EXPECT_EQ(busy.status, App::DocumentCommitStatus::Busy);
    EXPECT_EQ(callbackCalls, 0);
    EXPECT_STREQ(object->Label.getValue(), "before");

    ASSERT_TRUE(App::GetApplication().abortTransaction(transactionId));
    const auto completed =
        Gui::Test::runOnDocumentOwnerWhilePumpingGui(*document, attemptCommit);
    ASSERT_TRUE(completed.committed()) << completed.message;
    EXPECT_EQ(callbackCalls, 1);
    EXPECT_STREQ(object->Label.getValue(), "after GUI task");
}

TEST_F(CollaborationCompatibilityIntegrationTest,
       callbackFailureRollsBackAndDoesNotPublishRevisions)
{
    const auto before = captureRevisions();
    const auto outcome = Gui::Test::executeCompatibilityMutationWithoutBlockingGui(
        *_guiDocument,
        {Kind::UnknownModel, {}, {}},
        [&] {
            _object->Label.setValue("must roll back");
            throw std::runtime_error("legacy callback failed");
        });

    EXPECT_EQ(outcome.status, Status::CommitFailed);
    EXPECT_NE(outcome.diagnostic.find("legacy callback failed"), std::string::npos);
    EXPECT_STREQ(_object->Label.getValue(), "before");
    EXPECT_EQ(captureRevisions(), before);
    EXPECT_FALSE(_document->hasPendingTransaction());
}

TEST_F(CollaborationCompatibilityIntegrationTest,
       staleObjectIdentityRejectsBeforeCallback)
{
    const auto before = captureRevisions();
    int callbackCalls = 0;
    const auto outcome = Gui::Test::executeCompatibilityMutationWithoutBlockingGui(
        *_guiDocument,
        {Kind::Model, _object->getNameInDocument(), "stale-object-identity"},
        [&] { ++callbackCalls; });

    EXPECT_EQ(outcome.status, Status::CommitRejected);
    EXPECT_EQ(callbackCalls, 0);
    EXPECT_EQ(captureRevisions(), before);
}

TEST_F(CollaborationCompatibilityIntegrationTest,
       sharedPresentationSerializesWithoutModelRevisionOrTransaction)
{
    const auto before = captureRevisions();
    int callbackCalls = 0;
    const auto outcome = Gui::Test::executeCompatibilityMutationWithoutBlockingGui(
        *_guiDocument,
        {Kind::SharedPresentation, {}, {}},
        [&] { ++callbackCalls; });

    ASSERT_TRUE(outcome.completed()) << outcome.diagnostic;
    EXPECT_EQ(callbackCalls, 1);
    EXPECT_EQ(captureRevisions(), before);
    EXPECT_FALSE(_document->hasPendingTransaction());
}

TEST_F(CollaborationCompatibilityIntegrationTest,
       synchronousCompatibilitySupportsDocumentsWithPythonPayloads)
{
    ASSERT_NE(_object->addDynamicProperty("App::PropertyPythonObject", "PythonState"), nullptr);
    ASSERT_FALSE(_document->collaborationPreparationSupported());
    const auto before = captureRevisions();

    const auto outcome = Gui::Test::executeCompatibilityMutationWithoutBlockingGui(
        *_guiDocument,
        {Kind::UnknownModel, {}, {}},
        [&] { _object->Label.setValue("synchronous-python-document"); });

    ASSERT_TRUE(outcome.completed()) << outcome.diagnostic;
    EXPECT_STREQ(_object->Label.getValue(), "synchronous-python-document");
    const auto after = captureRevisions();
    ASSERT_EQ(after.size(), before.size());
    EXPECT_EQ(after[0].revision, before[0].revision);
    EXPECT_EQ(after[1].revision, before[1].revision + 1);
}

TEST_F(CollaborationCompatibilityIntegrationTest,
       structuralCreateDefersViewProviderUntilCommittedNewObjectReplay)
{
    App::DocumentObject* created = nullptr;
    bool callbackRan = false;
    bool callbackSawNoViewProvider = false;

    const auto result = Gui::Test::commitCompatibilityMutationWithoutBlockingGui(
        *_document,
        {App::CollaborationCompatibilityScope::Structural, {}, {}},
        [&] {
            callbackRan = true;
            created = _document->addObject("App::FeatureTest", "DeferredGuiObject");
            callbackSawNoViewProvider = created
                && _guiDocument->getViewProvider(created) == nullptr;
        });

    ASSERT_TRUE(result.committed()) << result.message;
    ASSERT_TRUE(callbackRan);
    ASSERT_NE(created, nullptr);
    EXPECT_TRUE(callbackSawNoViewProvider)
        << "Gui observed the object before the structural commit completed";
    EXPECT_EQ(_document->getObject("DeferredGuiObject"), created);
    EXPECT_NE(_guiDocument->getViewProvider(created), nullptr)
        << "deferred NewObject replay did not create the ViewProvider";
}

TEST_F(CollaborationCompatibilityIntegrationTest,
       structuralBulkImportDefersViewProviderAndRestoresGuiDataAfterCommit)
{
    const auto sourceName =
        App::GetApplication().getUniqueDocumentName("structuralGuiImportSource");
    App::DocumentInitFlags sourceFlags;
    sourceFlags.createView = false;
    auto* source = App::GetApplication().newDocument(
        sourceName.c_str(), "GUI import source", sourceFlags);
    ASSERT_NE(source, nullptr);
    auto* sourceObject = source->addObject("App::FeatureTest", "ImportedGuiFeature");
    ASSERT_NE(sourceObject, nullptr);
    auto* sourceGui = Gui::Application::Instance->getDocument(source);
    ASSERT_NE(sourceGui, nullptr);
    auto* sourceView = freecad_cast<Gui::ViewProviderDocumentObject*>(
        sourceGui->getViewProvider(sourceObject));
    ASSERT_NE(sourceView, nullptr);
    sourceView->ShowInTree.setValue(false);

    std::string archive;
    {
        Base::StringOStreambuf buffer(archive);
        std::ostream output(&buffer);
        Gui::MergeDocuments exportHooks(source);
        source->exportObjects({sourceObject}, output);
    }
    App::GetApplication().closeDocument(sourceName.c_str());
    QApplication::processEvents();

    int importSignals = 0;
    int finishSignals = 0;
    int finishRestoreSignals = 0;
    int newObjectSignals = 0;
    fastsignals::scoped_connection importConnection = _document->signalImportObjects.connect(
        [&](const auto&, Base::XMLReader&) { ++importSignals; });
    fastsignals::scoped_connection finishConnection = _document->signalFinishImportObjects.connect(
        [&](const auto&) { ++finishSignals; });
    fastsignals::scoped_connection finishRestoreConnection =
        _document->signalFinishRestoreObject.connect(
        [&](const App::DocumentObject& object) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument()) == "ImportedGuiFeature") {
                ++finishRestoreSignals;
            }
        });
    fastsignals::scoped_connection newConnection = _document->signalNewObject.connect(
        [&](const App::DocumentObject& object) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument()) == "ImportedGuiFeature") {
                ++newObjectSignals;
            }
        });
    App::DocumentObject* imported = nullptr;
    bool callbackSawNoObservers = false;
    bool callbackSawNoViewProvider = false;
    Gui::MergeDocuments retainedImporter(_document);

    const auto result = Gui::Test::commitCompatibilityMutationWithoutBlockingGui(
        *_document,
        {App::CollaborationCompatibilityScope::Structural, {}, {}},
        [&] {
            Base::StringIStreambuf buffer(archive);
            std::istream input(&buffer);
            const auto importedObjects = retainedImporter.importObjects(input);
            imported = importedObjects.size() == 1U ? importedObjects.front() : nullptr;
            callbackSawNoObservers = importSignals == 0 && finishSignals == 0
                && finishRestoreSignals == 0 && newObjectSignals == 0;
            callbackSawNoViewProvider = imported
                && _guiDocument->getViewProvider(imported) == nullptr;
        });

    ASSERT_TRUE(result.committed()) << result.message;
    ASSERT_NE(imported, nullptr);
    EXPECT_TRUE(callbackSawNoObservers);
    EXPECT_TRUE(callbackSawNoViewProvider);
    EXPECT_EQ(importSignals, 1);
    EXPECT_EQ(finishSignals, 1);
    EXPECT_EQ(finishRestoreSignals, 1);
    EXPECT_EQ(newObjectSignals, 1);
    auto* importedView = freecad_cast<Gui::ViewProviderDocumentObject*>(
        _guiDocument->getViewProvider(imported));
    ASSERT_NE(importedView, nullptr);
    EXPECT_FALSE(importedView->ShowInTree.getValue());

    int importViewSignals = 0;
    bool appCallbackSawNoImportViewSignal = false;
    App::DocumentObject* appImported = nullptr;
    fastsignals::scoped_connection importViewConnection =
        _document->signalImportViewObjects.connect(
        [&](const auto&, Base::Reader&, const auto&) { ++importViewSignals; });
    const auto appResult = Gui::Test::commitCompatibilityMutationWithoutBlockingGui(
        *_document,
        {App::CollaborationCompatibilityScope::Structural, {}, {}},
        [&] {
            Base::StringIStreambuf buffer(archive);
            std::istream input(&buffer);
            App::MergeDocuments importer(_document);
            const auto importedObjects = importer.importObjects(input);
            appImported = importedObjects.size() == 1U ? importedObjects.front() : nullptr;
            appCallbackSawNoImportViewSignal = importViewSignals == 0;
        });

    ASSERT_TRUE(appResult.committed()) << appResult.message;
    ASSERT_NE(appImported, nullptr);
    EXPECT_TRUE(appCallbackSawNoImportViewSignal);
    EXPECT_EQ(importViewSignals, 1);
    auto* appImportedView = freecad_cast<Gui::ViewProviderDocumentObject*>(
        _guiDocument->getViewProvider(appImported));
    ASSERT_NE(appImportedView, nullptr);
    EXPECT_FALSE(appImportedView->ShowInTree.getValue());

    Base::StringIStreambuf reimportBuffer(archive);
    std::istream reimportInput(&reimportBuffer);
    const auto reimported = retainedImporter.importObjects(reimportInput);
    ASSERT_EQ(reimported.size(), 1U);
    auto* reimportedView = freecad_cast<Gui::ViewProviderDocumentObject*>(
        _guiDocument->getViewProvider(reimported.front()));
    ASSERT_NE(reimportedView, nullptr);
    EXPECT_FALSE(reimportedView->ShowInTree.getValue());
}

TEST_F(CollaborationCompatibilityIntegrationTest,
       sharedPresentationHoldsAppCommitSerialization)
{
    bool competingThreadAcquiredMutex = false;
    const auto outcome = Gui::Test::executeCompatibilityMutationWithoutBlockingGui(
        *_guiDocument,
        {Kind::SharedPresentation, {}, {}},
        [&] {
            std::thread competing([&] {
                auto& mutex =
                    App::Internal::DocumentCollaborationConcurrencyTestAccess::commitMutex(
                        *_document);
                if (mutex.try_lock()) {
                    competingThreadAcquiredMutex = true;
                    mutex.unlock();
                }
            });
            competing.join();
        });

    ASSERT_TRUE(outcome.completed()) << outcome.diagnostic;
    EXPECT_FALSE(competingThreadAcquiredMutex)
        << "shared presentation callback ran without App commit serialization";
}

namespace
{

class ConsoleErrorCapture final: public Base::ILogger
{
public:
    ConsoleErrorCapture()
    {
        Base::Console().attachObserver(this);
    }

    ~ConsoleErrorCapture() override
    {
        Base::Console().detachObserver(this);
    }

    ConsoleErrorCapture(const ConsoleErrorCapture&) = delete;
    ConsoleErrorCapture& operator=(const ConsoleErrorCapture&) = delete;

    void sendLog(
        const std::string& /*notifiername*/,
        const std::string& msg,
        Base::LogStyle level,
        Base::IntendedRecipient /*recipient*/,
        Base::ContentType /*content*/
    ) override
    {
        if (level == Base::LogStyle::Error) {
            std::lock_guard lock(_mutex);
            _errors += msg;
        }
    }

    const char* name() override
    {
        return "ConsoleErrorCapture";
    }

    std::string errors() const
    {
        std::lock_guard lock(_mutex);
        return _errors;
    }

private:
    mutable std::mutex _mutex;
    std::string _errors;
};

}  // namespace

// Regression: the idle live-presentation catch-up refreshed view providers
// while another document held atomic presentation admission. The mutation
// guard then threw from the ObjectStatusLocker destructor in
// ViewProviderDocumentObject::updateView() and std::terminate aborted the run.
TEST_F(CollaborationCompatibilityIntegrationTest,
       idleLivePresentationCatchUpWaitsForForeignAtomicPresentationAdmission)
{
    // App::Placement's view provider has a display mode, so isShow() can turn
    // true again (App::FeatureTest's provider never shows anything).
    auto* placement = _document->addObject("App::Placement", "CatchUpPlacement");
    ASSERT_NE(placement, nullptr);
    Gui::Test::recomputeWithoutBlockingGui(*_document);
    auto* viewProvider = freecad_cast<Gui::ViewProviderDocumentObject*>(
        _guiDocument->getViewProvider(placement));
    ASSERT_NE(viewProvider, nullptr);
    ASSERT_TRUE(viewProvider->Visibility.getValue());
    ASSERT_TRUE(viewProvider->isShow());
    // Hide only the Coin switch and record it as a deferred show(), so the
    // catch-up has a visibility resync to do.
    viewProvider->Gui::ViewProvider::hide();
    ASSERT_FALSE(viewProvider->isShow());
    _guiDocument->noteDeferredVisibilityChange(viewProvider);

    App::DocumentInitFlags flags;
    flags.createView = false;
    const auto otherName =
        App::GetApplication().getUniqueDocumentName("catchUpAdmissionOther");
    auto* other = App::GetApplication().newDocument(
        otherName.c_str(), "catch-up admission other", flags);
    ASSERT_NE(other, nullptr);
    const auto closeOther = qScopeGuard([&] {
        App::GetApplication().closeDocument(otherName.c_str());
        QApplication::processEvents();
    });

    ConsoleErrorCapture capture;
    App::beginAtomicPresentationMutationTarget(*other);
    {
        const auto endAdmission =
            qScopeGuard([&] { App::endAtomicPresentationMutationTarget(*other); });
        _guiDocument->catchUpIdleLivePresentation();
        QApplication::processEvents();
        EXPECT_FALSE(viewProvider->isShow())
            << "catch-up refreshed view providers during a foreign admission";
    }
    EXPECT_EQ(capture.errors(), "");

    // The deferred catch-up stays queued and runs once admission has ended.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!viewProvider->isShow() && std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(viewProvider->isShow());
    EXPECT_EQ(capture.errors(), "");
}

// Synchronous compatibility: legacy callers (macros, workbench commands)
// recompute on the GUI thread. The recompute runs on the document owner while
// the GUI thread runs only marshalled owner work, never its Qt event loop.
TEST_F(CollaborationCompatibilityIntegrationTest,
       synchronousRecomputeOnGuiThreadWaitsWithoutEventLoop)
{
    _object->touch();
    ASSERT_TRUE(_object->isTouched());
    bool queuedEventRan = false;
    QTimer::singleShot(0, qApp, [&queuedEventRan] { queuedEventRan = true; });

    int recomputed = 0;
    ASSERT_NO_THROW(recomputed = _document->recompute());

    EXPECT_GE(recomputed, 1);
    EXPECT_FALSE(_object->isTouched());
    EXPECT_FALSE(queuedEventRan) << "the synchronous wait ran the Qt event loop";
    QApplication::processEvents();
    EXPECT_TRUE(queuedEventRan);
}

TEST_F(CollaborationCompatibilityIntegrationTest,
       synchronousPythonDocumentApisWorkOnGuiThread)
{
    const auto run = [](const std::string& code) {
        Base::PyGILStateLocker lock;
        Base::Interpreter().runString(code.c_str());
    };
    const std::string document = "App.getDocument('" + _documentName + "')";

    ASSERT_NO_THROW(run(document + ".getObject('Target').touch()"));
    EXPECT_TRUE(_object->isTouched());
    ASSERT_NO_THROW(run(document + ".recompute()"));
    EXPECT_FALSE(_object->isTouched());

    ASSERT_NO_THROW(run("App.closeDocument('" + _documentName + "')"));
    EXPECT_EQ(App::GetApplication().getDocument(_documentName.c_str()), nullptr)
        << "closeDocument() returned before the document was closed";
    _document = nullptr;
}

TEST_F(CollaborationCompatibilityIntegrationTest,
       synchronousCallInsideBlockingNotificationFailsInsteadOfDeadlocking)
{
    _object->touch();
    // While the GUI thread runs a functor the owner is blocked on, waiting for
    // that owner could never finish.
    App::MainThreadSignalConfig::BlockingInvokeScope blockingNotification;
    EXPECT_THROW(_document->recompute(), App::DocumentWouldBlock);
}

// Regression: the live catch-up re-synced every provider whose scene state
// differed from Visibility and so hid scene-only temporary visibility, such as
// a PartDesign boolean exposing its active tool body
// (TestActiveObject.testBooleanActiveBodyVisibilityWhenBooleanBecomesNonTip).
TEST_F(CollaborationCompatibilityIntegrationTest, liveCatchUpKeepsSceneOnlyVisibility)
{
    auto* placement = _document->addObject("App::Placement", "SceneOnlyPlacement");
    ASSERT_NE(placement, nullptr);
    Gui::Test::recomputeWithoutBlockingGui(*_document);
    auto* viewProvider = freecad_cast<Gui::ViewProviderDocumentObject*>(
        _guiDocument->getViewProvider(placement));
    ASSERT_NE(viewProvider, nullptr);

    viewProvider->hide();
    ASSERT_FALSE(viewProvider->Visibility.getValue());
    // Scene-only exposure: shown in Coin while Visibility stays false.
    viewProvider->Gui::ViewProvider::show();
    ASSERT_TRUE(viewProvider->isShow());

    _guiDocument->catchUpIdleLivePresentation();
    EXPECT_TRUE(viewProvider->isShow());
    EXPECT_FALSE(viewProvider->Visibility.getValue());
}

// Regression: a committed presentation revision applied after the live
// catch-up had already run left the non-pickable committed Coin root installed
// (prefersCommittedPresentation() stayed true) until the next model change, so
// nothing in the 3D view could be picked or preselected.
TEST_F(CollaborationCompatibilityIntegrationTest,
       committedPresentationRevisionHandsBackToLivePresentationWhenIdle)
{
    const auto processEventsFor = [](std::chrono::milliseconds duration) {
        const auto until = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < until) {
            QApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    // Let the live catch-up queued by SetUp's recompute run first.
    processEventsFor(std::chrono::milliseconds(300));

    _guiDocument->publishPresentationRevisionFromModel();
    Gui::PresentationApplyPumpResult result {};
    for (int turn = 0; turn < 20 && !result.committedRevision; ++turn) {
        result = _guiDocument->pumpPresentationApply(50);
    }
    ASSERT_TRUE(result.committedRevision);

    // Idle lane and no pending catch-up: the live view providers are current,
    // so the committed root must not replace them, now or later.
    EXPECT_FALSE(_guiDocument->prefersCommittedPresentation());
    processEventsFor(std::chrono::milliseconds(200));
    EXPECT_FALSE(_guiDocument->prefersCommittedPresentation());
}

// Regression: ViewProviderDocumentObject::updateView() holds an
// ObjectStatusLocker on Visibility. When the lane owner took atomic
// presentation admission while it was alive, restoring User1 threw from the
// destructor and std::terminate aborted Gui_tests_run.
TEST_F(CollaborationCompatibilityIntegrationTest,
       viewProviderRuntimeLockerIgnoresForeignAtomicPresentationAdmission)
{
    auto* viewProvider = freecad_cast<Gui::ViewProviderDocumentObject*>(
        _guiDocument->getViewProvider(_object));
    ASSERT_NE(viewProvider, nullptr);

    App::DocumentInitFlags flags;
    flags.createView = false;
    const auto otherName =
        App::GetApplication().getUniqueDocumentName("runtimeLockerAdmissionOther");
    auto* other = App::GetApplication().newDocument(
        otherName.c_str(), "runtime locker admission other", flags);
    ASSERT_NE(other, nullptr);
    const auto closeOther = qScopeGuard([&] {
        App::GetApplication().closeDocument(otherName.c_str());
        QApplication::processEvents();
    });

    using PropertyStatusLocker = Base::ObjectStatusLocker<App::Property::Status, App::Property>;
    App::beginAtomicPresentationMutationTarget(*other);
    const auto endAdmission =
        qScopeGuard([&] { App::endAtomicPresentationMutationTarget(*other); });
    EXPECT_NO_THROW({
        PropertyStatusLocker locker(App::Property::User1, &viewProvider->Visibility);
        EXPECT_TRUE(viewProvider->Visibility.testStatus(App::Property::User1));
    });
    EXPECT_FALSE(viewProvider->Visibility.testStatus(App::Property::User1));
    // Persisted status bits on view provider properties stay guarded.
    EXPECT_THROW(viewProvider->Visibility.setStatus(App::Property::Hidden, true),
                 Base::RuntimeError);
    EXPECT_FALSE(viewProvider->Visibility.testStatus(App::Property::Hidden));
}

namespace
{

App::DocumentObject* addShownPlacement(App::Document& document, const char* name)
{
    auto* placement = document.addObject("App::Placement", name);
    if (!placement) {
        return nullptr;
    }
    Gui::Test::recomputeWithoutBlockingGui(document);
    return placement;
}

Gui::ViewProviderDocumentObject* placementView(Gui::Document& guiDocument,
                                               App::DocumentObject& object)
{
    return freecad_cast<Gui::ViewProviderDocumentObject*>(guiDocument.getViewProvider(&object));
}

void pumpGuiUntil(const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

App::DocumentCommitResult commitOnPlacement(App::Document& document, std::function<void()> body)
{
    return Gui::Test::commitCompatibilityMutationWithoutBlockingGui(
        document,
        {App::CollaborationCompatibilityScope::Structural, {}, {}},
        std::move(body));
}

}  // namespace

// A direct App Visibility write on the GUI thread is applied by
// slotChangedObject -> ViewProviderDocumentObject::update().
TEST_F(CollaborationCompatibilityIntegrationTest, guiThreadVisibilityHideIsPresented)
{
    auto* placement = addShownPlacement(*_document, "DirectVisibility");
    ASSERT_NE(placement, nullptr);
    auto* view = placementView(*_guiDocument, *placement);
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(placement->Visibility.getValue());
    ASSERT_TRUE(view->Visibility.getValue());
    ASSERT_TRUE(view->isShow());

    placement->Visibility.setValue(false);
    pumpGuiUntil([&] { return !view->Visibility.getValue() && !view->isShow(); });

    EXPECT_FALSE(placement->Visibility.getValue());
    EXPECT_FALSE(view->Visibility.getValue());
    EXPECT_FALSE(view->isShow());
}

// Placement is an App property whose view refresh goes through updateData(),
// which the idle catch-up already replays. Visibility is the special case.
TEST_F(CollaborationCompatibilityIntegrationTest, committedPlacementChangeIsPresented)
{
    auto* placement = addShownPlacement(*_document, "CommittedPlacement");
    ASSERT_NE(placement, nullptr);
    auto* view = placementView(*_guiDocument, *placement);
    ASSERT_NE(view, nullptr);
    auto* transform = view->getTransformNode();
    ASSERT_NE(transform, nullptr);

    auto* placementProperty =
        dynamic_cast<App::PropertyPlacement*>(placement->getPropertyByName("Placement"));
    ASSERT_NE(placementProperty, nullptr);
    const auto result = commitOnPlacement(*_document, [&] {
        placementProperty->setValue(Base::Placement(Base::Vector3d(12, 0, 0), Base::Rotation()));
    });
    ASSERT_TRUE(result.committed()) << result.message;
    pumpGuiUntil([&] { return transform->translation.getValue()[0] > 11.0F; });

    EXPECT_NEAR(transform->translation.getValue()[0], 12.0, 1e-4);
}

// Regression: a collaborative commit sets App::DocumentObject::Visibility on
// the owner thread. The change is replayed while live presentation is
// deferred, and the idle catch-up must still copy it onto the view provider
// (and the reverse show).
TEST_F(CollaborationCompatibilityIntegrationTest, committedVisibilityChangeIsPresented)
{
    auto* placement = addShownPlacement(*_document, "CommittedVisibility");
    ASSERT_NE(placement, nullptr);
    auto* view = placementView(*_guiDocument, *placement);
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(view->Visibility.getValue());
    ASSERT_TRUE(view->isShow());

    const auto hidden = commitOnPlacement(*_document, [&] {
        placement->Visibility.setValue(false);
    });
    ASSERT_TRUE(hidden.committed()) << hidden.message;
    pumpGuiUntil([&] { return !view->Visibility.getValue() && !view->isShow(); });

    EXPECT_FALSE(placement->Visibility.getValue());
    EXPECT_FALSE(view->Visibility.getValue());
    EXPECT_FALSE(view->isShow());

    // Reverse: hide on the GUI thread (that path already follows the view
    // provider), then show again from inside a collaborative commit.
    auto* restored = addShownPlacement(*_document, "CommittedVisibilityShow");
    ASSERT_NE(restored, nullptr);
    auto* restoredView = placementView(*_guiDocument, *restored);
    ASSERT_NE(restoredView, nullptr);
    restored->Visibility.setValue(false);
    pumpGuiUntil([&] { return !restoredView->Visibility.getValue() && !restoredView->isShow(); });
    ASSERT_FALSE(restoredView->Visibility.getValue());
    ASSERT_FALSE(restoredView->isShow());

    const auto shown = commitOnPlacement(*_document, [&] {
        restored->Visibility.setValue(true);
    });
    ASSERT_TRUE(shown.committed()) << shown.message;
    pumpGuiUntil([&] { return restoredView->Visibility.getValue() && restoredView->isShow(); });

    EXPECT_TRUE(restored->Visibility.getValue());
    EXPECT_TRUE(restoredView->Visibility.getValue());
    EXPECT_TRUE(restoredView->isShow());
}

int countOccurrences(const std::string& haystack, const std::string& needle)
{
    int count = 0;
    for (std::size_t pos = 0; (pos = haystack.find(needle, pos)) != std::string::npos;
         pos += needle.size()) {
        ++count;
    }
    return count;
}

// A live main window (tree, property view, report view) is what recomputes a
// Part feature after its expression source document has closed.
class CrossDocumentExpressionGuiTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        initializeCompatibilityGui();
        Base::Interpreter().runString("import Part");
        Base::Interpreter().runString("import PartGui");
        Base::Interpreter().runString("import Spreadsheet");
        // One window for the suite. Destroying it and constructing another
        // re-enters a freed status-bar child.
        if (!Gui::MainWindow::getInstance()) {
            new Gui::MainWindow();
        }
    }

    void SetUp() override
    {
        _directory = std::filesystem::temp_directory_path()
            / ("fc-xdoc-gui-"
               + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(_directory);

        App::DocumentInitFlags flags;
        // A 3D view is an MDI child. Leaving it for process teardown destroys
        // that QMdiSubWindow after QApplication and segfaults in QFontCache.
        flags.createView = false;
        _sourceName = App::GetApplication().getUniqueDocumentName("QASrc");
        _drivenName = App::GetApplication().getUniqueDocumentName("QADst");
        _source = App::GetApplication().newDocument(_sourceName.c_str(), "sourceUser", flags);
        _driven = App::GetApplication().newDocument(_drivenName.c_str(), "drivenUser", flags);
        ASSERT_NE(_source, nullptr);
        ASSERT_NE(_driven, nullptr);
        QApplication::processEvents();

        auto* sheet = _source->addObject("Spreadsheet::Sheet", "Dims");
        ASSERT_NE(sheet, nullptr);
        const std::string sheetSetup = "App.getDocument('" + _sourceName
            + "').getObject('Dims').set('A1', '25')\n"
            + "App.getDocument('" + _sourceName + "').getObject('Dims').setAlias('A1', 'Height')\n";
        Base::Interpreter().runString(sheetSetup.c_str());
        ASSERT_NO_THROW(static_cast<void>(_source->recompute()));
        ASSERT_NE(sheet->getPropertyByName("Height"), nullptr);
        _drivenBox = _driven->addObject("Part::Box", "Box");
        ASSERT_NE(_drivenBox, nullptr);
        drivenLength()->setValue(10.0);
        Gui::Test::saveAsWithoutBlockingGui(*_source, (_directory / "qa_src.FCStd").string().c_str());
        Gui::Test::saveAsWithoutBlockingGui(*_driven, (_directory / "qa_dst.FCStd").string().c_str());

        const std::string expression = _sourceName + "#<<Dims>>.Height";
        _drivenBox->setExpression(
            App::ObjectIdentifier(*drivenLength()),
            std::shared_ptr<App::Expression>(App::Expression::parse(_drivenBox, expression)));
    }

    void TearDown() override
    {
        if (App::GetApplication().getDocument(_drivenName.c_str())) {
            App::GetApplication().closeDocument(_drivenName.c_str());
        }
        if (App::GetApplication().getDocument(_sourceName.c_str())) {
            App::GetApplication().closeDocument(_sourceName.c_str());
        }
        QApplication::processEvents();
        std::error_code error;
        std::filesystem::remove_all(_directory, error);
    }

    App::PropertyLength* drivenLength() const
    {
        return dynamic_cast<App::PropertyLength*>(_drivenBox->getPropertyByName("Length"));
    }

    std::filesystem::path _directory;
    std::string _sourceName;
    std::string _drivenName;
    App::Document* _source {nullptr};
    App::Document* _driven {nullptr};
    App::DocumentObject* _drivenBox {nullptr};
};

TEST_F(CrossDocumentExpressionGuiTest, openSourceRecomputeDoesNotLogExpressionErrors)
{
    ConsoleErrorCapture capture;
    EXPECT_NO_THROW(static_cast<void>(_driven->recompute()));
    QApplication::processEvents();

    EXPECT_FALSE(_drivenBox->isError());
    ASSERT_NE(drivenLength(), nullptr);
    EXPECT_DOUBLE_EQ(drivenLength()->getValue(), 25.0);
    const auto errors = capture.errors();
    EXPECT_EQ(errors.find("not found"), std::string::npos) << errors;
    EXPECT_EQ(errors.find("mutation is unavailable"), std::string::npos) << errors;
}

TEST_F(CrossDocumentExpressionGuiTest, closedSourceRecomputeDoesNotMutateDuringPresentation)
{
    EXPECT_NO_THROW(static_cast<void>(_driven->recompute()));
    QApplication::processEvents();
    ASSERT_FALSE(_drivenBox->isError());
    ASSERT_NE(drivenLength(), nullptr);
    ASSERT_DOUBLE_EQ(drivenLength()->getValue(), 25.0);

    App::GetApplication().closeDocument(_sourceName.c_str());
    _source = nullptr;
    QApplication::processEvents();

    ConsoleErrorCapture capture;
    EXPECT_NO_THROW(static_cast<void>(_driven->recompute()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_NE(App::GetApplication().getDocument(_drivenName.c_str()), nullptr);
    EXPECT_TRUE(_drivenBox->isError());
    const char* diagnostic = _driven->getErrorDescription(_drivenBox);
    ASSERT_NE(diagnostic, nullptr);
    const std::string diagnosticText(diagnostic);
    EXPECT_NE(diagnosticText.find("not found"), std::string::npos) << diagnosticText;
    EXPECT_NE(diagnosticText.find(_sourceName), std::string::npos) << diagnosticText;

    const auto errors = capture.errors();
    EXPECT_EQ(countOccurrences(errors, "not found"), 1) << errors;
    EXPECT_EQ(errors.find("mutation is unavailable"), std::string::npos) << errors;
}

// A GUI edit of shape appearance, with no atomic presentation in progress,
// reaches the view provider and does not trip the mutation guard.
TEST_F(CrossDocumentExpressionGuiTest, guiThreadAppearanceEditIsPresented)
{
    auto* guiDocument = Gui::Application::Instance->getDocument(_driven);
    ASSERT_NE(guiDocument, nullptr);
    auto* view = freecad_cast<Gui::ViewProviderGeometryObject*>(
        guiDocument->getViewProvider(_drivenBox));
    ASSERT_NE(view, nullptr);

    App::Material material = view->ShapeAppearance[0];
    material.diffuseColor = Base::Color(0.1F, 0.2F, 0.3F);
    ConsoleErrorCapture capture;
    EXPECT_NO_THROW(view->ShapeAppearance.setValue(material));

    EXPECT_EQ(view->ShapeAppearance[0].diffuseColor, material.diffuseColor);
    const auto errors = capture.errors();
    EXPECT_EQ(errors.find("mutation is unavailable"), std::string::npos) << errors;
}

// Regression: refreshing a view property while the document owner holds atomic
// presentation admission (the closed-source recompute's error presentation)
// called into the App document and the GUI catch logged "mutation is
// unavailable from a non-owner thread during an atomic presentation callback".
TEST_F(CrossDocumentExpressionGuiTest, viewAppearanceDuringOwnerAdmissionIsNotLogged)
{
    auto* guiDocument = Gui::Application::Instance->getDocument(_driven);
    ASSERT_NE(guiDocument, nullptr);
    auto* view = freecad_cast<Gui::ViewProviderGeometryObject*>(
        guiDocument->getViewProvider(_drivenBox));
    ASSERT_NE(view, nullptr);

    App::Material material = view->ShapeAppearance[0];
    material.diffuseColor = Base::Color(0.4F, 0.5F, 0.6F);
    std::string thrown;
    ConsoleErrorCapture capture;
    Gui::Test::runOnDocumentOwnerWhilePumpingGui(*_driven, [&] {
        App::beginAtomicPresentationMutationTarget(*_driven);
        const auto endAdmission =
            qScopeGuard([&] { App::endAtomicPresentationMutationTarget(*_driven); });
        App::MainThreadSignalConfig::invoke(
            [&] {
                try {
                    view->ShapeAppearance.setValue(material);
                }
                catch (const Base::Exception& error) {
                    thrown = error.what();
                }
            },
            true);
    });

    EXPECT_TRUE(thrown.empty()) << thrown;
    const auto errors = capture.errors();
    EXPECT_EQ(errors.find("mutation is unavailable"), std::string::npos) << errors;
    EXPECT_EQ(view->ShapeAppearance[0].diffuseColor, material.diffuseColor);
}
