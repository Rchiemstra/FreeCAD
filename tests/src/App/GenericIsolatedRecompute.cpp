// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperation.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/DocumentObjectPy.h>
#include <App/Expression.h>
#include <App/FeatureTest.h>
#include <App/GenericIsolatedRecompute.h>
#include <App/GeometryWorkerOperationRegistry.h>
#include <App/Link.h>
#include <App/ObjectIdentifier.h>
#include <App/PropertyLinks.h>
#include <App/PropertyPythonObject.h>
#include <App/Range.h>
#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <src/App/InitApplication.h>

#include <algorithm>
#include <chrono>
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

class DerivedWorkerFeature final: public App::FeatureTestColumn
{};

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
        auto* feature = _document->getObject(featureName.c_str());
        if (feature) {
            intent.arguments.emplace(
                "stable_object_identity",
                _document->collaborationObjectIdentity(*feature));
        }
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

    void executeAndApply(const std::string& featureName)
    {
        auto preparation = prepare(featureName);
        ASSERT_EQ(preparation.policy, App::PreparationPolicy::IsolatedProcess);
        ASSERT_NE(preparation.isolatedTask, nullptr);
        const auto output =
            App::Internal::GeometryWorkerOperationRegistry::instance().execute(
                std::string(App::GenericIsolatedRecomputeOperationType),
                preparation.isolatedTask->inputArchive,
                std::stop_token {});
        auto operation = preparation.isolatedTask->decodeResult(output);
        ASSERT_NE(operation, nullptr);
        operation->apply(*_document);
        const auto postcondition = operation->checkPostcondition(*_document);
        EXPECT_TRUE(postcondition.satisfied) << postcondition.message;
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

void addGroupExtension(App::DocumentObject& object)
{
    Base::PyGILStateLocker gil;
    Py::Object pythonObject(object.getPyObject(), true);
    Py::Tuple args(1);
    args.setItem(0, Py::String("App::GroupExtensionPython"));
    static_cast<void>(pythonObject.callMemberFunction("addExtension", args));
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
       expressionsAreRejectedBeforeArchivingOrWorkerExecution)
{
    auto* feature = _document->addObject<App::FeatureTest>("PlacementExpression");
    ASSERT_NE(feature, nullptr);
    feature->Distance.setValue(23.5);
    feature->setExpression(
        App::ObjectIdentifier::parse(feature, "Placement.Base.x"),
        std::shared_ptr<App::Expression>(App::Expression::parse(feature, "Distance")));
    ASSERT_DOUBLE_EQ(feature->Placement.getValue().getPosition().x, 0.0);

    EXPECT_THROW(static_cast<void>(prepare("PlacementExpression")),
                 std::invalid_argument);
    EXPECT_DOUBLE_EQ(feature->Placement.getValue().getPosition().x, 0.0);
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
        {{"feature", "NoRecomputeStorage"},
         {"stable_object_identity",
          _document->collaborationObjectIdentity(*storage)},
         {"force_execution", "1"}}};
    EXPECT_THROW(
        static_cast<void>(App::CollaborativeOperationRegistry::instance().prepare(
            *_document, forcedIntent)),
        std::invalid_argument);
}

TEST_F(GenericIsolatedRecomputeTest,
       appLinkTargetIsRejectedBecauseItsExecutePublishesViewRefreshState)
{
    auto* other = createOtherDocument();
    ASSERT_NE(other, nullptr);
    auto* source = other->addObject<App::FeatureTest>("Source");
    ASSERT_NE(source, nullptr);
    source->purgeTouched();
    _document->FileName.setValue(
        App::Application::getTempFileName("generic-link-owner.FCStd"));
    other->FileName.setValue(
        App::Application::getTempFileName("generic-link-source.FCStd"));

    auto* link = _document->addObject<App::Link>("ExternalLink");
    ASSERT_NE(link, nullptr);
    link->LinkedObject.setValue(source);
    ASSERT_TRUE(link->isTouched());
    ASSERT_EQ(link->mustExecute(), 0);
    ASSERT_TRUE(link->mustRecompute());

    EXPECT_THROW(static_cast<void>(prepare("ExternalLink")),
                 std::invalid_argument);
    EXPECT_EQ(link->LinkedObject.getValue(), source);
    EXPECT_TRUE(link->isTouched());
    EXPECT_TRUE(link->mustRecompute());
}

