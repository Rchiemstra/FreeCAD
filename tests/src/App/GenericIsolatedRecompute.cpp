// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperation.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/Expression.h>
#include <App/FeatureTest.h>
#include <App/GenericIsolatedRecompute.h>
#include <App/GeometryWorkerOperationRegistry.h>
#include <App/ObjectIdentifier.h>
#include <App/PropertyLinks.h>
#include <App/PropertyPythonObject.h>
#include <App/Range.h>
#include <Base/Exception.h>
#include <src/App/InitApplication.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

class GenericIsolatedRecomputeTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        App::Internal::ensureGenericIsolatedRecomputeRegistered();
    }

    void SetUp() override
    {
        _documentName =
            App::GetApplication().getUniqueDocumentName("genericIsolatedRecompute");
        _document = App::GetApplication().newDocument(
            _documentName.c_str(), "Generic isolated recompute test");
        ASSERT_NE(_document, nullptr);
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_documentName.c_str());
        if (!_otherDocumentName.empty()) {
            App::GetApplication().closeDocument(_otherDocumentName.c_str());
        }
    }

    App::CollaborativeOperationPreparation prepare(
        const std::string& featureName,
        const bool preserveLegacyRevisionSemantics = false)
    {
        App::CollaborativeOperationIntent intent {
            std::string(App::GenericIsolatedRecomputeOperationType),
            {{"feature", featureName}}};
        if (preserveLegacyRevisionSemantics) {
            intent.arguments.emplace("legacy_revision_semantics", "1");
        }
        return App::CollaborativeOperationRegistry::instance().prepare(
            *_document, intent);
    }

    App::Document* createOtherDocument()
    {
        _otherDocumentName =
            App::GetApplication().getUniqueDocumentName("genericRecomputeExternal");
        return App::GetApplication().newDocument(
            _otherDocumentName.c_str(), "External generic recompute test");
    }

    std::string _documentName;
    std::string _otherDocumentName;
    App::Document* _document {nullptr};
};

const App::DocumentRecomputeFeatureRequest& node(
    const App::DocumentRecomputeRequest& request,
    const std::string& name)
{
    const auto found = std::find_if(
        request.features.begin(), request.features.end(), [&](const auto& candidate) {
            return candidate.featureId == name;
        });
    if (found == request.features.end()) {
        throw std::runtime_error("generic recompute plan omitted " + name);
    }
    return *found;
}

constexpr std::uint32_t GenericProtocolMagic = 0x31524947U;
constexpr std::uint32_t GenericProtocolVersion = 2;

void appendProtocolU32(std::vector<std::uint8_t>& bytes, const std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void appendProtocolString(std::vector<std::uint8_t>& bytes, const std::string_view value)
{
    appendProtocolU32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

App::GeometryArchive resultArchive(const std::string_view target,
                                   const std::uint32_t status,
                                   const std::string_view diagnostic)
{
    App::GeometryArchive archive;
    std::vector<std::uint8_t> bytes;
    appendProtocolU32(bytes, GenericProtocolMagic);
    appendProtocolU32(bytes, GenericProtocolVersion);
    appendProtocolString(bytes, target);
    appendProtocolU32(bytes, status);
    appendProtocolString(bytes, diagnostic);
    archive.sections.push_back({"recompute.outputs", std::move(bytes)});
    return archive;
}

}  // namespace

TEST_F(GenericIsolatedRecomputeTest,
       workerArchiveRoundTripChangesOnlyDeclaredOutputAndDecodesForApply)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("Column");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("C");
    ASSERT_EQ(feature->Value.getValue(), 0);

    auto preparation = prepare("Column");
    ASSERT_EQ(preparation.policy, App::PreparationPolicy::IsolatedProcess);
    ASSERT_NE(preparation.isolatedTask, nullptr);
    EXPECT_FALSE(preparation.detachedTask);
    EXPECT_EQ(preparation.isolatedTask->request.operationType,
              App::GenericIsolatedRecomputeOperationType);
    ASSERT_EQ(preparation.isolatedTask->inputArchive.sections.size(), 2U);

    const auto output = App::Internal::GeometryWorkerOperationRegistry::instance().execute(
        std::string(App::GenericIsolatedRecomputeOperationType),
        preparation.isolatedTask->inputArchive,
        std::stop_token {});
    ASSERT_EQ(output.sections.size(), 1U);
    EXPECT_EQ(output.sections.front().name, "recompute.outputs");

    auto operation = preparation.isolatedTask->decodeResult(output);
    ASSERT_NE(operation, nullptr);
    EXPECT_TRUE(operation->recomputeOutcomeSucceeded());
    EXPECT_TRUE(operation->recomputeOutcomeDiagnostic().empty());
    EXPECT_FALSE(operation->checkPostcondition(*_document).satisfied);
    operation->apply(*_document);
    EXPECT_EQ(feature->Value.getValue(), App::decodeColumn("C"));
    const auto postcondition = operation->checkPostcondition(*_document);
    EXPECT_TRUE(postcondition.satisfied) << postcondition.message;
    feature->Value.setValue(feature->Value.getValue() + 1);
    EXPECT_FALSE(operation->checkPostcondition(*_document).satisfied);
}

