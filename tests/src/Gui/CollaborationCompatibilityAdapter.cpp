// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <thread>

#include <QApplication>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentObject.h>
#include <App/DocumentRevisionIndex.h>
#include <App/MergeDocuments.h>
#include <Base/Parameter.h>
#include <Base/Stream.h>
#include <Gui/Application.h>
#include <Gui/CollaborationCompatibilityAdapter.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/MergeDocuments.h>
#include <Gui/ViewProviderDocumentObject.h>
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
        _document->recompute();
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

        const auto outcome = _document->saveAsWithOutcome(target.toUtf8().constData(), false);
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
    const auto outcome = _guiDocument->executeCompatibilityMutation(
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

TEST_F(CollaborationCompatibilityIntegrationTest,
       callbackFailureRollsBackAndDoesNotPublishRevisions)
{
    const auto before = captureRevisions();
    const auto outcome = _guiDocument->executeCompatibilityMutation(
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
    const auto outcome = _guiDocument->executeCompatibilityMutation(
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
    const auto outcome = _guiDocument->executeCompatibilityMutation(
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

    const auto outcome = _guiDocument->executeCompatibilityMutation(
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

    const auto result = _document->collaborationService().commitCompatibilityMutation(
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

    const auto result = _document->collaborationService().commitCompatibilityMutation(
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
    const auto appResult = _document->collaborationService().commitCompatibilityMutation(
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
    const auto outcome = _guiDocument->executeCompatibilityMutation(
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
