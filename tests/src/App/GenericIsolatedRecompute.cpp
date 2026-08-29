// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperation.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/FeatureTest.h>
#include <App/GenericIsolatedRecompute.h>
#include <App/GeometryWorkerOperationRegistry.h>
#include <App/PropertyLinks.h>
#include <App/PropertyPythonObject.h>
#include <App/Range.h>
#include <Base/Exception.h>
#include <src/App/InitApplication.h>

#include <algorithm>
#include <stdexcept>
#include <stop_token>
#include <string>
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

    App::CollaborativeOperationPreparation prepare(const std::string& featureName)
    {
        return App::CollaborativeOperationRegistry::instance().prepare(
            *_document,
            {std::string(App::GenericIsolatedRecomputeOperationType),
             {{"feature", featureName}}});
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
    operation->apply(*_document);
    EXPECT_EQ(feature->Value.getValue(), App::decodeColumn("C"));
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
