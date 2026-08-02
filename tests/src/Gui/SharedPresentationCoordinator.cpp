// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/DocumentRevisionIndex.h>
#include <Gui/SharedPresentationCoordinator.h>

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

constexpr App::DocumentInstanceId TestDocumentInstance = 41;
constexpr App::DocumentLifecycleEpoch TestDocumentEpoch = 7;

using CommitStatus = Gui::SharedPresentationCommitStatus;
using Key = Gui::SharedPresentationRevisionKey;
using StepResult = Gui::SharedPresentationStepResult;

App::DocumentRevisionIdentityBinding testBinding()
{
    return {TestDocumentInstance, TestDocumentEpoch};
}

App::DocumentIdentity liveIdentity()
{
    return {TestDocumentInstance,
            TestDocumentEpoch,
            App::DocumentLifecycleState::Live};
}

Key visibilityKey()
{
    return {"stable-object-17", "Visibility"};
}

StepResult succeeded()
{
    return {true, {}};
}

StepResult failed(std::string diagnostic)
{
    return {false, std::move(diagnostic)};
}

Gui::SharedPresentationCommitRequest requestFor(
    Gui::SharedPresentationRevisionIndex& presentation,
    const Key& key,
    std::vector<App::DocumentRevisionObservation> appObservations = {})
{
    return {testBinding(),
            std::move(appObservations),
            presentation.capture({key}),
            {key}};
}

struct CommitHarness
{
    CommitHarness()
    {
        presentation.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
        app.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
        request = requestFor(presentation, visibilityKey(), app.capture({appWildcard}));

        callbacks.serialize = [this](Gui::SharedPresentationCommitWork&& work,
                                     Gui::SharedPresentationCommitCompletion&& complete) {
            std::lock_guard<std::mutex> lock(serializationMutex);
            ++serializationCalls;
            work();
            complete({true, {}});
        };
        callbacks.currentIdentity = [this] {
            ++lifecycleCalls;
            return std::optional<App::DocumentIdentity> {liveIdentity()};
        };
        callbacks.validateAppRevisions =
            [this](const std::vector<App::DocumentRevisionObservation>& observations) {
                ++appValidationCalls;
                return app.validate(observations);
            };
        callbacks.applyAppMutation = [this] {
            ++appApplyCalls;
            appBefore = appState;
            ++appState;
            trace.push_back("apply-app");
            return succeeded();
        };
        callbacks.applyGuiMutation = [this] {
            ++guiApplyCalls;
            guiBefore = guiState;
            ++guiState;
            trace.push_back("apply-gui");
            return succeeded();
        };
        callbacks.checkPostcondition = [this] {
            ++postconditionCalls;
            trace.push_back("postcondition");
            return succeeded();
        };
        callbacks.makeAppDurable = [this] {
            ++appDurabilityCalls;
            trace.push_back("commit-app");
            return succeeded();
        };
        callbacks.rollbackGuiMutation = [this] {
            ++guiRollbackCalls;
            guiState = guiBefore;
            trace.push_back("rollback-gui");
            return succeeded();
        };
        callbacks.rollbackAppMutation = [this] {
            ++appRollbackCalls;
            appState = appBefore;
            trace.push_back("rollback-app");
            return succeeded();
        };
    }

    Gui::SharedPresentationCommitResult commit()
    {
        return coordinator.commit(presentation, request, callbacks);
    }

    Gui::SharedPresentationRevisionIndex presentation;
    App::DocumentRevisionIndex app;
    Gui::SharedPresentationCoordinator coordinator;
    const App::DocumentRevisionKey appWildcard =
        App::DocumentRevisionKey::unknownModelMutation();
    Gui::SharedPresentationCommitRequest request;
    Gui::SharedPresentationCommitCallbacks callbacks;
    std::mutex serializationMutex;
    int serializationCalls {0};
    int lifecycleCalls {0};
    int appValidationCalls {0};
    int appApplyCalls {0};
    int guiApplyCalls {0};
    int postconditionCalls {0};
    int appDurabilityCalls {0};
    int guiRollbackCalls {0};
    int appRollbackCalls {0};
    int appState {0};
    int guiState {0};
    int appBefore {0};
    int guiBefore {0};
    std::vector<std::string> trace;
};

void expectNoPresentationPublication(const CommitHarness& harness)
{
    EXPECT_EQ(harness.presentation.latestPublicationSequence(), 0);
    EXPECT_EQ(harness.presentation.current(visibilityKey()), 0);
}

}  // namespace

TEST(SharedPresentationRevisionIndexTest,
     keyKeepsStableIdentityAndPropertyAsIndependentCopiedScalars)
{
    std::string objectIdentity = "ab";
    std::string propertyName = "c";
    const Key first {objectIdentity, propertyName};
    const Key second {"a", "bc"};
    objectIdentity = "changed-object";
    propertyName = "changed-property";

    Gui::SharedPresentationRevisionIndex index;
    index.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    const auto captured = index.capture({first, second});
    ASSERT_EQ(captured.size(), 2);
    EXPECT_EQ(captured[0].key.stableObjectIdentity, "ab");
    EXPECT_EQ(captured[0].key.propertyName, "c");
    EXPECT_NE(captured[0].key, captured[1].key);
    EXPECT_EQ(captured[0].document, testBinding());

    const auto publication = index.publish({first, second});
    ASSERT_EQ(publication.changes.size(), 2);
    EXPECT_EQ(publication.document, testBinding());
    EXPECT_EQ(publication.publicationSequence, 1);
    EXPECT_EQ(publication.changes[0].key.stableObjectIdentity, "ab");
    EXPECT_EQ(publication.changes[0].key.propertyName, "c");
    EXPECT_EQ(publication.changes[0].revision, 1);
}