TEST_F(GenericIsolatedRecomputeTest,
       subpathExpressionMarksAndAppliesItsOwningProperty)
{
    auto* feature = _document->addObject<App::FeatureTest>("PlacementExpression");
    ASSERT_NE(feature, nullptr);
    feature->Distance.setValue(23.5);
    feature->setExpression(
        App::ObjectIdentifier::parse(feature, "Placement.Base.x"),
        std::shared_ptr<App::Expression>(App::Expression::parse(feature, "Distance")));
    ASSERT_DOUBLE_EQ(feature->Placement.getValue().getPosition().x, 0.0);

    auto preparation = prepare("PlacementExpression");
    ASSERT_EQ(preparation.policy, App::PreparationPolicy::IsolatedProcess);
    ASSERT_NE(preparation.isolatedTask, nullptr);

    const auto output = App::Internal::GeometryWorkerOperationRegistry::instance().execute(
        std::string(App::GenericIsolatedRecomputeOperationType),
        preparation.isolatedTask->inputArchive,
        std::stop_token {});
    auto operation = preparation.isolatedTask->decodeResult(output);
    ASSERT_NE(operation, nullptr);

    EXPECT_DOUBLE_EQ(feature->Placement.getValue().getPosition().x, 0.0);
    operation->apply(*_document);
    EXPECT_DOUBLE_EQ(feature->Placement.getValue().getPosition().x, 23.5);
    const auto postcondition = operation->checkPostcondition(*_document);
    EXPECT_TRUE(postcondition.satisfied) << postcondition.message;
}

TEST_F(GenericIsolatedRecomputeTest,
       workerFailureReturnsStructuredArchiveAndNarrowsAuthorizedEffects)
{
    auto* feature = _document->addObject<App::FeatureTestException>("FailureFeature");
    ASSERT_NE(feature, nullptr);
    ASSERT_TRUE(feature->isValid());

    auto preparation = prepare("FailureFeature", true);
    ASSERT_EQ(preparation.policy, App::PreparationPolicy::IsolatedProcess);
    ASSERT_NE(preparation.isolatedTask, nullptr);
    ASSERT_TRUE(preparation.isolatedTask->decodePublicationEffects);

    App::GeometryArchive output;
    EXPECT_NO_THROW(
        output = App::Internal::GeometryWorkerOperationRegistry::instance().execute(
            std::string(App::GenericIsolatedRecomputeOperationType),
            preparation.isolatedTask->inputArchive,
            std::stop_token {}));
    ASSERT_EQ(output.sections.size(), 1U);
    EXPECT_EQ(output.sections.front().name, "recompute.outputs");
    EXPECT_TRUE(feature->isValid())
        << "detached failure execution must not mutate the live feature";

    auto operation = preparation.isolatedTask->decodeResult(output);
    ASSERT_NE(operation, nullptr);
    EXPECT_FALSE(operation->recomputeOutcomeSucceeded());
    EXPECT_FALSE(operation->recomputeOutcomeDiagnostic().empty());

    const auto effects =
        preparation.isolatedTask->decodePublicationEffects(output);
    ASSERT_EQ(effects.size(), 2U);
    const auto modelKey = App::DocumentRevisionKey::objectModel("FailureFeature");
    const auto unknownKey = App::DocumentRevisionKey::unknownModelMutation();
    const auto model = std::ranges::find(
        effects, modelKey, &App::DocumentRevisionPublicationRequest::key);
    const auto unknown = std::ranges::find(
        effects, unknownKey, &App::DocumentRevisionPublicationRequest::key);
    ASSERT_NE(model, effects.end());
    ASSERT_NE(unknown, effects.end());
    EXPECT_EQ(model->revisionDelta, 1U);
    EXPECT_EQ(unknown->revisionDelta, 1U);
    EXPECT_EQ(std::ranges::find_if(effects, [](const auto& effect) {
                  return effect.key.kind == App::DocumentRevisionKind::ObjectProperty;
              }),
              effects.end());
}

