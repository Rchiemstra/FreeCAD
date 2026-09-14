// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/GeometryWorkerOperationRegistry.h>
#include <Base/Exception.h>
#include <Mod/Part/App/CollaborativeBooleanOperation.h>
#include <Mod/Part/App/FCBRepAlgoAPI_Cut.h>
#include <Mod/Part/App/PartFeature.h>
#include <src/App/InitApplication.h>

#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepTools.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Mat.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <future>
#include <memory>
#include <sstream>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

double volume(const TopoDS_Shape& shape)
{
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return properties.Mass();
}

App::CollaborativeOperationIntent booleanIntent(std::string kind = "cut")
{
    return {std::string(Part::CollaborativeBooleanOperationType),
            {{"base", "Base"},
             {"tool", "Tool"},
             {"result", "Result"},
             {"kind", std::move(kind)}}};
}

std::unique_ptr<const App::CollaborativeOperation> materializeIsolated(
    App::CollaborativeOperationPreparation::IsolatedTask& task,
    const std::stop_token stopToken = {})
{
    task.inputArchive.metadata.operationType = task.request.operationType;
    auto output = App::Internal::GeometryWorkerOperationRegistry::instance().execute(
        task.request.operationType, task.inputArchive, stopToken);
    output.metadata = task.inputArchive.metadata;
    output.metadata.kind = App::GeometryArchiveKind::Result;
    return task.decodeResult(output);
}

std::unique_ptr<const App::CollaborativeOperation> materializeIsolated(
    App::CollaborativeOperationPreparation& preparation,
    const std::stop_token stopToken = {})
{
    if (preparation.policy != App::PreparationPolicy::IsolatedProcess
        || preparation.detachedTask || !preparation.isolatedTask) {
        throw std::runtime_error("Boolean adapter did not return isolated work");
    }
    return materializeIsolated(*preparation.isolatedTask, stopToken);
}

TopoDS_Shape manySidedPrism(double xOffset)
{
    constexpr int pointCount = 512;
    constexpr double pi = 3.14159265358979323846;
    BRepBuilderAPI_MakePolygon polygon;
    for (int index = 0; index < pointCount; ++index) {
        const double angle = 2.0 * pi * static_cast<double>(index)
            / static_cast<double>(pointCount);
        const double radius = index % 2 == 0 ? 10.0 : 9.75;
        polygon.Add(gp_Pnt(xOffset + radius * std::cos(angle),
                           radius * std::sin(angle),
                           0.0));
    }
    polygon.Close();
    BRepBuilderAPI_MakeFace face(polygon.Wire());
    return BRepPrimAPI_MakePrism(face.Face(), gp_Vec(0.0, 0.0, 3.0)).Shape();
}

TopoDS_Shape boxCompound(const std::vector<double>& centers)
{
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (double center : centers) {
        builder.Add(compound,
                    BRepPrimAPI_MakeBox(gp_Pnt(center - 0.2, -0.5, -0.5),
                                        0.4,
                                        1.0,
                                        1.0)
                        .Shape());
    }
    return compound;
}

TopoDS_Shape shapeCompound(const std::vector<TopoDS_Shape>& shapes)
{
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto& shape : shapes) {
        builder.Add(compound, shape);
    }
    return compound;
}

TopoDS_Shape regrouped(const TopoDS_Shape& shape)
{
    BRep_Builder builder;
    TopoDS_Compound outer;
    TopoDS_Compound inner;
    builder.MakeCompound(outer);
    builder.MakeCompound(inner);
    builder.Add(inner, shape);
    builder.Add(outer, inner);
    return outer;
}

std::array<std::size_t, 8> topologyCounts(const TopoDS_Shape& shape)
{
    static constexpr std::array<TopAbs_ShapeEnum, 8> types {
        TopAbs_COMPOUND,
        TopAbs_COMPSOLID,
        TopAbs_SOLID,
        TopAbs_SHELL,
        TopAbs_FACE,
        TopAbs_WIRE,
        TopAbs_EDGE,
        TopAbs_VERTEX};
    std::array<std::size_t, 8> counts {};
    for (std::size_t index = 0; index < types.size(); ++index) {
        TopTools_IndexedMapOfShape entities;
        TopExp::MapShapes(shape, types[index], entities);
        counts[index] = static_cast<std::size_t>(entities.Extent());
    }
    return counts;
}

class CollaborativeBooleanPreparationTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        Part::ensureCollaborativeBooleanOperationRegistered();
    }

    void SetUp() override
    {
        _documentName =
            App::GetApplication().getUniqueDocumentName("collaborativeBooleanDetached");
        _document = App::GetApplication().newDocument(_documentName.c_str(),
                                                       "Detached Boolean test");
        _base = _document->addObject<Part::Feature>("Base");
        _tool = _document->addObject<Part::Feature>("Tool");
        _result = _document->addObject<Part::Feature>("Result");
        _base->Shape.setValue(BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape());
        _tool->Shape.setValue(
            BRepPrimAPI_MakeBox(gp_Pnt(1.0, 0.0, 0.0), 2.0, 2.0, 2.0).Shape());
        _document->recompute();
    }

    void TearDown() override
    {
        if (_document) {
            App::GetApplication().closeDocument(_documentName.c_str());
        }
    }

    App::CollaborativeOperationPreparation prepare(std::string kind = "cut")
    {
        return App::CollaborativeOperationRegistry::instance().prepare(
            *_document, booleanIntent(std::move(kind)));
    }

    App::CollaborativeOperationPreparation prepareWithProbe(
        const std::shared_ptr<Part::Internal::CollaborativeBooleanTaskProbe>& probe,
        std::string kind = "cut")
    {
        return Part::Internal::prepareCollaborativeBooleanForTests(
            *_document, booleanIntent(std::move(kind)), probe);
    }

    void closeDocument()
    {
        App::GetApplication().closeDocument(_documentName.c_str());
        _document = nullptr;
        _base = nullptr;
        _tool = nullptr;
        _result = nullptr;
    }

    std::string _documentName;
    App::Document* _document {nullptr};
    Part::Feature* _base {nullptr};
    Part::Feature* _tool {nullptr};
    Part::Feature* _result {nullptr};
};

}  // namespace

TEST_F(CollaborativeBooleanPreparationTest,
       adapterReturnsIsolatedTaskWithoutSynchronousOperation)
{
    const TopoDS_Shape sentinel = BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape();
    _result->Shape.setValue(sentinel);

    auto preparation = prepare();

    EXPECT_TRUE(preparation.isDetached());
    EXPECT_EQ(preparation.operation.get(), nullptr);
    EXPECT_EQ(preparation.policy, App::PreparationPolicy::IsolatedProcess);
    EXPECT_FALSE(static_cast<bool>(preparation.detachedTask));
    ASSERT_NE(preparation.isolatedTask, nullptr);
    EXPECT_EQ(preparation.isolatedTask->request.policy,
              App::PreparationPolicy::IsolatedProcess);
    EXPECT_EQ(preparation.isolatedTask->request.operationType,
              Part::CollaborativeBooleanOperationType);
    EXPECT_FALSE(preparation.isolatedTask->inputArchive.sections.empty());
    EXPECT_TRUE(static_cast<bool>(preparation.isolatedTask->decodeResult));
    EXPECT_TRUE(_result->Shape.getValue().IsEqual(sentinel));
}

TEST_F(CollaborativeBooleanPreparationTest,
       isolatedArchiveRunsOffThreadAfterLiveDocumentIsGone)
{
    auto preparation = prepare("fuse");
    ASSERT_NE(preparation.isolatedTask, nullptr);
    auto task = std::move(preparation.isolatedTask);
    const auto ownerThread = std::this_thread::get_id();
    closeDocument();

    auto future = std::async(std::launch::async, [task = std::move(task)]() mutable {
        auto operation = materializeIsolated(*task);
        return std::pair {std::this_thread::get_id(), std::move(operation)};
    });
    auto [workerThread, operation] = future.get();

    EXPECT_NE(workerThread, ownerThread);
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->typeId(), Part::CollaborativeBooleanOperationType);
}