TEST(SharedPresentationRevisionIndexTest,
     deliberateViewProviderPropertiesCaptureValidateAndPublishAtomically)
{
    const std::vector<Key> keys {
        {"stable-object", "Visibility"},
        {"stable-object", "Color"},
        {"stable-object", "Transparency"},
        {"stable-object", "DisplayMode"},
        {"stable-object", "Annotation"},
    };
    Gui::SharedPresentationRevisionIndex index;
    index.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);

    const auto before = index.capture(keys);
    EXPECT_TRUE(index.validate(before).valid());
    const auto publication = index.publish(keys);

    EXPECT_EQ(publication.changes.size(), keys.size());
    EXPECT_EQ(index.latestPublicationSequence(), 1);
    const auto validation = index.validate(before);
    EXPECT_EQ(validation.status, Gui::SharedPresentationValidationStatus::Conflict);
    EXPECT_EQ(validation.conflicts.size(), keys.size());
    for (const auto& change : publication.changes) {
        EXPECT_EQ(change.revision, 1);
        EXPECT_EQ(change.document, testBinding());
    }
}

TEST(SharedPresentationRevisionIndexTest, observationsAreExactEpochBoundWithoutRevisionRewind)
{
    Gui::SharedPresentationRevisionIndex index;
    index.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    const auto key = visibilityKey();
    static_cast<void>(index.publish({key}));
    const auto oldEpoch = index.capture({key});

    index.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch + 1);
    const auto stale = index.validate(oldEpoch);
    EXPECT_EQ(stale.status, Gui::SharedPresentationValidationStatus::EpochMismatch);
    const auto newEpoch = index.capture({key});
    ASSERT_EQ(newEpoch.size(), 1);
    EXPECT_EQ(newEpoch[0].document.lifecycleEpoch, TestDocumentEpoch + 1);
    EXPECT_EQ(newEpoch[0].revision, 1);

    const auto next = index.publish({key});
    EXPECT_EQ(next.publicationSequence, 2);
    ASSERT_EQ(next.changes.size(), 1);
    EXPECT_EQ(next.changes[0].revision, 2);
}

TEST(SharedPresentationRevisionIndexTest,
     publicationSequenceOverflowLeavesEveryRevisionAndPriorPublicationUnchanged)
{
    Gui::SharedPresentationRevisionIndex index(
        std::numeric_limits<Gui::SharedPresentationRevision>::max(), 1);
    index.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    const Key firstKey {"stable-object", "Visibility"};
    const Key secondKey {"stable-object", "Color"};
    const auto priorPublication = index.publish({firstKey});

    EXPECT_THROW(static_cast<void>(index.publish({secondKey})), std::overflow_error);
    EXPECT_EQ(index.latestPublicationSequence(), 1);
    EXPECT_EQ(index.current(firstKey), 1);
    EXPECT_EQ(index.current(secondKey), 0);
    EXPECT_EQ(priorPublication.publicationSequence, 1);
    ASSERT_EQ(priorPublication.changes.size(), 1);
    EXPECT_EQ(priorPublication.changes[0].key, firstKey);
    EXPECT_EQ(priorPublication.changes[0].revision, 1);
}

TEST(SharedPresentationRevisionIndexTest,
     exhaustedKeyRejectsMultiKeyPublicationAtomically)
{
    Gui::SharedPresentationRevisionIndex index(1);
    index.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    const Key exhausted {"stable-object", "Visibility"};
    const Key untouched {"stable-object", "Color"};
    const auto priorPublication = index.publish({exhausted});

    EXPECT_THROW(static_cast<void>(index.publish({exhausted, untouched})),
                 std::overflow_error);
    EXPECT_EQ(index.latestPublicationSequence(), 1);
    EXPECT_EQ(index.current(exhausted), 1);
    EXPECT_EQ(index.current(untouched), 0);
    EXPECT_EQ(priorPublication.publicationSequence, 1);
    ASSERT_EQ(priorPublication.changes.size(), 1);
    EXPECT_EQ(priorPublication.changes[0].key, exhausted);
    EXPECT_EQ(priorPublication.changes[0].revision, 1);
}

TEST(SharedPresentationRevisionIndexTest,
     failedAndAbandonedSavesNeverAdvancePersistedPresentationMarker)
{
    Gui::SharedPresentationRevisionIndex index;
    index.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    const auto initial = index.persistenceState();
    EXPECT_EQ(initial.document, testBinding());
    EXPECT_EQ(initial.currentPublicationSequence, 0);
    EXPECT_EQ(initial.persistedPublicationSequence, 0);
    EXPECT_FALSE(initial.hasUnpersistedChanges);

    static_cast<void>(index.publish({visibilityKey()}));
    const auto captured = index.capturePersistence();
    EXPECT_EQ(captured.document, testBinding());
    EXPECT_EQ(captured.publicationSequence, 1);
    EXPECT_EQ(index.markPersisted(
                  captured, Gui::SharedPresentationSaveDisposition::Failed),
              Gui::SharedPresentationPersistStatus::NotMarked);
    EXPECT_EQ(index.markPersisted(
                  captured, Gui::SharedPresentationSaveDisposition::Abandoned),
              Gui::SharedPresentationPersistStatus::NotMarked);
    auto state = index.persistenceState();
    EXPECT_EQ(state.persistedPublicationSequence, 0);
    EXPECT_TRUE(state.hasUnpersistedChanges);

    EXPECT_EQ(index.markPersisted(
                  captured, Gui::SharedPresentationSaveDisposition::Succeeded),
              Gui::SharedPresentationPersistStatus::Marked);
    state = index.persistenceState();
    EXPECT_EQ(state.currentPublicationSequence, 1);
    EXPECT_EQ(state.persistedPublicationSequence, 1);
    EXPECT_FALSE(state.hasUnpersistedChanges);
}

