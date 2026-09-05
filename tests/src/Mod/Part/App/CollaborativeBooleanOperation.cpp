// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/FeatureTest.h>
#include <App/GeometryWorkerOperationRegistry.h>
#include <App/PropertyLinks.h>
#include <Base/Placement.h>
#include <Mod/Part/App/CollaborativeBooleanOperation.h>
#include <Mod/Part/App/FeaturePartBox.h>
#include <Mod/Part/App/PartFeature.h>
#include <src/App/InitApplication.h>

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;

double volume(const TopoDS_Shape& shape)
{
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return properties.Mass();
}

App::CollaborativeOperationIntent booleanIntent(std::string kind)
{
    return {std::string(Part::CollaborativeBooleanOperationType),
            {{"base", "Base"},
             {"tool", "Tool"},
             {"result", "Result"},
             {"kind", std::move(kind)}}};
}

bool containsKey(const std::vector<App::DocumentRevisionKey>& keys,
                 const App::DocumentRevisionKey& expected)
{
    return std::ranges::find(keys, expected) != keys.end();
}

class CollaborativeBooleanOperationTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        Part::ensureCollaborativeBooleanOperationRegistered();
    }

    void SetUp() override
    {
        _documentName = App::GetApplication().getUniqueDocumentName("collaborativeBoolean");
        _document = App::GetApplication().newDocument(_documentName.c_str(), "Boolean test");
        _base = _document->addObject<Part::Feature>("Base");
        _tool = _document->addObject<Part::Feature>("Tool");
        _result = _document->addObject<Part::Feature>("Result");
        _base->Shape.setValue(BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape());
        _tool->Shape.setValue(
            BRepPrimAPI_MakeBox(gp_Pnt(1.0, 0.0, 0.0), 2.0, 2.0, 2.0).Shape());
        _document->recompute();
        _session = _document->collaborationService().beginEditSession("part-boolean-test");
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_documentName.c_str());
    }

    App::CollaborativeOperationPreparation prepareAdapter(std::string kind)
    {
        return App::CollaborativeOperationRegistry::instance().prepare(
            *_document, booleanIntent(std::move(kind)));
    }

    App::PreparedEdit prepareEdit(std::string kind)
    {
        auto intent = booleanIntent(kind);
        const auto executionId = _document->collaborationService().prepareEditAsync(
            _session.sessionId(),
            "part-boolean-" + kind,
            intent,
            "native-part-test");
        waitUntilTerminal(executionId);
        auto terminal =
            _document->collaborationService().takePreparedEdit(_session.sessionId(),
                                                               executionId);
        if (!terminal || terminal->status != App::PreparedEditExecutionStatus::Completed
            || !terminal->preparedEdit) {
            throw std::runtime_error(
                terminal ? terminal->diagnostic : "detached Boolean did not complete");
        }
        return std::move(*terminal->preparedEdit);
    }

    void waitUntilTerminal(App::PreparedEditExecutionId executionId)
    {
        // A cold installed FreeCADCmd must load the Part module and OCC before
        // publishing its first result.  Keep the poll responsive but allow a
        // bounded startup window on slower native CI hosts.
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto status =
                _document->collaborationService().preparedEditStatus(executionId);
            if (status
                && (status->status == App::PreparedEditExecutionStatus::Completed
                    || status->status == App::PreparedEditExecutionStatus::Cancelled
                    || status->status == App::PreparedEditExecutionStatus::Failed)) {
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
        throw std::runtime_error("timed out waiting for detached Boolean preparation");
    }

    std::unique_ptr<const App::CollaborativeOperation>
    materialize(App::CollaborativeOperationPreparation& preparation)
    {
        if (preparation.policy != App::PreparationPolicy::IsolatedProcess
            || preparation.detachedTask || !preparation.isolatedTask) {
            throw std::runtime_error("Boolean adapter did not return isolated work");
        }
        auto& task = *preparation.isolatedTask;
        task.inputArchive.metadata.operationType = task.request.operationType;
        auto output = App::Internal::GeometryWorkerOperationRegistry::instance().execute(
            task.request.operationType, task.inputArchive, std::stop_token {});
        output.metadata = task.inputArchive.metadata;
        output.metadata.kind = App::GeometryArchiveKind::Result;
        return task.decodeResult(output);
    }

    std::string _documentName;
    App::Document* _document {nullptr};
    Part::Feature* _base {nullptr};
    Part::Feature* _tool {nullptr};
    Part::Feature* _result {nullptr};
    App::EditSession _session {"placeholder", "placeholder", 1};
};