TEST_F(GenericIsolatedRecomputeTest,
       touchedNoRecomputeStorageUsesLightweightBookkeepingUnlessForced)
{
    auto* storage = _document->addObject<App::DocumentObject>("NoRecomputeStorage");
    ASSERT_NE(storage, nullptr);
    auto* value = dynamic_cast<App::PropertyInteger*>(storage->addDynamicProperty(
        "App::PropertyInteger", "Value", "Data", "", App::Prop_NoRecompute));
    ASSERT_NE(value, nullptr);
    value->setValue(17);
    ASSERT_TRUE(storage->isTouched());
    ASSERT_EQ(storage->mustRecompute(), 0);

    auto preparation = prepare("NoRecomputeStorage");
    EXPECT_EQ(preparation.policy, App::PreparationPolicy::DetachedInProcess);
    EXPECT_EQ(preparation.isolatedTask, nullptr);
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    ASSERT_NE(operation, nullptr);
    EXPECT_FALSE(operation->checkPostcondition(*_document).satisfied);
    operation->apply(*_document);
    const auto postcondition = operation->checkPostcondition(*_document);
    EXPECT_TRUE(postcondition.satisfied) << postcondition.message;
    EXPECT_EQ(value->getValue(), 17);
    EXPECT_FALSE(storage->isTouched());
    EXPECT_EQ(storage->mustRecompute(), 0);

    value->setValue(18);
    App::CollaborativeOperationIntent forcedIntent {
        std::string(App::GenericIsolatedRecomputeOperationType),
        {{"feature", "NoRecomputeStorage"}, {"force_execution", "1"}}};
    auto forced = App::CollaborativeOperationRegistry::instance().prepare(
        *_document, forcedIntent);
    EXPECT_EQ(forced.policy, App::PreparationPolicy::IsolatedProcess);
    EXPECT_NE(forced.isolatedTask, nullptr);
    EXPECT_FALSE(forced.detachedTask);
}

TEST_F(GenericIsolatedRecomputeTest,
       malformedStatusAndEmptyFailureDiagnosticAreRejectedByTrustedDecoders)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("Column");
    ASSERT_NE(feature, nullptr);
    auto preparation = prepare("Column", true);
    ASSERT_NE(preparation.isolatedTask, nullptr);
    ASSERT_TRUE(preparation.isolatedTask->decodePublicationEffects);

    const auto malformedStatus = resultArchive("Column", 2, "invalid status");
    EXPECT_THROW(
        static_cast<void>(preparation.isolatedTask->decodeResult(malformedStatus)),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(
            preparation.isolatedTask->decodePublicationEffects(malformedStatus)),
        std::invalid_argument);

    const auto emptyFailure = resultArchive("Column", 0, {});
    EXPECT_THROW(
        static_cast<void>(preparation.isolatedTask->decodeResult(emptyFailure)),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(
            preparation.isolatedTask->decodePublicationEffects(emptyFailure)),
        std::invalid_argument);
}