TEST_F(CollaborativeBooleanPreparationTest, cancellationBeforeBooleanDoesNoWork)
{
    const TopoDS_Shape sentinel = BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape();
    _result->Shape.setValue(sentinel);
    auto preparation = prepare();
    std::stop_source cancellation;
    ASSERT_TRUE(cancellation.request_stop());

    EXPECT_THROW(
        static_cast<void>(materializeIsolated(preparation, cancellation.get_token())),
        std::runtime_error);
    EXPECT_TRUE(_result->Shape.getValue().IsEqual(sentinel));
}

TEST_F(CollaborativeBooleanPreparationTest,
       cancellationAfterOcctBuildStartsCompletesWithinBound)
{
    _base->Shape.setValue(manySidedPrism(0.0));
    _tool->Shape.setValue(manySidedPrism(0.1));
    _document->recompute();
    auto probe = std::make_shared<Part::Internal::CollaborativeBooleanTaskProbe>();
    auto preparation = prepareWithProbe(probe, "common");
    closeDocument();
    std::stop_source cancellation;
    std::promise<void> completionPromise;
    auto completion = completionPromise.get_future();
    std::thread worker([task = std::move(preparation.detachedTask),
                        token = cancellation.get_token(),
                        completionPromise = std::move(completionPromise)]() mutable {
        try {
            static_cast<void>(task(token));
            completionPromise.set_value();
        }
        catch (...) {
            completionPromise.set_exception(std::current_exception());
        }
    });
    const auto startDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!probe->buildEntered.load(std::memory_order_relaxed)
            || probe->progressCallbacks.load(std::memory_order_relaxed) == 0)
           && completion.wait_for(std::chrono::milliseconds(0))
               != std::future_status::ready
           && std::chrono::steady_clock::now() < startDeadline) {
        std::this_thread::yield();
    }
    if (!probe->buildEntered.load(std::memory_order_relaxed)
        || probe->progressCallbacks.load(std::memory_order_relaxed) == 0) {
        static_cast<void>(cancellation.request_stop());
        if (completion.wait_for(std::chrono::seconds(10))
            == std::future_status::ready) {
            worker.join();
        }
        else {
            worker.detach();
        }
        FAIL() << "OCCT Boolean did not reach its progress-aware Build entry";
    }
    if (completion.wait_for(std::chrono::milliseconds(0))
        == std::future_status::ready) {
        worker.join();
        FAIL() << "test geometry completed before active cancellation could be requested";
    }
    EXPECT_FALSE(probe->buildCompletedNaturally.load(std::memory_order_relaxed));

    EXPECT_TRUE(cancellation.request_stop());
    if (completion.wait_for(std::chrono::seconds(10))
        != std::future_status::ready) {
        worker.detach();
        FAIL() << "OCCT Boolean did not observe cancellation within the test bound";
    }
    worker.join();
    EXPECT_THROW(static_cast<void>(completion.get()), std::runtime_error);
    EXPECT_TRUE(probe->cancellationObserved.load(std::memory_order_relaxed));
    EXPECT_FALSE(probe->buildCompletedNaturally.load(std::memory_order_relaxed));
}

TEST_F(CollaborativeBooleanPreparationTest,
       captureEnforcesNoSharedInputTShapesAndSurvivesSourceDestruction)
{
    auto probe = std::make_shared<Part::Internal::CollaborativeBooleanTaskProbe>();
    auto preparation = prepareWithProbe(probe);
    EXPECT_TRUE(probe->baseSnapshotIndependent.load(std::memory_order_relaxed));
    EXPECT_TRUE(probe->toolSnapshotIndependent.load(std::memory_order_relaxed));
    auto task = std::move(preparation.detachedTask);
    closeDocument();

    auto operation = task(std::stop_token {});

    ASSERT_NE(operation, nullptr);
}