TEST(SharedPresentationRevisionIndexTest,
     persistedMarkerIsMonotonicConcurrentPublicationAwareAndEpochSafe)
{
    Gui::SharedPresentationRevisionIndex index;
    index.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    const auto key = visibilityKey();
    static_cast<void>(index.publish({key}));
    const auto firstSave = index.capturePersistence();
    static_cast<void>(index.publish({key}));

    EXPECT_EQ(index.markPersisted(
                  firstSave, Gui::SharedPresentationSaveDisposition::Succeeded),
              Gui::SharedPresentationPersistStatus::Marked);
    auto state = index.persistenceState();
    EXPECT_EQ(state.currentPublicationSequence, 2);
    EXPECT_EQ(state.persistedPublicationSequence, 1);
    EXPECT_TRUE(state.hasUnpersistedChanges);

    const auto secondSave = index.capturePersistence();
    EXPECT_EQ(index.markPersisted(
                  secondSave, Gui::SharedPresentationSaveDisposition::Succeeded),
              Gui::SharedPresentationPersistStatus::Marked);
    EXPECT_EQ(index.markPersisted(
                  firstSave, Gui::SharedPresentationSaveDisposition::Succeeded),
              Gui::SharedPresentationPersistStatus::Rewind);

    auto future = secondSave;
    ++future.publicationSequence;
    EXPECT_EQ(index.markPersisted(
                  future, Gui::SharedPresentationSaveDisposition::Succeeded),
              Gui::SharedPresentationPersistStatus::FutureSequence);
    auto foreign = secondSave;
    ++foreign.document.documentInstanceId;
    EXPECT_EQ(index.markPersisted(
                  foreign, Gui::SharedPresentationSaveDisposition::Succeeded),
              Gui::SharedPresentationPersistStatus::ForeignDocument);

    index.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch + 1);
    EXPECT_EQ(index.markPersisted(
                  secondSave, Gui::SharedPresentationSaveDisposition::Succeeded),
              Gui::SharedPresentationPersistStatus::EpochMismatch);
    state = index.persistenceState();
    EXPECT_EQ(state.document.lifecycleEpoch, TestDocumentEpoch + 1);
    EXPECT_EQ(state.currentPublicationSequence, 2);
    EXPECT_EQ(state.persistedPublicationSequence, 2);
    EXPECT_FALSE(state.hasUnpersistedChanges);

    const auto currentEpochSave = index.capturePersistence();
    EXPECT_EQ(index.markPersisted(
                  currentEpochSave, Gui::SharedPresentationSaveDisposition::Succeeded),
              Gui::SharedPresentationPersistStatus::Marked);
}