class CollaborativeBooleanKindsTest:
    public CollaborativeBooleanOperationTest,
    public ::testing::WithParamInterface<std::pair<const char*, double>>
{};

}  // namespace

TEST_P(CollaborativeBooleanKindsTest, computesFromLiveShapesIntoPreExistingResult)
{
    auto prepared = prepareEdit(GetParam().first);
    EXPECT_FALSE(prepared.operation().checkPostcondition(*_document).satisfied);

    const auto commit =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);

    EXPECT_TRUE(commit.committed()) << commit.message;
    EXPECT_NEAR(volume(_result->Shape.getValue()), GetParam().second, 1e-7);
    EXPECT_TRUE(prepared.operation().checkPostcondition(*_document).satisfied);
    EXPECT_EQ(_document->getObjects().size(), 3U);
}

INSTANTIATE_TEST_SUITE_P(CutFuseCommon,
                         CollaborativeBooleanKindsTest,
                         ::testing::Values(std::pair {"cut", 4.0},
                                           std::pair {"fuse", 12.0},
                                           std::pair {"common", 4.0}));

TEST_F(CollaborativeBooleanOperationTest, registrationIsIdempotent)
{
    EXPECT_NO_THROW(Part::ensureCollaborativeBooleanOperationRegistered());
    EXPECT_TRUE(App::CollaborativeOperationRegistry::instance().contains(
        std::string(Part::CollaborativeBooleanOperationType)));
}

TEST_F(CollaborativeBooleanOperationTest, declaresCompleteConservativeDependenciesAndEffects)
{
    auto prepared = prepareEdit("cut");

    EXPECT_EQ(prepared.readSet(),
              (std::vector<App::DocumentRevisionKey> {
                  App::DocumentRevisionKey::objectExistence("Base"),
                  App::DocumentRevisionKey::objectExistence("Result"),
                  App::DocumentRevisionKey::objectExistence("Tool"),
                  App::DocumentRevisionKey::objectModel("Base"),
                  App::DocumentRevisionKey::objectModel("Result"),
                  App::DocumentRevisionKey::objectModel("Tool"),
                  App::DocumentRevisionKey::objectStructure("Base"),
                  App::DocumentRevisionKey::objectStructure("Result"),
                  App::DocumentRevisionKey::objectStructure("Tool"),
                  App::DocumentRevisionKey::documentStructure(),
                  App::DocumentRevisionKey::unknownModelMutation()}));
    EXPECT_EQ(prepared.writeSet(),
              (std::vector<App::DocumentRevisionKey> {
                  App::DocumentRevisionKey::objectModel("Result"),
                  App::DocumentRevisionKey::objectStructure("Result")}));

    ASSERT_EQ(prepared.publicationEffects().size(), prepared.writeSet().size());
    const auto resultIdentity = _document->collaborationObjectIdentity(*_result);
    for (const auto& effect : prepared.publicationEffects()) {
        EXPECT_TRUE(containsKey(prepared.writeSet(), effect.key));
        EXPECT_EQ(effect.stableObjectIdentity, resultIdentity);
    }
}

TEST_F(CollaborativeBooleanOperationTest, rejectsMalformedIntentAndInvalidInputShapes)
{
    auto unsupported = booleanIntent("section");
    EXPECT_THROW(static_cast<void>(App::CollaborativeOperationRegistry::instance().prepare(
                     *_document, unsupported)),
                 std::invalid_argument);

    auto incomplete = booleanIntent("cut");
    incomplete.arguments.erase("tool");
    EXPECT_THROW(static_cast<void>(App::CollaborativeOperationRegistry::instance().prepare(
                     *_document, incomplete)),
                 std::invalid_argument);

    _base->Shape.setValue(TopoDS_Shape {});
    EXPECT_THROW(static_cast<void>(prepareAdapter("cut")), std::invalid_argument);
}

TEST_F(CollaborativeBooleanOperationTest, rejectsResultFeatureSubclass)
{
    _document->removeObject("Result");
    _result = _document->addObject<Part::Box>("Result");

    EXPECT_THROW(static_cast<void>(prepareAdapter("cut")), std::invalid_argument);
}