TEST_F(CollaborativeBooleanPreparationTest,
       preparedOperationAppliesResultComputedFromCapturedShapes)
{
    auto probe = std::make_shared<Part::Internal::CollaborativeBooleanTaskProbe>();
    auto preparation = prepareWithProbe(probe, "cut");
    ASSERT_TRUE(probe->baseSnapshotIndependent.load(std::memory_order_relaxed));
    ASSERT_TRUE(probe->toolSnapshotIndependent.load(std::memory_order_relaxed));

    // Mutate a source TShape in place after capture. The detached task must
    // remain isolated even from changes that bypass property replacement.
    TopExp_Explorer vertices(_tool->Shape.getValue(), TopAbs_VERTEX);
    ASSERT_TRUE(vertices.More());
    const TopoDS_Vertex vertex = TopoDS::Vertex(vertices.Current());
    const gp_Pnt originalPoint = BRep_Tool::Pnt(vertex);
    BRep_Builder builder;
    builder.UpdateVertex(vertex,
                         gp_Pnt(originalPoint.X() + 100.0,
                                originalPoint.Y(),
                                originalPoint.Z()),
                         BRep_Tool::Tolerance(vertex));
    auto operation = preparation.detachedTask(std::stop_token {});
    ASSERT_NE(operation, nullptr);

    operation->apply(*_document);

    const auto immediatePostcondition = operation->checkPostcondition(*_document);
    ASSERT_TRUE(immediatePostcondition.satisfied) << immediatePostcondition.message;
    EXPECT_NEAR(volume(_result->Shape.getValue()), 4.0, 1e-7);
    const auto postVolumePostcondition = operation->checkPostcondition(*_document);
    EXPECT_TRUE(postVolumePostcondition.satisfied) << postVolumePostcondition.message;
    BRepBuilderAPI_Copy transactionCopy(_result->Shape.getValue(),
                                        Standard_True,
                                        Standard_False);
    ASSERT_TRUE(transactionCopy.IsDone());
    ASSERT_FALSE(transactionCopy.Shape().IsPartner(_result->Shape.getValue()));
    _result->Shape.setValue(transactionCopy.Shape());
    _document->recompute();
    EXPECT_TRUE(operation->checkPostcondition(*_document).satisfied);

    _result->Shape.setValue(BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape());
    EXPECT_FALSE(operation->checkPostcondition(*_document).satisfied);
}

TEST_F(CollaborativeBooleanPreparationTest,
       retryCopiesPristineResultAfterLiveTShapeMutation)
{
    auto preparation = prepare("cut");
    auto operation = materializeIsolated(preparation);
    ASSERT_NE(operation, nullptr);
    operation->apply(*_document);
    ASSERT_NEAR(volume(_result->Shape.getValue()), 4.0, 1e-7);
    ASSERT_TRUE(operation->checkPostcondition(*_document).satisfied);

    TopExp_Explorer vertices(_result->Shape.getValue(), TopAbs_VERTEX);
    ASSERT_TRUE(vertices.More());
    const TopoDS_Vertex vertex = TopoDS::Vertex(vertices.Current());
    const gp_Pnt originalPoint = BRep_Tool::Pnt(vertex);
    BRep_Builder builder;
    builder.UpdateVertex(vertex,
                         gp_Pnt(originalPoint.X() + 100.0,
                                originalPoint.Y(),
                                originalPoint.Z()),
                         BRep_Tool::Tolerance(vertex));
    EXPECT_FALSE(operation->checkPostcondition(*_document).satisfied);

    // Replacing the failed attempt models transaction rollback. The retry must
    // still source a fresh copy from immutable detached geometry rather than
    // the live TShapes mutated above.
    _result->Shape.setValue(BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape());
    operation->apply(*_document);

    EXPECT_NEAR(volume(_result->Shape.getValue()), 4.0, 1e-7);
    EXPECT_TRUE(operation->checkPostcondition(*_document).satisfied);
}