TEST(SharedPresentationCoordinatorTest,
     lifecycleMismatchRejectsBeforeMalformedRequestAndEveryDependencyCallback)
{
    CommitHarness harness;
    harness.request.presentationWrites.clear();
    harness.callbacks.currentIdentity = [&] {
        ++harness.lifecycleCalls;
        return std::optional<App::DocumentIdentity> {
            App::DocumentIdentity {TestDocumentInstance,
                                   TestDocumentEpoch + 1,
                                   App::DocumentLifecycleState::Live}};
    };

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::StaleDocument);
    EXPECT_EQ(harness.lifecycleCalls, 1);
    EXPECT_EQ(harness.appValidationCalls, 0);
    EXPECT_EQ(harness.appApplyCalls, 0);
    EXPECT_EQ(harness.guiApplyCalls, 0);
    EXPECT_EQ(harness.guiRollbackCalls, 0);
    EXPECT_EQ(harness.appRollbackCalls, 0);
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest,
     stalePresentationBindingPrecedesMalformedDeclaration)
{
    CommitHarness harness;
    ASSERT_EQ(harness.request.expectedPresentationRevisions.size(), 1);
    harness.request.expectedPresentationRevisions[0].document.lifecycleEpoch =
        TestDocumentEpoch + 1;
    harness.request.expectedPresentationRevisions[0].key.propertyName.clear();
    harness.request.presentationWrites.clear();

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::StaleDocument);
    EXPECT_EQ(harness.lifecycleCalls, 1);
    EXPECT_EQ(harness.appValidationCalls, 0);
    EXPECT_EQ(harness.appApplyCalls, 0);
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest,
     stalePresentationBindingPrecedesChangedAppDependency)
{
    CommitHarness harness;
    static_cast<void>(harness.app.publish(
        std::vector<App::DocumentRevisionKey> {harness.appWildcard}));
    ASSERT_EQ(harness.request.expectedPresentationRevisions.size(), 1);
    harness.request.expectedPresentationRevisions[0].document.documentInstanceId =
        TestDocumentInstance + 1;

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::StaleDocument);
    EXPECT_EQ(harness.lifecycleCalls, 1);
    EXPECT_EQ(harness.appValidationCalls, 0);
    EXPECT_EQ(harness.appApplyCalls, 0);
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest,
     zeroWorkInvocationIsAuthoritativeSerializationFailure)
{
    CommitHarness harness;
    harness.callbacks.serialize = [](Gui::SharedPresentationCommitWork&&,
                                     Gui::SharedPresentationCommitCompletion&& complete) {
        complete({false, "boundary did not admit work"});
    };

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::SerializationFailed);
    EXPECT_EQ(harness.lifecycleCalls, 0);
    EXPECT_EQ(harness.appApplyCalls, 0);
    EXPECT_EQ(harness.guiApplyCalls, 0);
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest,
     duplicateWorkInvocationCancelsReservationBeforeAnyLiveMutation)
{
    CommitHarness harness;
    harness.callbacks.serialize = [](Gui::SharedPresentationCommitWork&& work,
                                     Gui::SharedPresentationCommitCompletion&& complete) {
        work();
        work();
        complete({true, {}});
    };

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::SerializationFailed);
    EXPECT_EQ(harness.appApplyCalls, 0);
    EXPECT_EQ(harness.guiApplyCalls, 0);
    EXPECT_EQ(harness.postconditionCalls, 0);
    EXPECT_EQ(harness.guiRollbackCalls, 0);
    EXPECT_EQ(harness.appRollbackCalls, 0);
    EXPECT_EQ(harness.appState, 0);
    EXPECT_EQ(harness.guiState, 0);
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest,
     explicitBoundaryRejectionCancelsReservationWithoutLiveMutation)
{
    CommitHarness harness;
    harness.callbacks.serialize = [](Gui::SharedPresentationCommitWork&& work,
                                     Gui::SharedPresentationCommitCompletion&& complete) {
        work();
        complete({false, "injected App boundary rejection"});
    };

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::SerializationFailed);
    EXPECT_EQ(result.diagnostic, "injected App boundary rejection");
    EXPECT_EQ(harness.appApplyCalls, 0);
    EXPECT_EQ(harness.guiApplyCalls, 0);
    EXPECT_EQ(harness.guiRollbackCalls, 0);
    EXPECT_EQ(harness.appRollbackCalls, 0);
    EXPECT_EQ(harness.appState, 0);
    EXPECT_EQ(harness.guiState, 0);
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest,
     workThenReturnWithoutCompletionNeverMutatesOrRollsBackOutsideBoundary)
{
    CommitHarness harness;
    bool boundaryActive = false;
    bool rollbackOutsideBoundary = false;
    harness.callbacks.serialize = [&](Gui::SharedPresentationCommitWork&& work,
                                      Gui::SharedPresentationCommitCompletion&&) {
        boundaryActive = true;
        work();
        boundaryActive = false;
    };
    harness.callbacks.rollbackGuiMutation = [&] {
        rollbackOutsideBoundary = rollbackOutsideBoundary || !boundaryActive;
        ++harness.guiRollbackCalls;
        return succeeded();
    };
    harness.callbacks.rollbackAppMutation = [&] {
        rollbackOutsideBoundary = rollbackOutsideBoundary || !boundaryActive;
        ++harness.appRollbackCalls;
        return succeeded();
    };

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::SerializationFailed);
    EXPECT_EQ(harness.lifecycleCalls, 1);
    EXPECT_EQ(harness.appValidationCalls, 1);
    EXPECT_EQ(harness.appApplyCalls, 0);
    EXPECT_EQ(harness.guiApplyCalls, 0);
    EXPECT_EQ(harness.guiRollbackCalls, 0);
    EXPECT_EQ(harness.appRollbackCalls, 0);
    EXPECT_FALSE(rollbackOutsideBoundary);
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest,
     exceptionAfterWorkBeforeCompletionNeverMutatesOrRollsBackOutsideBoundary)
{
    CommitHarness harness;
    bool boundaryActive = false;
    bool rollbackOutsideBoundary = false;
    harness.callbacks.serialize = [&](Gui::SharedPresentationCommitWork&& work,
                                      Gui::SharedPresentationCommitCompletion&&) {
        boundaryActive = true;
        work();
        boundaryActive = false;
        throw std::runtime_error("boundary failed before completion");
    };
    harness.callbacks.rollbackGuiMutation = [&] {
        rollbackOutsideBoundary = rollbackOutsideBoundary || !boundaryActive;
        ++harness.guiRollbackCalls;
        return succeeded();
    };
    harness.callbacks.rollbackAppMutation = [&] {
        rollbackOutsideBoundary = rollbackOutsideBoundary || !boundaryActive;
        ++harness.appRollbackCalls;
        return succeeded();
    };

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::SerializationFailed);
    EXPECT_NE(result.diagnostic.find("boundary failed before completion"),
              std::string::npos);
    EXPECT_EQ(harness.appApplyCalls, 0);
    EXPECT_EQ(harness.guiApplyCalls, 0);
    EXPECT_EQ(harness.guiRollbackCalls, 0);
    EXPECT_EQ(harness.appRollbackCalls, 0);
    EXPECT_FALSE(rollbackOutsideBoundary);
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest,
     retainedCallbacksSafelyNoOpAfterCommitAndReferencedObjectsAreDestroyed)
{
    Gui::SharedPresentationCommitWork retainedWork;
    Gui::SharedPresentationCommitCompletion retainedCompletion;
    {
        CommitHarness harness;
        harness.callbacks.serialize = [&](Gui::SharedPresentationCommitWork&& work,
                                          Gui::SharedPresentationCommitCompletion&& complete) {
            retainedWork = work;
            retainedCompletion = complete;
        };

        const auto result = harness.commit();
        EXPECT_EQ(result.status, CommitStatus::SerializationFailed);
        expectNoPresentationPublication(harness);
    }

    ASSERT_TRUE(retainedWork);
    ASSERT_TRUE(retainedCompletion);
    EXPECT_NO_THROW(retainedWork());
    EXPECT_NO_THROW(retainedCompletion({true, {}}));
}

