// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/GeometryWorkerOperationRegistry.h>
#include <Mod/Part/App/CollaborativeSweepFilletOperations.h>
#include <Mod/Part/App/PartFeature.h>
#include <src/App/InitApplication.h>

#include <BRepCheck_Analyzer.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <chrono>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;

App::CollaborativeOperationIntent sweepIntent()
{
    return {std::string(Part::CollaborativeSweepOperationType),
            {{"spine", "Spine"},
             {"sections", "Section"},
             {"result", "SweepResult"},
             {"solid", "true"},
             {"frenet", "false"},
             {"transition", "0"},
             {"linearize", "false"}}};
}

App::CollaborativeOperationIntent filletIntent()
{
    return {std::string(Part::CollaborativeFilletOperationType),
            {{"base", "Box"}, {"result", "FilletResult"}, {"edges", "1:0.25:0.25"}}};
}

App::GeometryArchiveSection& requireSection(App::GeometryArchive& archive,
                                            const std::string& name)
{
    const auto found = std::ranges::find_if(archive.sections, [&](const auto& section) {
        return section.name == name;
    });
    if (found == archive.sections.end()) {
        throw std::runtime_error("test archive has no section named " + name);
    }
    return *found;
}

App::GeometryArchive executeArchive(
    App::CollaborativeOperationPreparation& preparation,
    const std::stop_token stopToken = {})
{
    if (preparation.policy != App::PreparationPolicy::IsolatedProcess
        || preparation.detachedTask || !preparation.isolatedTask) {
        throw std::runtime_error("Part adapter did not return isolated work");
    }
    auto& task = *preparation.isolatedTask;
    task.inputArchive.metadata.kind = App::GeometryArchiveKind::Request;
    task.inputArchive.metadata.operationType = task.request.operationType;
    auto output = App::Internal::GeometryWorkerOperationRegistry::instance().execute(
        task.request.operationType, task.inputArchive, stopToken);
    output.metadata = task.inputArchive.metadata;
    output.metadata.kind = App::GeometryArchiveKind::Result;
    return output;
}

std::unique_ptr<const App::CollaborativeOperation> decodeArchive(
    App::CollaborativeOperationPreparation& preparation,
    const App::GeometryArchive& output)
{
    if (!preparation.isolatedTask
        || !preparation.isolatedTask->decodeResult) {
        throw std::runtime_error("Part adapter has no trusted result decoder");
    }
    return preparation.isolatedTask->decodeResult(output);
}

std::unique_ptr<const App::CollaborativeOperation> materialize(
    App::CollaborativeOperationPreparation& preparation,
    const std::stop_token stopToken = {})
{
    auto output = executeArchive(preparation, stopToken);
    return decodeArchive(preparation, output);
}