TEST_F(CollaborativeBooleanPreparationTest,
       intersectingCompoundBaseCutMatchesFcbSemantics)
{
    const TopoDS_Shape base = shapeCompound(
        {BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape(),
         BRepPrimAPI_MakeBox(gp_Pnt(1.0, 0.0, 0.0), 2.0, 2.0, 2.0).Shape()});
    const TopoDS_Shape tool =
        BRepPrimAPI_MakeBox(gp_Pnt(1.5, 0.0, 0.0), 1.0, 2.0, 2.0).Shape();
    FCBRepAlgoAPI_Cut reference(base, tool);
    ASSERT_TRUE(reference.IsDone());
    ASSERT_FALSE(reference.Shape().IsNull());
    _base->Shape.setValue(base);
    _tool->Shape.setValue(tool);
    _document->recompute();

    auto preparation = prepare("cut");
    auto operation = materializeIsolated(preparation);
    ASSERT_NE(operation, nullptr);
    operation->apply(*_document);

    EXPECT_NEAR(volume(_result->Shape.getValue()), volume(reference.Shape()), 1e-7);
    EXPECT_EQ(topologyCounts(_result->Shape.getValue()),
              topologyCounts(reference.Shape()));
}

TEST_F(CollaborativeBooleanPreparationTest,
       intersectingCompoundBaseAndToolCutMatchesFcbSemantics)
{
    const TopoDS_Shape base = shapeCompound(
        {BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape(),
         BRepPrimAPI_MakeBox(gp_Pnt(1.0, 0.0, 0.0), 2.0, 2.0, 2.0).Shape()});
    const TopoDS_Shape tool = shapeCompound(
        {BRepPrimAPI_MakeBox(gp_Pnt(0.5, 0.0, 0.0), 1.5, 2.0, 2.0).Shape(),
         BRepPrimAPI_MakeBox(gp_Pnt(1.5, 0.0, 0.0), 1.25, 2.0, 2.0).Shape()});
    FCBRepAlgoAPI_Cut reference(base, tool);
    ASSERT_TRUE(reference.IsDone());
    ASSERT_FALSE(reference.Shape().IsNull());
    _base->Shape.setValue(base);
    _tool->Shape.setValue(tool);
    _document->recompute();

    auto preparation = prepare("cut");
    auto operation = materializeIsolated(preparation);
    ASSERT_NE(operation, nullptr);
    operation->apply(*_document);

    EXPECT_NEAR(volume(_result->Shape.getValue()), volume(reference.Shape()), 1e-7);
    EXPECT_EQ(topologyCounts(_result->Shape.getValue()),
              topologyCounts(reference.Shape()));
}

TEST_F(CollaborativeBooleanPreparationTest,
       largeResultStreamsCanonicalBytesIntoFixedSizeDigest)
{
    _base->Shape.setValue(manySidedPrism(0.0));
    _tool->Shape.setValue(
        BRepPrimAPI_MakeBox(gp_Pnt(50.0, 50.0, 50.0), 1.0, 1.0, 1.0).Shape());
    _document->recompute();
    auto probe = std::make_shared<Part::Internal::CollaborativeBooleanTaskProbe>();
    auto preparation = prepareWithProbe(probe, "cut");

    auto operation = preparation.detachedTask(std::stop_token {});
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(Part::Internal::CollaborativeBooleanDigestSize, 32U);
    EXPECT_GT(probe->expectedCanonicalBytes.load(std::memory_order_relaxed),
              64U * 1024U);
    operation->apply(*_document);
    EXPECT_TRUE(operation->checkPostcondition(*_document).satisfied);
}