TEST(SharedPresentationCoordinatorTest,
     doubleCompletionKeepsFirstAuthorityAndPublishesExactlyOnce)
{
    CommitHarness harness;
    harness.callbacks.serialize = [](Gui::SharedPresentationCommitWork&& work,
                                     Gui::SharedPresentationCommitCompletion&& complete) {
        work();
        complete({true, {}});
        complete({false, "late rejection must not win"});
    };

    const auto result = harness.commit();

    ASSERT_TRUE(result.committed()) << result.diagnostic;
    ASSERT_TRUE(result.publishedPresentation.has_value());
    EXPECT_EQ(result.publishedPresentation->publicationSequence, 1);
    EXPECT_EQ(harness.presentation.latestPublicationSequence(), 1);
    EXPECT_EQ(harness.presentation.current(visibilityKey()), 1);
    EXPECT_EQ(harness.appApplyCalls, 1);
    EXPECT_EQ(harness.guiApplyCalls, 1);
    EXPECT_EQ(harness.postconditionCalls, 1);
    EXPECT_EQ(harness.guiRollbackCalls, 0);
    EXPECT_EQ(harness.appRollbackCalls, 0);
}

TEST(SharedPresentationCoordinatorTest,
     exceptionAfterAuthoritativeCompletionCannotReportPublishedCommitAsFailure)
{
    CommitHarness harness;
    harness.callbacks.serialize = [](Gui::SharedPresentationCommitWork&& work,
                                     Gui::SharedPresentationCommitCompletion&& complete) {
        work();
        complete({true, {}});
        throw std::runtime_error("late boundary exception");
    };

    const auto result = harness.commit();

    ASSERT_TRUE(result.committed()) << result.diagnostic;
    ASSERT_TRUE(result.publishedPresentation.has_value());
    EXPECT_EQ(result.publishedPresentation->publicationSequence, 1);
    EXPECT_EQ(harness.presentation.current(visibilityKey()), 1);
    EXPECT_EQ(harness.guiRollbackCalls, 0);
    EXPECT_EQ(harness.appRollbackCalls, 0);
    EXPECT_NE(result.diagnostic.find("late boundary exception"), std::string::npos);
}

TEST(SharedPresentationCoordinatorTest,
     serializationBoundaryRemainsActiveThroughApplyPostconditionAndCompletion)
{
    CommitHarness harness;
    bool boundaryActive = false;
    bool everyStageObservedBoundary = true;
    harness.callbacks.serialize = [&](Gui::SharedPresentationCommitWork&& work,
                                      Gui::SharedPresentationCommitCompletion&& complete) {
        boundaryActive = true;
        work();
        complete({true, {}});
        boundaryActive = false;
    };
    harness.callbacks.currentIdentity = [&] {
        everyStageObservedBoundary = everyStageObservedBoundary && boundaryActive;
        return std::optional<App::DocumentIdentity> {liveIdentity()};
    };
    harness.callbacks.validateAppRevisions = [&](const auto& observations) {
        everyStageObservedBoundary = everyStageObservedBoundary && boundaryActive;
        return harness.app.validate(observations);
    };
    harness.callbacks.applyAppMutation = [&] {
        everyStageObservedBoundary = everyStageObservedBoundary && boundaryActive;
        ++harness.appApplyCalls;
        return succeeded();
    };
    harness.callbacks.applyGuiMutation = [&] {
        everyStageObservedBoundary = everyStageObservedBoundary && boundaryActive;
        ++harness.guiApplyCalls;
        return succeeded();
    };
    harness.callbacks.checkPostcondition = [&] {
        everyStageObservedBoundary = everyStageObservedBoundary && boundaryActive;
        ++harness.postconditionCalls;
        return succeeded();
    };

    const auto result = harness.commit();

    ASSERT_TRUE(result.committed()) << result.diagnostic;
    EXPECT_TRUE(everyStageObservedBoundary);
    EXPECT_FALSE(boundaryActive);
    ASSERT_TRUE(result.publishedPresentation.has_value());
    EXPECT_EQ(result.publishedPresentation->publicationSequence, 1);
}