TEST_F(GenericIsolatedRecomputeTest,
       plainSameDocumentAppLinkUsesNarrowEnforceOnlyBookkeeping)
{
    auto* source = _document->addObject<App::FeatureTest>("Source");
    ASSERT_NE(source, nullptr);
    source->purgeTouched();

    auto* link = _document->addObject<App::Link>("LocalLink");
    ASSERT_NE(link, nullptr);
    link->LinkedObject.setValue(source);
    ASSERT_TRUE(link->isTouched());
    ASSERT_EQ(link->mustExecute(), 0);
    ASSERT_TRUE(link->mustRecompute());

    const int recomputed = _document->recompute({link});
    EXPECT_EQ(recomputed, 1)
        << (_document->getErrorDescription(link)
                ? _document->getErrorDescription(link)
                : "no recompute diagnostic");
    EXPECT_EQ(link->LinkedObject.getValue(), source);
    EXPECT_FALSE(link->isTouched());
    EXPECT_FALSE(link->mustRecompute());
    EXPECT_TRUE(link->isValid());
}

TEST_F(GenericIsolatedRecomputeTest,
       nullProxyFeaturePythonHoldingOnlySafeExternalLinkUsesBookkeeping)
{
    auto* other = createOtherDocument();
    ASSERT_NE(other, nullptr);
    auto* source = other->addObject<App::FeatureTest>("Source");
    ASSERT_NE(source, nullptr);
    source->purgeTouched();
    _document->FileName.setValue(
        App::Application::getTempFileName("generic-holder-owner.FCStd"));
    other->FileName.setValue(
        App::Application::getTempFileName("generic-holder-source.FCStd"));

    auto* link = _document->addObject<App::Link>("ExternalLink");
    ASSERT_NE(link, nullptr);
    link->LinkedObject.setValue(source);
    ASSERT_EQ(_document->recompute({link}), 1);
    ASSERT_FALSE(link->mustRecompute());

    auto* holder = _document->addObject("App::FeaturePython", "Holder");
    ASSERT_NE(holder, nullptr);
    auto* support = dynamic_cast<App::PropertyLink*>(holder->addDynamicProperty(
        "App::PropertyLink", "Support", "Data"));
    ASSERT_NE(support, nullptr);
    support->setValue(link);
    ASSERT_TRUE(holder->mustExecute());

    auto preparation = prepare("Holder");
    EXPECT_EQ(preparation.policy, App::PreparationPolicy::DetachedInProcess);
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    ASSERT_NE(operation, nullptr);
    operation->apply(*_document);
    const auto postcondition = operation->checkPostcondition(*_document);
    EXPECT_TRUE(postcondition.satisfied) << postcondition.message;
    EXPECT_EQ(support->getValue(), link);
    EXPECT_FALSE(holder->mustRecompute());
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
       dynamicConfiguredLinkSchemaIsRejectedBeforeArchiving)
{
    auto* feature = _document->addObject<App::FeatureTest>("PartialLinkBaseline");
    ASSERT_NE(feature, nullptr);
    auto* links = dynamic_cast<App::PropertyXLinkSubList*>(
        feature->addDynamicProperty("App::PropertyXLinkSubList",
                                    "ConfiguredLinks",
                                    "Test",
                                    "Configured non-output link property",
                                    App::Prop_NoRecompute));
    ASSERT_NE(links, nullptr);
    links->setAllowPartial(true);

    EXPECT_THROW(static_cast<void>(prepare("PartialLinkBaseline")),
                 std::invalid_argument);
    EXPECT_EQ(links->getSize(), 0);
}

TEST_F(GenericIsolatedRecomputeTest,
       everyAuditedTestFeatureHasANativeWorkerRoundTrip)
{
    auto* row = _document->addObject<App::FeatureTestRow>("Row");
    auto* address =
        _document->addObject<App::FeatureTestAbsAddress>("AbsoluteAddress");
    auto* placement =
        _document->addObject<App::FeatureTestPlacement>("Placement");
    ASSERT_NE(row, nullptr);
    ASSERT_NE(address, nullptr);
    ASSERT_NE(placement, nullptr);

    row->Row.setValue("12");
    row->Silent.setValue(true);
    address->Address.setValue("$A$12");
    placement->Input1.setValue(
        Base::Placement(Base::Vector3d(10, 20, 30), Base::Rotation()));
    placement->Input2.setValue(
        Base::Placement(Base::Vector3d(1, 2, 3), Base::Rotation()));

    executeAndApply("Row");
    executeAndApply("AbsoluteAddress");
    executeAndApply("Placement");

    EXPECT_EQ(row->Value.getValue(), 11);
    EXPECT_TRUE(address->Valid.getValue());
    EXPECT_EQ(placement->MultLeft.getValue().getPosition(),
              Base::Vector3d(11, 22, 33));
    EXPECT_EQ(placement->MultRight.getValue().getPosition(),
              Base::Vector3d(11, 22, 33));
}