TEST_F(CollaborativeBooleanPreparationTest,
       canonicalResultRejectsUnitTranslationAtLargeCoordinates)
{
    constexpr double coordinate = 1.0e12;
    _base->Shape.setValue(
        BRepPrimAPI_MakeBox(gp_Pnt(coordinate, 0.0, 0.0), 4.0, 2.0, 2.0).Shape());
    _tool->Shape.setValue(
        BRepPrimAPI_MakeBox(gp_Pnt(coordinate + 2.0, 0.0, 0.0), 2.0, 2.0, 2.0)
            .Shape());
    _document->recompute();
    auto preparation = prepare("cut");
    auto operation = materializeIsolated(preparation);
    ASSERT_NE(operation, nullptr);
    operation->apply(*_document);
    ASSERT_TRUE(operation->checkPostcondition(*_document).satisfied);

    TopoDS_Shape translated = _result->Shape.getValue();
    gp_Trsf translation;
    translation.SetTranslation(gp_Vec(1.0, 0.0, 0.0));
    translated.Move(TopLoc_Location(translation));
    _result->Shape.setValue(translated);

    EXPECT_FALSE(operation->checkPostcondition(*_document).satisfied);
}

TEST_F(CollaborativeBooleanPreparationTest,
       canonicalResultRejectsSameCountsAndMomentsDifferentGeometry)
{
    const TopoDS_Shape first = boxCompound({-3.0, -1.0, 1.0, 3.0});
    const TopoDS_Shape second =
        boxCompound({-std::sqrt(7.0), -std::sqrt(3.0), std::sqrt(3.0), std::sqrt(7.0)});
    ASSERT_EQ(topologyCounts(first), topologyCounts(second));
    GProp_GProps firstProperties;
    GProp_GProps secondProperties;
    BRepGProp::VolumeProperties(first, firstProperties);
    BRepGProp::VolumeProperties(second, secondProperties);
    ASSERT_NEAR(firstProperties.Mass(), secondProperties.Mass(), 1e-12);
    ASSERT_NEAR(firstProperties.CentreOfMass().X(),
                secondProperties.CentreOfMass().X(),
                1e-12);
    const gp_Mat firstInertia = firstProperties.MatrixOfInertia();
    const gp_Mat secondInertia = secondProperties.MatrixOfInertia();
    for (int row = 1; row <= 3; ++row) {
        for (int column = 1; column <= 3; ++column) {
            ASSERT_NEAR(firstInertia.Value(row, column),
                        secondInertia.Value(row, column),
                        1e-10);
        }
    }

    _base->Shape.setValue(first);
    _tool->Shape.setValue(
        BRepPrimAPI_MakeBox(gp_Pnt(100.0, 100.0, 100.0), 1.0, 1.0, 1.0).Shape());
    _document->recompute();
    auto preparation = prepare("cut");
    auto operation = materializeIsolated(preparation);
    ASSERT_NE(operation, nullptr);
    operation->apply(*_document);
    ASSERT_TRUE(operation->checkPostcondition(*_document).satisfied);
    ASSERT_EQ(topologyCounts(_result->Shape.getValue()), topologyCounts(second));

    _result->Shape.setValue(second);

    EXPECT_FALSE(operation->checkPostcondition(*_document).satisfied);
}

TEST_F(CollaborativeBooleanPreparationTest,
       canonicalResultRejectsTopologyRegroupingOfSameGeometry)
{
    auto preparation = prepare("fuse");
    auto operation = materializeIsolated(preparation);
    ASSERT_NE(operation, nullptr);
    operation->apply(*_document);
    ASSERT_TRUE(operation->checkPostcondition(*_document).satisfied);

    const TopoDS_Shape nested = regrouped(_result->Shape.getValue());
    TopoDS_Iterator outer(nested);
    ASSERT_TRUE(outer.More());
    ASSERT_EQ(outer.Value().ShapeType(), TopAbs_COMPOUND);
    TopoDS_Iterator inner(outer.Value());
    ASSERT_TRUE(inner.More());
    EXPECT_TRUE(inner.Value().IsSame(_result->Shape.getValue()));
    inner.Next();
    EXPECT_FALSE(inner.More());
    outer.Next();
    EXPECT_FALSE(outer.More());
    _result->Shape.setValue(nested);

    EXPECT_FALSE(operation->checkPostcondition(*_document).satisfied);
}