TEST(SharedPresentationCoordinatorTest, changedAppDependencyRejectsBeforePresentationOrMutation)
{
    CommitHarness harness;
    static_cast<void>(harness.app.publish(
        std::vector<App::DocumentRevisionKey> {harness.appWildcard}));

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::AppConflict);
    ASSERT_EQ(result.appConflicts.size(), 1);
    EXPECT_EQ(result.appConflicts[0].key, harness.appWildcard);
    EXPECT_EQ(harness.appApplyCalls, 0);
    EXPECT_EQ(harness.guiApplyCalls, 0);
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest, changedPresentationDependencyRejectsWithoutMutation)
{
    CommitHarness harness;
    static_cast<void>(harness.presentation.publish({visibilityKey()}));

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::PresentationConflict);
    ASSERT_EQ(result.presentationConflicts.size(), 1);
    EXPECT_EQ(result.presentationConflicts[0].key, visibilityKey());
    EXPECT_EQ(harness.appApplyCalls, 0);
    EXPECT_EQ(harness.guiApplyCalls, 0);
    EXPECT_EQ(harness.presentation.latestPublicationSequence(), 1);
    EXPECT_EQ(harness.presentation.current(visibilityKey()), 1);
}

TEST(SharedPresentationCoordinatorTest, AppExceptionRestoresBothDomainsAndPublishesNothing)
{
    CommitHarness harness;
    harness.callbacks.applyAppMutation = [&]() -> StepResult {
        ++harness.appApplyCalls;
        harness.appBefore = harness.appState;
        harness.appState = 91;
        harness.trace.push_back("apply-app");
        throw std::runtime_error("injected App failure");
    };

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::AppApplyFailed);
    EXPECT_NE(result.diagnostic.find("injected App failure"), std::string::npos);
    EXPECT_EQ(harness.appState, 0);
    EXPECT_EQ(harness.guiState, 0);
    EXPECT_EQ(harness.guiApplyCalls, 0);
    EXPECT_EQ(harness.guiRollbackCalls, 1);
    EXPECT_EQ(harness.appRollbackCalls, 1);
    EXPECT_EQ(harness.trace,
              (std::vector<std::string> {"apply-app", "rollback-gui", "rollback-app"}));
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest, GuiFailureRestoresGuiThenAppAndPublishesNothing)
{
    CommitHarness harness;
    harness.callbacks.applyGuiMutation = [&] {
        ++harness.guiApplyCalls;
        harness.guiBefore = harness.guiState;
        harness.guiState = 37;
        harness.trace.push_back("apply-gui");
        return failed("injected GUI failure");
    };

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::GuiApplyFailed);
    EXPECT_EQ(result.diagnostic, "injected GUI failure");
    EXPECT_EQ(harness.appState, 0);
    EXPECT_EQ(harness.guiState, 0);
    EXPECT_EQ(harness.guiRollbackCalls, 1);
    EXPECT_EQ(harness.appRollbackCalls, 1);
    EXPECT_EQ(harness.trace,
              (std::vector<std::string> {
                  "apply-app", "apply-gui", "rollback-gui", "rollback-app"}));
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest, falsePostconditionRestoresBothDomainsAndPublishesNothing)
{
    CommitHarness harness;
    harness.callbacks.checkPostcondition = [&] {
        ++harness.postconditionCalls;
        harness.trace.push_back("postcondition");
        return failed("injected postcondition failure");
    };

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::PostconditionFailed);
    EXPECT_EQ(result.diagnostic, "injected postcondition failure");
    EXPECT_EQ(harness.appState, 0);
    EXPECT_EQ(harness.guiState, 0);
    EXPECT_EQ(harness.trace,
              (std::vector<std::string> {"apply-app",
                                         "apply-gui",
                                         "postcondition",
                                         "rollback-gui",
                                         "rollback-app"}));
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest,
     AppDurabilityFailureRestoresBothDomainsAndPublishesNothing)
{
    CommitHarness harness;
    harness.callbacks.makeAppDurable = [&] {
        ++harness.appDurabilityCalls;
        harness.trace.push_back("commit-app");
        return failed("injected native App commit failure");
    };

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::AppCommitFailed);
    EXPECT_EQ(result.diagnostic, "injected native App commit failure");
    EXPECT_EQ(harness.appState, 0);
    EXPECT_EQ(harness.guiState, 0);
    EXPECT_EQ(harness.trace,
              (std::vector<std::string> {"apply-app",
                                         "apply-gui",
                                         "postcondition",
                                         "commit-app",
                                         "rollback-gui",
                                         "rollback-app"}));
    expectNoPresentationPublication(harness);
}