TEST_F(CollaborativeBooleanOperationTest,
       noTouchInputLinkMutationConflictsThroughFrozenStructure)
{
    auto* dependency = dynamic_cast<App::PropertyLink*>(
        _base->addDynamicProperty("App::PropertyLink", "LateDependency"));
    ASSERT_NE(dependency, nullptr);
    _base->setStatus(App::ObjectStatus::NoTouch, true);
    _document->recompute();

    auto prepared = prepareEdit("cut");
    dependency->setValue(_tool);
    EXPECT_FALSE(_document->mustExecute())
        << "NoTouch fixture must exercise the no-pending-recompute admission path";

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    EXPECT_EQ(result.status, App::DocumentCommitStatus::Conflict);
    EXPECT_NE(std::ranges::find_if(result.conflicts, [](const auto& conflict) {
                  return conflict.key
                      == App::DocumentRevisionKey::objectStructure("Base");
              }),
              result.conflicts.end());
    EXPECT_TRUE(_result->Shape.getValue().IsNull());
}

TEST_F(CollaborativeBooleanOperationTest, rejectsResultToInputDependencyAtPrepareAndApply)
{
    auto* baseDependency = dynamic_cast<App::PropertyLink*>(
        _base->addDynamicProperty("App::PropertyLink", "DependsOnResult"));
    ASSERT_NE(baseDependency, nullptr);
    baseDependency->setValue(_result);
    EXPECT_THROW(static_cast<void>(prepareAdapter("cut")), std::invalid_argument);

    baseDependency->setValue(nullptr);
    ASSERT_TRUE(_base->removeDynamicProperty("DependsOnResult"));
    _document->recompute();
    auto preparation = prepareAdapter("cut");
    auto operation = materialize(preparation);
    auto* toolDependency = dynamic_cast<App::PropertyLink*>(
        _tool->addDynamicProperty("App::PropertyLink", "DependsOnResult"));
    ASSERT_NE(toolDependency, nullptr);
    toolDependency->setValue(_result);

    EXPECT_THROW(operation->apply(*_document), std::runtime_error);
    EXPECT_FALSE(operation->checkPostcondition(*_document).satisfied);
}

TEST_F(CollaborativeBooleanOperationTest, applyRejectsStaleObjectIdentity)
{
    auto preparation = prepareAdapter("cut");
    auto operation = materialize(preparation);
    const auto originalIdentity = _document->collaborationObjectIdentity(*_base);

    _document->removeObject("Base");
    _base = _document->addObject<Part::Feature>("Base");
    _base->Shape.setValue(BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape());
    ASSERT_NE(_document->collaborationObjectIdentity(*_base), originalIdentity);

    EXPECT_THROW(operation->apply(*_document), std::runtime_error);
}

TEST_F(CollaborativeBooleanOperationTest, applyUsesCapturedShapeAfterLiveInputBecomesInvalid)
{
    auto preparation = prepareAdapter("cut");
    auto operation = materialize(preparation);
    _tool->Shape.setValue(TopoDS_Shape {});

    EXPECT_NO_THROW(operation->apply(*_document));
    EXPECT_NEAR(volume(_result->Shape.getValue()), 4.0, 1e-7);
}

TEST_F(CollaborativeBooleanOperationTest, applyDoesNotRecalculateFromCurrentInputShapes)
{
    auto preparation = prepareAdapter("cut");
    auto operation = materialize(preparation);
    _tool->Placement.setValue(
        Base::Placement(Base::Vector3d(3.0, 0.0, 0.0), Base::Rotation()));

    operation->apply(*_document);

    EXPECT_NEAR(volume(_result->Shape.getValue()), 4.0, 1e-7);
}

TEST_F(CollaborativeBooleanOperationTest, preparedEditConflictsAfterInputModelMutation)
{
    auto prepared = prepareEdit("cut");
    _base->Shape.setValue(BRepPrimAPI_MakeBox(3.0, 2.0, 2.0).Shape());

    const auto commit =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);

    EXPECT_EQ(commit.status, App::DocumentCommitStatus::Conflict);
    EXPECT_TRUE(_result->Shape.getValue().IsNull());
}