TEST_F(GenericIsolatedRecomputeTest,
       semanticEqualityIgnoresNormalizedNonOutputStateAndPublishesOnlyOutputs)
{
    auto* feature = _document->addObject<App::FeatureTest>("SemanticFeature");
    ASSERT_NE(feature, nullptr);
    feature->Angle.setValue(17.25);
    feature->Integer.setValue(1729);
    feature->String.setValue("archive-normalized baseline");

    ASSERT_TRUE(feature->isOutputProperty(&feature->ExecCount));
    ASSERT_TRUE(feature->isOutputProperty(&feature->ExecResult));
    ASSERT_FALSE(feature->isOutputProperty(&feature->Angle));
    ASSERT_FALSE(feature->isOutputProperty(&feature->Integer));
    ASSERT_FALSE(feature->isOutputProperty(&feature->String));
    ASSERT_EQ(feature->ExecCount.getValue(), 0);
    ASSERT_EQ(feature->ExecResult.getStrValue(), "empty");
    const double angleBefore = feature->Angle.getValue();
    const long integerBefore = feature->Integer.getValue();
    const std::string stringBefore = feature->String.getStrValue();

    auto preparation = prepare("SemanticFeature");
    ASSERT_EQ(preparation.policy, App::PreparationPolicy::IsolatedProcess);
    ASSERT_NE(preparation.isolatedTask, nullptr);

    const auto output = App::Internal::GeometryWorkerOperationRegistry::instance().execute(
        std::string(App::GenericIsolatedRecomputeOperationType),
        preparation.isolatedTask->inputArchive,
        std::stop_token {});
    ASSERT_EQ(output.sections.size(), 1U);
    EXPECT_EQ(output.sections.front().name, "recompute.outputs");

    // Detached execution must not leak state into the live document before apply.
    EXPECT_EQ(feature->ExecCount.getValue(), 0);
    EXPECT_EQ(feature->ExecResult.getStrValue(), "empty");
    EXPECT_DOUBLE_EQ(feature->Angle.getValue(), angleBefore);
    EXPECT_EQ(feature->Integer.getValue(), integerBefore);
    EXPECT_EQ(feature->String.getStrValue(), stringBefore);

    auto operation = preparation.isolatedTask->decodeResult(output);
    ASSERT_NE(operation, nullptr);
    operation->apply(*_document);

    EXPECT_EQ(feature->ExecCount.getValue(), 1);
    EXPECT_EQ(feature->ExecResult.getStrValue(), "Exec");
    EXPECT_DOUBLE_EQ(feature->Angle.getValue(), angleBefore);
    EXPECT_EQ(feature->Integer.getValue(), integerBefore);
    EXPECT_EQ(feature->String.getStrValue(), stringBefore);
    const auto postcondition = operation->checkPostcondition(*_document);
    EXPECT_TRUE(postcondition.satisfied) << postcondition.message;
}

TEST_F(GenericIsolatedRecomputeTest,
       documentFeatureFacadeUsesTheProductionFreeCADCmdBackend)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("ProcessColumn");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("D");
    ASSERT_EQ(feature->Value.getValue(), 0);

    const bool recomputed = feature->recomputeFeature(false);
    EXPECT_TRUE(recomputed)
        << (_document->getErrorDescription(feature)
                ? _document->getErrorDescription(feature)
                : "no recompute diagnostic");
    EXPECT_EQ(feature->Value.getValue(), App::decodeColumn("D"));
    EXPECT_FALSE(feature->mustRecompute());
    EXPECT_TRUE(feature->isValid());
}

TEST_F(GenericIsolatedRecomputeTest,
       recursivePlanCapturesTheTransitiveGraphInDependencyOrder)
{
    auto* source = _document->addObject<App::FeatureTest>("Source");
    auto* middle = _document->addObject<App::FeatureTest>("Middle");
    auto* leaf = _document->addObject<App::FeatureTest>("Leaf");
    ASSERT_NE(source, nullptr);
    ASSERT_NE(middle, nullptr);
    ASSERT_NE(leaf, nullptr);
    middle->Source1.setValue(source);
    leaf->Source1.setValue(middle);

    const auto request = App::Internal::makeGenericIsolatedRecomputeRequest(
        *_document, *source, true);

    ASSERT_EQ(request.features.size(), 3U);
    EXPECT_EQ(node(request, "Source").dependencies,
              std::vector<std::string> {});
    EXPECT_EQ(node(request, "Middle").dependencies,
              std::vector<std::string> {"Source"});
    EXPECT_EQ(node(request, "Leaf").dependencies,
              std::vector<std::string> {"Middle"});
    for (const auto& feature : request.features) {
        EXPECT_EQ(feature.intent.operationType,
                  App::GenericIsolatedRecomputeOperationType);
        EXPECT_EQ(feature.intent.arguments.at("feature"), feature.featureId);
    }
}