TEST(SharedPresentationCoordinatorTest,
     incompleteRollbackPoisonsStreamAndRejectsEveryLaterAdmission)
{
    CommitHarness harness;
    const auto saveCapture = harness.presentation.capturePersistence();
    harness.callbacks.applyAppMutation = [&] {
        ++harness.appApplyCalls;
        harness.appState = 13;
        return failed("primary App apply failure");
    };
    harness.callbacks.rollbackGuiMutation = [&]() -> StepResult {
        ++harness.guiRollbackCalls;
        harness.guiState = 0;
        throw std::runtime_error("GUI rollback exception");
    };
    harness.callbacks.rollbackAppMutation = [&] {
        ++harness.appRollbackCalls;
        return failed("App rollback refusal");
    };

    const auto result = harness.commit();

    EXPECT_EQ(result.status, CommitStatus::RollbackFailed);
    EXPECT_NE(result.diagnostic.find("primary App apply failure"), std::string::npos);
    ASSERT_EQ(result.rollbackDiagnostics.size(), 2);
    EXPECT_NE(result.rollbackDiagnostics[0].find("GUI rollback exception"),
              std::string::npos);
    EXPECT_NE(result.rollbackDiagnostics[1].find("App rollback refusal"),
              std::string::npos);
    EXPECT_EQ(harness.appState, 13);
    EXPECT_EQ(harness.guiState, 0);
    EXPECT_TRUE(harness.presentation.poisoned());
    EXPECT_TRUE(harness.presentation.persistenceState().poisoned);
    EXPECT_TRUE(harness.presentation.persistenceState().hasUnpersistedChanges);
    EXPECT_EQ(harness.presentation.validate(
                  harness.request.expectedPresentationRevisions).status,
              Gui::SharedPresentationValidationStatus::Poisoned);
    EXPECT_EQ(harness.presentation.markPersisted(
                  saveCapture, Gui::SharedPresentationSaveDisposition::Succeeded),
              Gui::SharedPresentationPersistStatus::Poisoned);
    EXPECT_THROW(static_cast<void>(harness.presentation.capturePersistence()),
                 std::logic_error);
    expectNoPresentationPublication(harness);

    const auto later = harness.commit();
    EXPECT_EQ(later.status, CommitStatus::RollbackFailed);
    EXPECT_EQ(harness.appValidationCalls, 1);
    EXPECT_EQ(harness.appApplyCalls, 1);
    EXPECT_EQ(harness.guiApplyCalls, 0);
    EXPECT_THROW(static_cast<void>(
                     harness.presentation.publish({visibilityKey()})),
                 std::logic_error);
    EXPECT_EQ(harness.presentation.latestPublicationSequence(), 0);
    EXPECT_EQ(harness.presentation.current(visibilityKey()), 0);
}

TEST(SharedPresentationCoordinatorTest,
     unpublishedOrdinaryMutationCanNoexceptPoisonEveryFuturePresentationPath)
{
    static_assert(noexcept(
        std::declval<Gui::SharedPresentationRevisionIndex&>()
            .markInconsistentAfterUnpublishedMutation()));

    CommitHarness harness;
    const auto observations = harness.request.expectedPresentationRevisions;
    const auto saveCapture = harness.presentation.capturePersistence();

    // Model Gui::Document's ordinary path: the live ViewProvider changed, but
    // the following revision publication threw before becoming authoritative.
    harness.guiState = 29;
    harness.presentation.markInconsistentAfterUnpublishedMutation();

    EXPECT_TRUE(harness.presentation.poisoned());
    EXPECT_EQ(harness.presentation.validate(observations).status,
              Gui::SharedPresentationValidationStatus::Poisoned);
    const auto coordinated = harness.commit();
    EXPECT_EQ(coordinated.status, CommitStatus::RollbackFailed);
    EXPECT_EQ(harness.appApplyCalls, 0);
    EXPECT_EQ(harness.guiApplyCalls, 0);
    EXPECT_EQ(harness.guiState, 29);
    EXPECT_THROW(static_cast<void>(
                     harness.presentation.publish({visibilityKey()})),
                 std::logic_error);
    EXPECT_EQ(harness.presentation.markPersisted(
                  saveCapture, Gui::SharedPresentationSaveDisposition::Succeeded),
              Gui::SharedPresentationPersistStatus::Poisoned);
    EXPECT_THROW(static_cast<void>(harness.presentation.capturePersistence()),
                 std::logic_error);
    EXPECT_TRUE(harness.presentation.persistenceState().hasUnpersistedChanges);
    EXPECT_EQ(harness.presentation.latestPublicationSequence(), 0);
    EXPECT_EQ(harness.presentation.current(visibilityKey()), 0);
}

TEST(SharedPresentationCoordinatorTest,
     successIsExactOnceMonotonicAndDoesNotAdvanceAppWildcard)
{
    CommitHarness harness;
    const auto wildcardBefore = harness.app.current(harness.appWildcard);

    const auto first = harness.commit();
    ASSERT_TRUE(first.committed()) << first.diagnostic;
    ASSERT_TRUE(first.publishedPresentation.has_value());
    EXPECT_EQ(first.publishedPresentation->publicationSequence, 1);
    ASSERT_EQ(first.publishedPresentation->changes.size(), 1);
    EXPECT_EQ(first.publishedPresentation->changes[0].revision, 1);
    EXPECT_EQ(harness.appApplyCalls, 1);
    EXPECT_EQ(harness.guiApplyCalls, 1);
    EXPECT_EQ(harness.postconditionCalls, 1);
    EXPECT_EQ(harness.appRollbackCalls, 0);
    EXPECT_EQ(harness.guiRollbackCalls, 0);
    EXPECT_EQ(harness.app.current(harness.appWildcard), wildcardBefore);

    harness.request.expectedPresentationRevisions =
        harness.presentation.capture({visibilityKey()});
    const auto second = harness.commit();
    ASSERT_TRUE(second.committed()) << second.diagnostic;
    ASSERT_TRUE(second.publishedPresentation.has_value());
    EXPECT_EQ(second.publishedPresentation->publicationSequence, 2);
    ASSERT_EQ(second.publishedPresentation->changes.size(), 1);
    EXPECT_EQ(second.publishedPresentation->changes[0].revision, 2);
    EXPECT_EQ(harness.presentation.current(visibilityKey()), 2);
    EXPECT_EQ(harness.appApplyCalls, 2);
    EXPECT_EQ(harness.guiApplyCalls, 2);
    EXPECT_EQ(harness.postconditionCalls, 2);
    EXPECT_EQ(harness.app.current(harness.appWildcard), wildcardBefore);
}