TEST_F(CollaborativeBooleanOperationTest,
       freezesAndPublishesDownstreamDependentModelWithIdentity)
{
    auto* dependent = _document->addObject<App::FeatureTest>("Dependent");
    ASSERT_NE(dependent, nullptr);
    dependent->Source1.setValue(_result);
    _document->recompute();

    const auto dependentModel =
        App::DocumentRevisionKey::objectModel("Dependent");
    const auto dependentModelBefore =
        _document->collaborationRevisions().current(dependentModel);
    const auto identity = _document->collaborationIdentity();
    const auto journal = _document->collaborationRevisions().pollPublications(
        {identity.instanceId, identity.lifecycleEpoch, 0}, 0);
    const App::DocumentRevisionCursor cursor {
        identity.instanceId, identity.lifecycleEpoch, journal.latestSequence};

    auto prepared = prepareEdit("fuse");
    EXPECT_TRUE(containsKey(prepared.readSet(),
                            App::DocumentRevisionKey::objectExistence("Dependent")));
    EXPECT_TRUE(containsKey(prepared.readSet(), dependentModel));
    EXPECT_TRUE(containsKey(prepared.readSet(),
                            App::DocumentRevisionKey::objectStructure("Dependent")));
    EXPECT_TRUE(containsKey(prepared.writeSet(), dependentModel));
    const auto dependentEffect = std::ranges::find_if(
        prepared.publicationEffects(),
        [&](const auto& effect) { return effect.key == dependentModel; });
    ASSERT_NE(dependentEffect, prepared.publicationEffects().end());
    EXPECT_EQ(dependentEffect->stableObjectIdentity,
              std::optional<std::string>(
                  _document->collaborationObjectIdentity(*dependent)));

    const auto commit =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);

    ASSERT_TRUE(commit.committed()) << commit.message;
    EXPECT_EQ(_document->collaborationRevisions().current(dependentModel),
              dependentModelBefore + 1);
    const auto publications =
        _document->collaborationRevisions().pollPublications(cursor);
    ASSERT_EQ(publications.events.size(), 1U);
    const auto dependentChange = std::ranges::find_if(
        publications.events.front().changes,
        [&](const auto& change) { return change.key == dependentModel; });
    ASSERT_NE(dependentChange, publications.events.front().changes.end());
    EXPECT_EQ(dependentChange->stableObjectIdentity,
              std::optional<std::string>(
                  _document->collaborationObjectIdentity(*dependent)));
}

TEST_F(CollaborativeBooleanOperationTest,
       failedBooleanCommitPreservesSentinelWithoutPublication)
{
    const TopoDS_Shape sentinel = BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape();
    _result->Shape.setValue(sentinel);
    _tool->Shape.setValue(
        BRepPrimAPI_MakeBox(gp_Pnt(4.0, 0.0, 0.0), 1.0, 1.0, 1.0).Shape());
    _document->recompute();
    const auto resultModel = App::DocumentRevisionKey::objectModel("Result");
    const auto resultStructure =
        App::DocumentRevisionKey::objectStructure("Result");
    const auto modelBefore =
        _document->collaborationRevisions().current(resultModel);
    const auto structureBefore =
        _document->collaborationRevisions().current(resultStructure);
    auto intent = booleanIntent("common");
    const auto executionId = _document->collaborationService().prepareEditAsync(
        _session.sessionId(), "part-boolean-common", intent, "native-part-test");
    waitUntilTerminal(executionId);
    auto terminal =
        _document->collaborationService().takePreparedEdit(_session.sessionId(), executionId);

    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->status, App::PreparedEditExecutionStatus::Failed);
    EXPECT_EQ(terminal->preparedEdit, nullptr);
    EXPECT_FALSE(_result->Shape.getValue().IsNull());
    EXPECT_NEAR(volume(_result->Shape.getValue()), 1.0, 1e-7);
    EXPECT_EQ(_document->collaborationRevisions().current(resultModel), modelBefore);
    EXPECT_EQ(_document->collaborationRevisions().current(resultStructure),
              structureBefore);
}

TEST_F(CollaborativeBooleanOperationTest, postconditionRejectsStaleOrInvalidResult)
{
    auto invalidShape = prepareAdapter("cut");
    auto invalidOperation = materialize(invalidShape);
    invalidOperation->apply(*_document);
    _result->Shape.setValue(TopoDS_Shape {});
    EXPECT_FALSE(invalidOperation->checkPostcondition(*_document).satisfied);

    auto staleIdentity = prepareAdapter("fuse");
    auto staleOperation = materialize(staleIdentity);
    _document->removeObject("Result");
    _result = _document->addObject<Part::Feature>("Result");
    _result->Shape.setValue(BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape());
    EXPECT_FALSE(staleOperation->checkPostcondition(*_document).satisfied);
}