std::vector<App::DocumentRevisionKey> expectedReads(
    const std::vector<std::string>& inputs,
    const std::string& result)
{
    std::vector<App::DocumentRevisionKey> keys {
        App::DocumentRevisionKey::documentStructure(),
        App::DocumentRevisionKey::unknownModelMutation(),
        App::DocumentRevisionKey::objectExistence(result),
        App::DocumentRevisionKey::objectModel(result),
        App::DocumentRevisionKey::objectStructure(result)};
    for (const auto& input : inputs) {
        keys.push_back(App::DocumentRevisionKey::objectExistence(input));
        keys.push_back(App::DocumentRevisionKey::objectModel(input));
        keys.push_back(App::DocumentRevisionKey::objectStructure(input));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

std::vector<App::DocumentRevisionKey> expectedWrites(const std::string& result)
{
    std::vector<App::DocumentRevisionKey> keys {
        App::DocumentRevisionKey::objectModel(result),
        App::DocumentRevisionKey::objectStructure(result)};
    std::sort(keys.begin(), keys.end());
    return keys;
}

class CollaborativeSweepFilletOperationsTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        Part::ensureCollaborativeSweepFilletOperationsRegistered();
    }

    void SetUp() override
    {
        _documentName =
            App::GetApplication().getUniqueDocumentName("collaborativeSweepFillet");
        _document = App::GetApplication().newDocument(_documentName.c_str(),
                                                       "Sweep and Fillet test");
        _spine = _document->addObject<Part::Feature>("Spine");
        _section = _document->addObject<Part::Feature>("Section");
        _sweepResult = _document->addObject<Part::Feature>("SweepResult");
        _box = _document->addObject<Part::Feature>("Box");
        _filletResult = _document->addObject<Part::Feature>("FilletResult");

        BRepBuilderAPI_MakePolygon spine;
        spine.Add(gp_Pnt(0.0, 0.0, 0.0));
        spine.Add(gp_Pnt(0.0, 0.0, 5.0));
        ASSERT_TRUE(spine.IsDone());
        _spine->Shape.setValue(spine.Wire());

        BRepBuilderAPI_MakePolygon section;
        section.Add(gp_Pnt(-0.5, -0.5, 0.0));
        section.Add(gp_Pnt(0.5, -0.5, 0.0));
        section.Add(gp_Pnt(0.5, 0.5, 0.0));
        section.Add(gp_Pnt(-0.5, 0.5, 0.0));
        section.Close();
        ASSERT_TRUE(section.IsDone());
        _section->Shape.setValue(section.Wire());
        _box->Shape.setValue(BRepPrimAPI_MakeBox(4.0, 4.0, 4.0).Shape());
        _document->recompute();
        _session =
            _document->collaborationService().beginEditSession("part-sweep-fillet-test");
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_documentName.c_str());
    }

    App::CollaborativeOperationPreparation prepare(
        const App::CollaborativeOperationIntent& intent)
    {
        return App::CollaborativeOperationRegistry::instance().prepare(*_document, intent);
    }

    App::PreparedEdit prepareThroughProcess(
        const App::CollaborativeOperationIntent& intent,
        const std::string& operationId)
    {
        const auto executionId = _document->collaborationService().prepareEditAsync(
            _session.sessionId(), operationId, intent, "native-part-test");
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto status =
                _document->collaborationService().preparedEditStatus(executionId);
            if (status
                && (status->status == App::PreparedEditExecutionStatus::Completed
                    || status->status == App::PreparedEditExecutionStatus::Cancelled
                    || status->status == App::PreparedEditExecutionStatus::Failed)) {
                auto terminal = _document->collaborationService().takePreparedEdit(
                    _session.sessionId(), executionId);
                if (terminal
                    && terminal->status == App::PreparedEditExecutionStatus::Completed
                    && terminal->preparedEdit) {
                    return std::move(*terminal->preparedEdit);
                }
                throw std::runtime_error(
                    terminal ? terminal->diagnostic
                             : "isolated Part operation produced no terminal result");
            }
            std::this_thread::sleep_for(1ms);
        }
        static_cast<void>(
            _document->collaborationService().cancelPreparedEdit(executionId));
        throw std::runtime_error("timed out waiting for isolated Part worker");
    }

    std::string _documentName;
    App::Document* _document {nullptr};
    Part::Feature* _spine {nullptr};
    Part::Feature* _section {nullptr};
    Part::Feature* _sweepResult {nullptr};
    Part::Feature* _box {nullptr};
    Part::Feature* _filletResult {nullptr};
    App::EditSession _session {"placeholder", "placeholder", 1};
};

}  // namespace

TEST_F(CollaborativeSweepFilletOperationsTest, registrationIsIdempotent)
{
    EXPECT_NO_THROW(Part::ensureCollaborativeSweepFilletOperationsRegistered());
    EXPECT_TRUE(App::CollaborativeOperationRegistry::instance().contains(
        std::string(Part::CollaborativeSweepOperationType)));
    EXPECT_TRUE(App::CollaborativeOperationRegistry::instance().contains(
        std::string(Part::CollaborativeFilletOperationType)));
}