TEST_F(CollaborativeBooleanPreparationTest,
       canonicalResultIgnoresVolatileButPreservesSemanticTShapeFlags)
{
    auto preparation = prepare("fuse");
    auto operation = materializeIsolated(preparation);
    ASSERT_NE(operation, nullptr);
    operation->apply(*_document);
    ASSERT_TRUE(operation->checkPostcondition(*_document).satisfied);

    TopoDS_Shape changed = _result->Shape.getValue();
    changed.Free(!changed.Free());
    changed.Modified(!changed.Modified());
    changed.Checked(!changed.Checked());
    EXPECT_TRUE(operation->checkPostcondition(*_document).satisfied);

    changed.Closed(!changed.Closed());
    EXPECT_FALSE(operation->checkPostcondition(*_document).satisfied);

    operation->apply(*_document);
    ASSERT_TRUE(operation->checkPostcondition(*_document).satisfied);
    changed = _result->Shape.getValue();
    changed.Infinite(!changed.Infinite());
    EXPECT_FALSE(operation->checkPostcondition(*_document).satisfied);
}

TEST_F(CollaborativeBooleanPreparationTest,
       sevenBitGeometryLineIsNotMistakenForTShapeFlags)
{
    BRep_Builder builder;
    TopoDS_Vertex first;
    TopoDS_Vertex second;
    builder.MakeVertex(first, gp_Pnt(0.0, 0.0, 0.0), 1010101.0);
    builder.MakeVertex(second, gp_Pnt(0.0, 0.0, 0.0), 1101011.0);

    std::ostringstream firstBrep;
    std::ostringstream secondBrep;
    BRepTools::Write(first, firstBrep);
    BRepTools::Write(second, secondBrep);
    ASSERT_NE(firstBrep.str().find("\n1010101\n"), std::string::npos);
    ASSERT_NE(secondBrep.str().find("\n1101011\n"), std::string::npos);

    EXPECT_NE(Part::Internal::collaborativeBooleanShapeDigestForTests(first),
              Part::Internal::collaborativeBooleanShapeDigestForTests(second));
}

TEST_F(CollaborativeBooleanPreparationTest,
       frozenReadRevisionsDetectUnclassifiedShapeMutationThroughWildcard)
{
    auto preparation = prepare();
    const auto capturedRevisions =
        _document->collaborationRevisions().capture(preparation.readSet);
    const auto baseModel = App::DocumentRevisionKey::objectModel("Base");
    const auto wildcard = App::DocumentRevisionKey::unknownModelMutation();
    ASSERT_NE(std::ranges::find(preparation.readSet, baseModel),
              preparation.readSet.end());
    ASSERT_NE(std::ranges::find(preparation.readSet, wildcard),
              preparation.readSet.end());

    _base->Shape.setValue(BRepPrimAPI_MakeBox(3.0, 2.0, 2.0).Shape());
    const auto conflicts =
        _document->collaborationRevisions().validate(capturedRevisions);

    const auto wildcardConflict = std::ranges::find_if(conflicts, [&](const auto& conflict) {
        return conflict.key == wildcard;
    });
    ASSERT_NE(wildcardConflict, conflicts.end());
    EXPECT_LT(wildcardConflict->expected, wildcardConflict->current);
    EXPECT_EQ(std::ranges::find_if(conflicts, [&](const auto& conflict) {
                  return conflict.key == baseModel;
              }),
              conflicts.end());
    EXPECT_TRUE(_result->Shape.getValue().IsNull());
}

TEST_F(CollaborativeBooleanPreparationTest,
       documentPreparationBoundaryRejectsMutablePythonPayloadBeforeCapture)
{
    ASSERT_NE(_base->addDynamicProperty("App::PropertyPythonObject", "MutablePayload"),
              nullptr);
    _document->recompute();
    ASSERT_FALSE(_document->collaborationPreparationSupported());
    auto session = _document->collaborationService().beginEditSession("python-boundary-test");

    EXPECT_THROW(static_cast<void>(_document->collaborationService().prepareEdit(
                     session.sessionId(),
                     "must-not-capture",
                     booleanIntent(),
                     "native-part-test")),
                 Base::RuntimeError);
    EXPECT_TRUE(_result->Shape.getValue().IsNull());
}