TEST_F(GenericIsolatedRecomputeTest,
       workerSnapshotComparesSameDocumentLinkPropertiesWithoutDetachedOwners)
{
    auto* source = _document->addObject<App::FeatureTest>("Source");
    auto* link = _document->addObject<App::Link>("LocalLink");
    auto* target = _document->addObject<App::FeatureTest>("Target");
    ASSERT_NE(source, nullptr);
    ASSERT_NE(link, nullptr);
    ASSERT_NE(target, nullptr);
    source->purgeTouched();
    link->LinkedObject.setValue(source);
    link->purgeTouched();
    target->Source1.setValue(link);

    auto preparation = prepare("Target");
    ASSERT_EQ(preparation.policy, App::PreparationPolicy::IsolatedProcess);
    ASSERT_NE(preparation.isolatedTask, nullptr);

    App::GeometryArchive output;
    EXPECT_NO_THROW(
        output = App::Internal::GeometryWorkerOperationRegistry::instance().execute(
            std::string(App::GenericIsolatedRecomputeOperationType),
            preparation.isolatedTask->inputArchive,
            std::stop_token {}));
    auto operation = preparation.isolatedTask->decodeResult(output);
    ASSERT_NE(operation, nullptr);
    operation->apply(*_document);
    EXPECT_EQ(link->LinkedObject.getValue(), source);
    EXPECT_TRUE(target->isValid());
    EXPECT_FALSE(target->mustRecompute());
}

TEST_F(GenericIsolatedRecomputeTest,
       documentFeatureFacadeUsesTheSynchronousCompatibilityKernel)
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
    // The legacy single-feature facade executes immediately but does not
    // settle the touched bit; a document-wide recompute owns that lifecycle.
    EXPECT_TRUE(feature->mustRecompute());
    EXPECT_TRUE(feature->isValid());
}

TEST_F(GenericIsolatedRecomputeTest,
       documentFeatureFacadeExecutesWorkerUnsafeFeatureInProcess)
{
    App::FeatureTestAsyncBlocker::resetBlocker();
    App::FeatureTestAsyncBlocker::releaseBlocker();
    auto* feature =
        _document->addObject<App::FeatureTestAsyncBlocker>("SyncWorkerUnsafeFeature");
    ASSERT_NE(feature, nullptr);
    ASSERT_FALSE(feature->canRecomputeOnWorker());

    const bool recomputed = feature->recomputeFeature(false);

    EXPECT_TRUE(recomputed)
        << (_document->getErrorDescription(feature)
                ? _document->getErrorDescription(feature)
                : "no recompute diagnostic");
    EXPECT_TRUE(
        App::FeatureTestAsyncBlocker::waitUntilStarted(std::chrono::milliseconds(0)));
    EXPECT_TRUE(feature->mustRecompute());
    EXPECT_TRUE(feature->isValid());
    App::FeatureTestAsyncBlocker::resetBlocker();
}