TEST_F(CollaborativeSweepFilletOperationsTest, rejectsNonExactSweepAndFilletIntents)
{
    auto missingSweep = sweepIntent();
    missingSweep.arguments.erase("sections");
    EXPECT_THROW(static_cast<void>(prepare(missingSweep)), std::invalid_argument);

    auto extraSweep = sweepIntent();
    extraSweep.arguments.emplace("unexpected", "true");
    EXPECT_THROW(static_cast<void>(prepare(extraSweep)), std::invalid_argument);

    auto invalidSweep = sweepIntent();
    invalidSweep.arguments["transition"] = "3";
    EXPECT_THROW(static_cast<void>(prepare(invalidSweep)), std::invalid_argument);

    auto missingFillet = filletIntent();
    missingFillet.arguments.erase("edges");
    EXPECT_THROW(static_cast<void>(prepare(missingFillet)), std::invalid_argument);

    auto extraFillet = filletIntent();
    extraFillet.arguments.emplace("unexpected", "true");
    EXPECT_THROW(static_cast<void>(prepare(extraFillet)), std::invalid_argument);

    auto invalidFillet = filletIntent();
    invalidFillet.arguments["edges"] = "1:0.25";
    EXPECT_THROW(static_cast<void>(prepare(invalidFillet)), std::invalid_argument);
}

TEST_F(CollaborativeSweepFilletOperationsTest,
       rejectsResultAliasingAnySweepOrFilletInput)
{
    auto sweepResultIsSpine = sweepIntent();
    sweepResultIsSpine.arguments["result"] = "Spine";
    EXPECT_THROW(static_cast<void>(prepare(sweepResultIsSpine)),
                 std::invalid_argument);

    auto sweepResultIsSection = sweepIntent();
    sweepResultIsSection.arguments["result"] = "Section";
    EXPECT_THROW(static_cast<void>(prepare(sweepResultIsSection)),
                 std::invalid_argument);

    auto filletResultIsBase = filletIntent();
    filletResultIsBase.arguments["result"] = "Box";
    EXPECT_THROW(static_cast<void>(prepare(filletResultIsBase)),
                 std::invalid_argument);
}

TEST_F(CollaborativeSweepFilletOperationsTest,
       adaptersAreIsolatedAndDeclareExactDependencies)
{
    auto sweep = prepare(sweepIntent());
    EXPECT_EQ(sweep.policy, App::PreparationPolicy::IsolatedProcess);
    EXPECT_FALSE(static_cast<bool>(sweep.detachedTask));
    ASSERT_NE(sweep.isolatedTask, nullptr);
    EXPECT_EQ(sweep.isolatedTask->request.operationType,
              Part::CollaborativeSweepOperationType);
    EXPECT_EQ(sweep.readSet, expectedReads({"Spine", "Section"}, "SweepResult"));
    EXPECT_EQ(sweep.writeSet, expectedWrites("SweepResult"));
    EXPECT_EQ(sweep.publicationEffects.size(), sweep.writeSet.size());

    auto fillet = prepare(filletIntent());
    EXPECT_EQ(fillet.policy, App::PreparationPolicy::IsolatedProcess);
    EXPECT_FALSE(static_cast<bool>(fillet.detachedTask));
    ASSERT_NE(fillet.isolatedTask, nullptr);
    EXPECT_EQ(fillet.isolatedTask->request.operationType,
              Part::CollaborativeFilletOperationType);
    EXPECT_EQ(fillet.readSet, expectedReads({"Box"}, "FilletResult"));
    EXPECT_EQ(fillet.writeSet, expectedWrites("FilletResult"));
    EXPECT_EQ(fillet.publicationEffects.size(), fillet.writeSet.size());
}

TEST_F(CollaborativeSweepFilletOperationsTest,
       nativeSweepHandlerDecodesAppliesAndChecksValidResult)
{
    auto preparation = prepare(sweepIntent());
    auto operation = materialize(preparation);
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->typeId(), Part::CollaborativeSweepOperationType);

    operation->apply(*_document);

    ASSERT_FALSE(_sweepResult->Shape.getValue().IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(_sweepResult->Shape.getValue()).IsValid());
    const auto postcondition = operation->checkPostcondition(*_document);
    EXPECT_TRUE(postcondition.satisfied) << postcondition.message;
}

TEST_F(CollaborativeSweepFilletOperationsTest,
       nativeFilletHandlerDecodesAppliesAndChecksValidResult)
{
    auto preparation = prepare(filletIntent());
    auto operation = materialize(preparation);
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->typeId(), Part::CollaborativeFilletOperationType);

    operation->apply(*_document);

    ASSERT_FALSE(_filletResult->Shape.getValue().IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(_filletResult->Shape.getValue()).IsValid());
    const auto postcondition = operation->checkPostcondition(*_document);
    EXPECT_TRUE(postcondition.satisfied) << postcondition.message;
}