TEST_F(GenericIsolatedRecomputeTest,
       malformedIntentUnknownFeatureAndMalformedWorkerPayloadFailClosed)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("Column");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("B");
    auto& registry = App::CollaborativeOperationRegistry::instance();

    EXPECT_THROW(
        static_cast<void>(registry.prepare(
            *_document,
            {std::string(App::GenericIsolatedRecomputeOperationType), {}})),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(registry.prepare(
            *_document,
            {std::string(App::GenericIsolatedRecomputeOperationType),
             {{"feature", "Missing"}}})),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(registry.prepare(
            *_document,
            {std::string(App::GenericIsolatedRecomputeOperationType),
             {{"feature", "Column"}, {"unexpected", "value"}}})),
        std::invalid_argument);

    auto preparation = prepare("Column");
    ASSERT_NE(preparation.isolatedTask, nullptr);
    auto malformed = preparation.isolatedTask->inputArchive;
    const auto parameter = std::find_if(
        malformed.sections.begin(), malformed.sections.end(), [](const auto& section) {
            return section.name == "recompute.params";
        });
    ASSERT_NE(parameter, malformed.sections.end());
    parameter->bytes.push_back(0xffU);
    EXPECT_THROW(
        static_cast<void>(
            App::Internal::GeometryWorkerOperationRegistry::instance().execute(
                std::string(App::GenericIsolatedRecomputeOperationType),
                malformed,
                std::stop_token {})),
        std::invalid_argument);
}

TEST_F(GenericIsolatedRecomputeTest,
       crossDocumentClosureAndUnoptedPythonFeatureAreRejected)
{
    auto* target = _document->addObject<App::FeatureTestColumn>("CrossDocument");
    ASSERT_NE(target, nullptr);
    auto* other = createOtherDocument();
    ASSERT_NE(other, nullptr);
    auto* external = other->addObject<App::FeatureTest>("External");
    ASSERT_NE(external, nullptr);
    _document->FileName.setValue(
        App::Application::getTempFileName("generic-recompute-owner.FCStd"));
    other->FileName.setValue(
        App::Application::getTempFileName("generic-recompute-external.FCStd"));
    auto* externalLink = dynamic_cast<App::PropertyXLink*>(
        target->addDynamicProperty("App::PropertyXLink", "ExternalSource"));
    ASSERT_NE(externalLink, nullptr);
    try {
        externalLink->setValue(external);
    }
    catch (const Base::Exception& error) {
        FAIL() << "cross-document test setup failed: " << error.what();
    }
    catch (const std::exception& error) {
        FAIL() << "cross-document test setup failed: " << error.what();
    }
    catch (...) {
        FAIL() << "cross-document test setup failed with an unknown exception";
    }

    EXPECT_THROW(static_cast<void>(prepare("CrossDocument")), std::invalid_argument);

    auto* python = _document->addObject("App::FeaturePython", "PythonFeature");
    ASSERT_NE(python, nullptr);
    EXPECT_FALSE(python->canRecomputeOnWorker());
    EXPECT_THROW(static_cast<void>(prepare("PythonFeature")), std::invalid_argument);
}

TEST_F(GenericIsolatedRecomputeTest,
       structuralAndPythonOutputPropertiesAreRejectedBeforePublication)
{
    auto* structural = _document->addObject<App::FeatureTestColumn>("StructuralOutput");
    ASSERT_NE(structural, nullptr);
    ASSERT_NE(structural->addDynamicProperty("App::PropertyXLink",
                                             "ResultLink",
                                             "Test",
                                             "Rejected structural output",
                                             App::Prop_Output),
              nullptr);
    EXPECT_THROW(static_cast<void>(prepare("StructuralOutput")),
                 std::invalid_argument);

    auto* python = _document->addObject<App::FeatureTestColumn>("PythonOutput");
    ASSERT_NE(python, nullptr);
    ASSERT_NE(python->addDynamicProperty("App::PropertyPythonObject",
                                         "ResultPython",
                                         "Test",
                                         "Rejected Python output",
                                         App::Prop_Output),
              nullptr);
    EXPECT_THROW(static_cast<void>(prepare("PythonOutput")),
                 std::invalid_argument);
}