TEST_F(GenericIsolatedRecomputeTest,
       documentWideFacadeExecutesWorkerUnsafeFeatureInProcess)
{
    App::FeatureTestAsyncBlocker::resetBlocker();
    App::FeatureTestAsyncBlocker::releaseBlocker();
    auto* feature =
        _document->addObject<App::FeatureTestAsyncBlocker>("SyncDocumentWorkerUnsafeFeature");
    ASSERT_NE(feature, nullptr);
    ASSERT_FALSE(feature->canRecomputeOnWorker());

    bool hasError = true;
    const int recomputed = _document->recompute({}, false, &hasError);

    EXPECT_EQ(recomputed, 1);
    EXPECT_FALSE(hasError);
    EXPECT_TRUE(
        App::FeatureTestAsyncBlocker::waitUntilStarted(std::chrono::milliseconds(0)));
    EXPECT_FALSE(feature->mustRecompute());
    EXPECT_TRUE(feature->isValid());
    App::FeatureTestAsyncBlocker::resetBlocker();
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
        auto* object = _document->getObject(feature.featureId.c_str());
        ASSERT_NE(object, nullptr);
        EXPECT_EQ(feature.stableObjectIdentity,
                  _document->collaborationObjectIdentity(*object));
        EXPECT_EQ(feature.intent.arguments.at("stable_object_identity"),
                  feature.stableObjectIdentity);
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
    EXPECT_THROW(
        static_cast<void>(registry.prepare(
            *_document,
            {std::string(App::GenericIsolatedRecomputeOperationType),
             {{"feature", "Column"}, {"owner_thread_execution", "1"}}})),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(App::Internal::makeGenericIsolatedRecomputeRequest(
            *_document,
            *feature,
            false,
            true,
            /*ownerThreadExecution=*/true)),
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
       crossDocumentClosureThroughCanonicalLinkIsRejected)
{
    auto* target = _document->addObject<App::FeatureTest>("CrossDocument");
    ASSERT_NE(target, nullptr);
    auto* other = createOtherDocument();
    ASSERT_NE(other, nullptr);
    auto* external = other->addObject<App::FeatureTest>("External");
    ASSERT_NE(external, nullptr);
    _document->FileName.setValue(
        App::Application::getTempFileName("generic-recompute-owner.FCStd"));
    other->FileName.setValue(
        App::Application::getTempFileName("generic-recompute-external.FCStd"));
    auto* externalLink = _document->addObject<App::Link>("ExternalSource");
    ASSERT_NE(externalLink, nullptr);
    try {
        externalLink->LinkedObject.setValue(external);
        target->Source1.setValue(externalLink);
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
}

TEST_F(GenericIsolatedRecomputeTest,
       featurePythonExecutionIsCategoricallyRejected)
{
    // A scripted feature's execute() lives in a Python proxy that the worker
    // archive cannot carry. The historical thread-affinity hook is not an
    // isolated result-contract opt-in.
    auto* python = _document->addObject("App::FeaturePython", "PythonFeature");
    ASSERT_NE(python, nullptr);
    EXPECT_FALSE(python->canRecomputeOnWorker());
    EXPECT_THROW(static_cast<void>(prepare("PythonFeature")), std::invalid_argument);
}

TEST_F(GenericIsolatedRecomputeTest,
       workerUnsafeTouchedBookkeepingCandidateIsRejectedWithoutExecuting)
{
    App::FeatureTestAsyncBlocker::resetBlocker();
    auto* feature =
        _document->addObject<App::FeatureTestAsyncBlocker>("UnsafeBookkeepingCandidate");
    ASSERT_NE(feature, nullptr);
    feature->purgeTouched();
    feature->touch(/*noRecompute=*/true);
    ASSERT_TRUE(feature->isTouched());
    ASSERT_EQ(feature->mustRecompute(), 0);

    EXPECT_THROW(
        static_cast<void>(prepare("UnsafeBookkeepingCandidate")),
        std::invalid_argument);
    EXPECT_FALSE(
        App::FeatureTestAsyncBlocker::waitUntilStarted(std::chrono::milliseconds(0)));
}

TEST_F(GenericIsolatedRecomputeTest,
       dynamicStructuralAndPythonPropertiesAreRejectedBeforeArchiving)
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

    auto* pythonInput =
        _document->addObject<App::FeatureTestColumn>("PythonInput");
    ASSERT_NE(pythonInput, nullptr);
    ASSERT_NE(pythonInput->addDynamicProperty("App::PropertyPythonObject",
                                              "InputPython",
                                              "Test",
                                              "Rejected Python input"),
              nullptr);
    EXPECT_THROW(static_cast<void>(prepare("PythonInput")),
                 std::invalid_argument);
}

TEST_F(GenericIsolatedRecomputeTest,
       archiveOmittedAndRuntimeOnlyValuesMustMatchConstructorState)
{
    auto* dynamic =
        _document->addObject<App::FeatureTestColumn>("PlainDynamicProperty");
    ASSERT_NE(dynamic, nullptr);
    ASSERT_NE(dynamic->addDynamicProperty("App::PropertyInteger",
                                          "DynamicInput",
                                          "Test",
                                          "Rejected dynamic input"),
              nullptr);
    EXPECT_THROW(static_cast<void>(prepare("PlainDynamicProperty")),
                 std::invalid_argument);

    auto* noPersist =
        _document->addObject<App::FeatureTestColumn>("DynamicNoPersist");
    ASSERT_NE(noPersist, nullptr);
    auto* omitted = dynamic_cast<App::PropertyInteger*>(
        noPersist->addDynamicProperty("App::PropertyInteger",
                                      "OmittedInput",
                                      "Test",
                                      "Rejected no-persist input",
                                      App::Prop_NoPersist));
    ASSERT_NE(omitted, nullptr);
    omitted->setValue(91);
    EXPECT_THROW(static_cast<void>(prepare("DynamicNoPersist")),
                 std::invalid_argument);

    auto* runtimeStatus =
        _document->addObject<App::FeatureTestColumn>("RuntimeTransient");
    ASSERT_NE(runtimeStatus, nullptr);
    runtimeStatus->Column.setStatus(App::Property::Transient, true);
    runtimeStatus->Column.setValue("D");
    EXPECT_THROW(static_cast<void>(prepare("RuntimeTransient")),
                 std::invalid_argument);

    auto* staticTransient =
        _document->addObject<App::FeatureTest>("StaticTransient");
    ASSERT_NE(staticTransient, nullptr);
    staticTransient->TypeTransient.setValue(8128);
    EXPECT_THROW(static_cast<void>(prepare("StaticTransient")),
                 std::invalid_argument);

    auto* configuredLink =
        _document->addObject<App::FeatureTest>("ConfiguredStaticLink");
    ASSERT_NE(configuredLink, nullptr);
    configuredLink->Source1.setScope(App::LinkScope::Global);
    EXPECT_THROW(static_cast<void>(prepare("ConfiguredStaticLink")),
                 std::invalid_argument);
}

TEST_F(GenericIsolatedRecomputeTest,
       exactRuntimeTypeAndCanonicalExtensionSetAreRequired)
{
    auto derivedOwner = std::make_unique<DerivedWorkerFeature>();
    auto* derived = derivedOwner.get();
    _document->addObject(derivedOwner.get(), "UnregisteredDerived");
    static_cast<void>(derivedOwner.release());
    ASSERT_FALSE(derived->canRecomputeOnWorker());
    EXPECT_THROW(static_cast<void>(prepare("UnregisteredDerived")),
                 std::invalid_argument);

    auto* extendedTarget =
        _document->addObject<App::FeatureTest>("ExtendedTarget");
    ASSERT_NE(extendedTarget, nullptr);
    ASSERT_NO_THROW(addGroupExtension(*extendedTarget));
    EXPECT_THROW(static_cast<void>(prepare("ExtendedTarget")),
                 std::invalid_argument);

    auto* extendedDependency =
        _document->addObject<App::FeatureTest>("ExtendedDependency");
    auto* target = _document->addObject<App::FeatureTest>("ExtensionConsumer");
    ASSERT_NE(extendedDependency, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NO_THROW(addGroupExtension(*extendedDependency));
    target->Source1.setValue(extendedDependency);
    EXPECT_THROW(static_cast<void>(prepare("ExtensionConsumer")),
                 std::invalid_argument);

    auto* source = _document->addObject<App::FeatureTest>("LinkSource");
    auto* extendedLink = _document->addObject<App::Link>("ExtendedLink");
    auto* linkConsumer =
        _document->addObject<App::FeatureTest>("ExtendedLinkConsumer");
    ASSERT_NE(source, nullptr);
    ASSERT_NE(extendedLink, nullptr);
    ASSERT_NE(linkConsumer, nullptr);
    source->purgeTouched();
    extendedLink->LinkedObject.setValue(source);
    extendedLink->purgeTouched();
    ASSERT_NO_THROW(addGroupExtension(*extendedLink));
    const auto groupExtension =
        Base::Type::fromName("App::GroupExtensionPython");
    ASSERT_FALSE(groupExtension.isBad());
    ASSERT_TRUE(extendedLink->hasExtension(groupExtension, false));
    EXPECT_THROW(static_cast<void>(prepare("ExtendedLink")),
                 std::invalid_argument);
    linkConsumer->Source1.setValue(extendedLink);
    EXPECT_THROW(static_cast<void>(prepare("ExtendedLinkConsumer")),
                 std::invalid_argument);
}