TEST_F(CollaborativeSweepFilletOperationsTest,
       nativeHandlersRejectTruncatedParametersAndCancellation)
{
    auto truncatedSweep = prepare(sweepIntent());
    ASSERT_NE(truncatedSweep.isolatedTask, nullptr);
    auto& sweepParameters =
        requireSection(truncatedSweep.isolatedTask->inputArchive, "parameters");
    ASSERT_EQ(sweepParameters.bytes.size(), 4U);
    sweepParameters.bytes.resize(3);
    EXPECT_THROW(static_cast<void>(materialize(truncatedSweep)), std::invalid_argument);

    auto truncatedFillet = prepare(filletIntent());
    ASSERT_NE(truncatedFillet.isolatedTask, nullptr);
    auto& filletParameters =
        requireSection(truncatedFillet.isolatedTask->inputArchive, "parameters");
    ASSERT_GT(filletParameters.bytes.size(), 4U);
    filletParameters.bytes.pop_back();
    EXPECT_THROW(static_cast<void>(materialize(truncatedFillet)), std::invalid_argument);

    std::stop_source cancellation;
    ASSERT_TRUE(cancellation.request_stop());
    auto cancelledSweep = prepare(sweepIntent());
    EXPECT_THROW(static_cast<void>(
                     materialize(cancelledSweep, cancellation.get_token())),
                 std::invalid_argument);
    auto cancelledFillet = prepare(filletIntent());
    EXPECT_THROW(static_cast<void>(
                     materialize(cancelledFillet, cancellation.get_token())),
                 std::invalid_argument);
}

TEST_F(CollaborativeSweepFilletOperationsTest,
       trustedDecoderRejectsTrailingNonWhitespaceBrepWithoutMutation)
{
    const TopoDS_Shape sentinel = BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape();
    _sweepResult->Shape.setValue(sentinel);
    auto preparation = prepare(sweepIntent());
    auto output = executeArchive(preparation);
    auto& result = requireSection(output, "result.brep");
    result.bytes.push_back('\n');
    result.bytes.push_back('\t');
    result.bytes.push_back('X');

    EXPECT_THROW(static_cast<void>(decodeArchive(preparation, output)),
                 std::invalid_argument);
    EXPECT_TRUE(_sweepResult->Shape.getValue().IsEqual(sentinel));
}

TEST_F(CollaborativeSweepFilletOperationsTest,
       malformedWorkerResultThrowsWithoutMutatingLiveResult)
{
    const TopoDS_Shape sentinel = BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape();
    _filletResult->Shape.setValue(sentinel);
    auto preparation = prepare(filletIntent());
    auto output = executeArchive(preparation);
    requireSection(output, "result.brep").bytes = {0xffU, 0x00U, 0x7fU};

    EXPECT_THROW(static_cast<void>(decodeArchive(preparation, output)),
                 std::invalid_argument);
    EXPECT_TRUE(_filletResult->Shape.getValue().IsEqual(sentinel));
}

TEST_F(CollaborativeSweepFilletOperationsTest,
       sweepCompletesThroughFreeCADCmdAndCoordinatorCommit)
{
    auto prepared = prepareThroughProcess(sweepIntent(), "part-sweep-process");
    const auto commit =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);

    ASSERT_TRUE(commit.committed()) << commit.message;
    EXPECT_FALSE(_sweepResult->Shape.getValue().IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(_sweepResult->Shape.getValue()).IsValid());
    EXPECT_TRUE(prepared.operation().checkPostcondition(*_document).satisfied);
}

TEST_F(CollaborativeSweepFilletOperationsTest,
       filletCompletesThroughFreeCADCmdAndCoordinatorCommit)
{
    auto prepared = prepareThroughProcess(filletIntent(), "part-fillet-process");
    const auto commit =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);

    ASSERT_TRUE(commit.committed()) << commit.message;
    EXPECT_FALSE(_filletResult->Shape.getValue().IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(_filletResult->Shape.getValue()).IsValid());
    EXPECT_TRUE(prepared.operation().checkPostcondition(*_document).satisfied);
}