TEST(SharedPresentationCoordinatorTest,
     suppliedAppBoundarySerializesCommitsWithoutRevisionMutexMasking)
{
    // Separate revision stores deliberately remove their internal mutex as a
    // possible source of serialization.  The shared App boundary is therefore
    // the only lock capable of keeping the live callbacks disjoint.
    Gui::SharedPresentationRevisionIndex firstPresentation;
    Gui::SharedPresentationRevisionIndex secondPresentation;
    firstPresentation.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    secondPresentation.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    Gui::SharedPresentationCoordinator coordinator;
    const Key firstKey {"stable-object", "Visibility"};
    const Key secondKey {"stable-object", "Color"};
    auto firstRequest = requestFor(firstPresentation, firstKey);
    auto secondRequest = requestFor(secondPresentation, secondKey);

    std::mutex startMutex;
    std::condition_variable startCondition;
    int boundaryEntrants = 0;
    std::mutex appSerializationMutex;
    std::atomic<int> activeMutations {0};
    std::atomic<int> maximumActiveMutations {0};
    std::atomic<int> applyCalls {0};

    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.serialize = [&](Gui::SharedPresentationCommitWork&& work,
                              Gui::SharedPresentationCommitCompletion&& complete) {
        {
            std::unique_lock<std::mutex> startLock(startMutex);
            ++boundaryEntrants;
            startCondition.notify_all();
            startCondition.wait(startLock, [&] { return boundaryEntrants == 2; });
        }
        std::lock_guard<std::mutex> serialized(appSerializationMutex);
        work();
        complete({true, {}});
    };
    callbacks.currentIdentity = [] {
        return std::optional<App::DocumentIdentity> {liveIdentity()};
    };
    callbacks.validateAppRevisions = [](const auto&) {
        return std::vector<App::DocumentRevisionConflict> {};
    };
    callbacks.applyAppMutation = [&] {
        const int active = activeMutations.fetch_add(1) + 1;
        int previousMaximum = maximumActiveMutations.load();
        while (active > previousMaximum
               && !maximumActiveMutations.compare_exchange_weak(previousMaximum, active)) {}
        ++applyCalls;
        std::this_thread::yield();
        return succeeded();
    };
    callbacks.applyGuiMutation = [&] {
        activeMutations.fetch_sub(1);
        return succeeded();
    };
    callbacks.checkPostcondition = succeeded;
    callbacks.makeAppDurable = succeeded;
    callbacks.rollbackGuiMutation = succeeded;
    callbacks.rollbackAppMutation = [&] {
        if (activeMutations.load() != 0) {
            activeMutations.fetch_sub(1);
        }
        return succeeded();
    };

    Gui::SharedPresentationCommitResult firstResult;
    Gui::SharedPresentationCommitResult secondResult;
    std::thread first([&] {
        firstResult = coordinator.commit(firstPresentation, firstRequest, callbacks);
    });
    std::thread second([&] {
        secondResult = coordinator.commit(secondPresentation, secondRequest, callbacks);
    });
    first.join();
    second.join();

    EXPECT_TRUE(firstResult.committed()) << firstResult.diagnostic;
    EXPECT_TRUE(secondResult.committed()) << secondResult.diagnostic;
    EXPECT_EQ(applyCalls.load(), 2);
    EXPECT_EQ(maximumActiveMutations.load(), 1);
    EXPECT_EQ(activeMutations.load(), 0);
    EXPECT_EQ(firstPresentation.current(firstKey), 1);
    EXPECT_EQ(secondPresentation.current(secondKey), 1);
    EXPECT_EQ(firstPresentation.latestPublicationSequence(), 1);
    EXPECT_EQ(secondPresentation.latestPublicationSequence(), 1);
}

TEST(SharedPresentationCoordinatorTest,
     publicationReservationFailureRunsNoMutationAndPublishesNothing)
{
    Gui::SharedPresentationRevisionIndex presentation(0);
    presentation.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    Gui::SharedPresentationCoordinator coordinator;
    const auto request = requestFor(presentation, visibilityKey());
    int mutationCalls = 0;

    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.serialize = [](Gui::SharedPresentationCommitWork&& work,
                             Gui::SharedPresentationCommitCompletion&& complete) {
        work();
        complete({true, {}});
    };
    callbacks.currentIdentity = [] {
        return std::optional<App::DocumentIdentity> {liveIdentity()};
    };
    callbacks.validateAppRevisions = [](const auto&) {
        return std::vector<App::DocumentRevisionConflict> {};
    };
    callbacks.applyAppMutation = [&] {
        ++mutationCalls;
        return succeeded();
    };
    callbacks.applyGuiMutation = callbacks.applyAppMutation;
    callbacks.checkPostcondition = succeeded;
    callbacks.makeAppDurable = succeeded;
    callbacks.rollbackGuiMutation = succeeded;
    callbacks.rollbackAppMutation = succeeded;

    const auto result = coordinator.commit(presentation, request, callbacks);

    EXPECT_EQ(result.status, CommitStatus::PresentationPublicationFailed);
    EXPECT_EQ(mutationCalls, 0);
    EXPECT_FALSE(result.publishedPresentation.has_value());
    EXPECT_EQ(presentation.latestPublicationSequence(), 0);
    EXPECT_EQ(presentation.current(visibilityKey()), 0);
}
