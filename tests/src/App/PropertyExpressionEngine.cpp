// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <thread>
#include <string>

#include "Base/Quantity.h"
#include "Base/Exception.h"

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentCollaborationService.h"
#include "App/DocumentObject.h"
#include "App/DocumentObjectGroup.h"
#include "App/Expression.h"
#include "App/FeatureTest.h"
#include "App/GenericIsolatedRecompute.h"
#include "App/RecomputeHandle.h"
#include "App/ObjectIdentifier.h"
#include "App/PropertyExpressionEngine.h"
#include "App/PropertyLinks.h"

#include "src/App/InitApplication.h"

// clang-format off

class PropertyExpressionEngineTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _doc_name = App::GetApplication().getUniqueDocumentName("test");
        _this_doc = App::GetApplication().newDocument(_doc_name.c_str(), "testUser");
        _this_obj = _this_doc -> addObject("Sketcher::SketchObject");
        _source_name = std::string("this_origin");
        _source_prop = _this_obj -> addDynamicProperty("App::PropertyString", _source_name.c_str()); // property with length as string
        _target_name = std::string("this_length");
        _target_prop = _this_obj -> addDynamicProperty("App::PropertyLength", _target_name.c_str()); // property with reference to source
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_doc_name.c_str());
    }

    std::string doc_name() { return _doc_name; }
    App::Document* this_doc() { return _this_doc; }
    App::DocumentObject* this_obj() { return _this_obj; }
    std::string source_name() { return _source_name; }
    App::Property* source_prop() { return _source_prop; }
    std::string target_name() { return _target_name; }
    App::Property* target_prop() { return _target_prop; }

private:
    std::string _doc_name;
    App::Document* _this_doc {};
    App::DocumentObject* _this_obj {};
    std::string _source_name;
    App::Property* _source_prop {};
    std::string _target_name;
    App::Property* _target_prop {};
};

// https://github.com/FreeCAD/FreeCAD/issues/11965
TEST_F(PropertyExpressionEngineTest, executeCrossPropertyReference)
{
    auto source_text = std::string("1.5 m"); // provided in source
    auto target_text = std::string("1500 mm"); // expected in target

    auto source_path = App::ObjectIdentifier::parse(this_obj(), source_name());
    source_prop()->setPathValue(source_path, source_text);

    auto target_expr = "parsequant(" + source_name() + ")"; // Solution B: parsequant() function

    auto target_path = App::ObjectIdentifier::parse(this_obj(), target_name());
    std::shared_ptr<App::Expression> target_rule(App::Expression::parse(this_obj(), target_expr));
    this_obj()->setExpression(target_path, target_rule);

    this_obj() -> ExpressionEngine.execute();

    auto source_entry = source_prop() -> getPathValue(source_path);
    ASSERT_TRUE(source_entry.type() == typeid(std::string));
    auto source_value = App::any_cast<std::string>(source_entry);

    auto target_entry = target_prop() -> getPathValue(target_path);
    ASSERT_TRUE(target_entry.type() == typeid(Base::Quantity));
    auto target_quant = App::any_cast<Base::Quantity>(target_entry);
    auto target_value = target_quant.getValue();
    auto target_unit = target_quant.getUnit().getString();

    auto verify_quant = Base::Quantity::parse(target_text);

    EXPECT_EQ(target_quant, verify_quant) << ""
        "expecting equal: source_text='" + source_text + "' target_text='" + target_text + "'"
        "instead produced: target_value='" + std::to_string(target_value) + "' target_unit='" + target_unit + "'"
    ;
}

class CrossDocumentExpressionRecomputeTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _directory = std::filesystem::temp_directory_path()
            / ("fc-xdoc-expr-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(_directory);
        _sourceName = App::GetApplication().getUniqueDocumentName("QA_Hole");
        _drivenName = App::GetApplication().getUniqueDocumentName("QA_Driven");
        _source = App::GetApplication().newDocument(_sourceName.c_str(), "sourceUser");
        _driven = App::GetApplication().newDocument(_drivenName.c_str(), "drivenUser");
        _sourceObject = _source->addObject<App::FeatureTest>("Dims");
        _drivenObject = _driven->addObject<App::FeatureTest>("Pad");
        ASSERT_NE(_sourceObject, nullptr);
        ASSERT_NE(_drivenObject, nullptr);
        _sourceObject->Float.setValue(6.0);
        _drivenObject->Float.setValue(10.0);
        ASSERT_TRUE(_source->saveAs((_directory / "qa_hole.FCStd").string().c_str()));
        ASSERT_TRUE(_driven->saveAs((_directory / "qa_driven.FCStd").string().c_str()));
        const std::string expression =
            _sourceName + "#" + _sourceObject->getNameInDocument() + ".Float";
        _drivenObject->setExpression(
            App::ObjectIdentifier(_drivenObject->Float),
            std::shared_ptr<App::Expression>(
                App::Expression::parse(_drivenObject, expression)));
    }

    void TearDown() override
    {
        if (App::GetApplication().getDocument(_drivenName.c_str())) {
            App::GetApplication().closeDocument(_drivenName.c_str());
        }
        if (App::GetApplication().getDocument(_sourceName.c_str())) {
            App::GetApplication().closeDocument(_sourceName.c_str());
        }
        std::error_code error;
        std::filesystem::remove_all(_directory, error);
    }

    std::filesystem::path _directory;
    std::string _sourceName;
    std::string _drivenName;
    App::Document* _source {};
    App::Document* _driven {};
    App::FeatureTest* _sourceObject {};
    App::FeatureTest* _drivenObject {};
};

TEST_F(CrossDocumentExpressionRecomputeTest, openSourceDocumentDrivesTheDependentValue)
{
    _drivenObject->touch();
    EXPECT_NO_THROW(static_cast<void>(_driven->recompute()));
    EXPECT_FALSE(_drivenObject->isError());
    EXPECT_DOUBLE_EQ(_drivenObject->Float.getValue(), 6.0);

    _sourceObject->Float.setValue(9.0);
    _drivenObject->touch();
    EXPECT_NO_THROW(static_cast<void>(_driven->recompute()));
    EXPECT_FALSE(_drivenObject->isError());
    EXPECT_DOUBLE_EQ(_drivenObject->Float.getValue(), 9.0);
}

TEST_F(CrossDocumentExpressionRecomputeTest,
       collaborativeCommitTellsTheCallerToRecomputeDirectly)
{
    // FeatureTest opts out of worker recompute, so it never reaches the
    // closure check. A worker-capable object with a cross-document input does.
    auto* sourceColumn = _source->addObject<App::FeatureTestColumn>("Column");
    auto* drivenColumn = _driven->addObject<App::FeatureTestColumn>("DrivenColumn");
    ASSERT_NE(sourceColumn, nullptr);
    ASSERT_NE(drivenColumn, nullptr);
    ASSERT_TRUE(drivenColumn->canRecomputeOnWorker());
    sourceColumn->Value.setValue(6);
    drivenColumn->Value.setValue(10);
    EXPECT_NO_THROW(static_cast<void>(_source->recompute()));
    _drivenObject->touch();
    EXPECT_NO_THROW(static_cast<void>(_driven->recompute()));
    ASSERT_FALSE(_driven->mustExecute());

    const std::string expression =
        _sourceName + "#" + sourceColumn->getNameInDocument() + ".Value";
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::Structural;
    const auto result = _driven->collaborationService().commitCompatibilityMutation(
        mutation,
        [&] {
            drivenColumn->setExpression(
                App::ObjectIdentifier(drivenColumn->Value),
                std::shared_ptr<App::Expression>(
                    App::Expression::parse(drivenColumn, expression)));
        });

    EXPECT_EQ(result.status, App::DocumentCommitStatus::RecomputeFailed) << result.message;
    EXPECT_NE(result.message.find("unresolved cross-document dependency"), std::string::npos)
        << result.message;
    EXPECT_NE(result.message.find("outside this collaborative commit"), std::string::npos)
        << result.message;
    EXPECT_EQ(drivenColumn->Value.getValue(), 10);
    EXPECT_TRUE(drivenColumn->ExpressionEngine.getExpressions().empty());

    drivenColumn->setExpression(
        App::ObjectIdentifier(drivenColumn->Value),
        std::shared_ptr<App::Expression>(
            App::Expression::parse(drivenColumn, expression)));
    drivenColumn->touch();
    EXPECT_NO_THROW(static_cast<void>(_driven->recompute()));
    EXPECT_FALSE(drivenColumn->isError());
    EXPECT_EQ(drivenColumn->Value.getValue(), 6);
}

TEST_F(CrossDocumentExpressionRecomputeTest, closedSourceDocumentReportsUnresolvedLink)
{
    EXPECT_EQ(_driven->recompute(), 1);
    ASSERT_DOUBLE_EQ(_drivenObject->Float.getValue(), 6.0);

    App::GetApplication().closeDocument(_sourceName.c_str());
    _source = nullptr;
    _sourceObject = nullptr;

    // The expression still names the closed document. Recompute must report
    // that unresolved link instead of walking the destroyed object.
    EXPECT_NO_THROW(static_cast<void>(
        _driven->recompute(std::vector<App::DocumentObject*>(), true)));
    EXPECT_TRUE(_drivenObject->isError());
    const char* diagnostic = _driven->getErrorDescription(_drivenObject);
    ASSERT_NE(diagnostic, nullptr);
    const std::string diagnosticText(diagnostic);
    EXPECT_NE(diagnosticText.find("not found"), std::string::npos) << diagnosticText;
    EXPECT_NE(diagnosticText.find(_sourceName), std::string::npos) << diagnosticText;

    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
    mutation.objectName = _drivenObject->getNameInDocument();
    const auto committed = _driven->collaborationService().commitCompatibilityMutation(
        mutation,
        [this] {
            _drivenObject->touch();
        });
    EXPECT_NE(committed.status, App::DocumentCommitStatus::RollbackFailed) << committed.message;
    EXPECT_TRUE(_drivenObject->isError());
    diagnostic = _driven->getErrorDescription(_drivenObject);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_NE(std::string(diagnostic).find("not found"), std::string::npos) << diagnostic;

    // A later open of a copy under whatever name the file restores must not
    // resurrect the destroyed object pointer.
    const auto copyPath = _directory / "qa_hole_good.FCStd";
    std::filesystem::copy_file(_directory / "qa_hole.FCStd", copyPath);
    auto* reopened = App::GetApplication().openDocument(copyPath.string().c_str());
    ASSERT_NE(reopened, nullptr);
    EXPECT_NO_THROW(static_cast<void>(
        _driven->recompute(std::vector<App::DocumentObject*>(), true)));
    if (std::string(reopened->getName()) != _sourceName) {
        EXPECT_TRUE(_drivenObject->isError());
    }

    if (App::GetApplication().getDocument(reopened->getName())) {
        App::GetApplication().closeDocument(reopened->getName());
    }
}

class CrossDocumentUnrelatedMutationTest:
    public CrossDocumentExpressionRecomputeTest,
    public ::testing::WithParamInterface<bool>
{};

TEST_P(CrossDocumentUnrelatedMutationTest, settledBindingAllowsUnrelatedCollaborativeEdit)
{
    auto* source = (GetParam() ? _source : _driven)
        ->addObject<App::FeatureTestColumn>("SourceColumn");
    auto* driven = _driven->addObject<App::FeatureTestColumn>("DrivenColumn");
    auto* unrelated = _driven->addObject<App::FeatureTestColumn>("UnrelatedColumn");
    auto* group = _driven->addObject<App::DocumentObjectGroup>("Group");
    ASSERT_NE(source, nullptr);
    ASSERT_NE(driven, nullptr);
    ASSERT_NE(unrelated, nullptr);
    ASSERT_NE(group, nullptr);
    source->Column.setValue("C");
    const std::string expression =
        (GetParam() ? _sourceName + "#" : "") + "SourceColumn.Column";
    driven->setExpression(
        App::ObjectIdentifier(driven->Column),
        std::shared_ptr<App::Expression>(App::Expression::parse(driven, expression)));
    ASSERT_FALSE(group->addObject(driven).empty());
    ASSERT_FALSE(group->addObject(unrelated).empty());
    // The shared container connects the edited and driven objects, as a
    // PartDesign Body does for a pad and an unrelated sketch.
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    ASSERT_FALSE(_driven->mustExecute());
    ASSERT_EQ(driven->Value.getValue(), App::decodeColumn("C"));
    // Eager commits still settle edits when the human has disabled automatic
    // recompute. That override must not force clean, unrelated bindings.
    _driven->setStatus(App::Document::SkipRecompute, true);

    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
    mutation.objectName = unrelated->getNameInDocument();
    mutation.stableObjectIdentity = _driven->collaborationObjectIdentity(*unrelated);
    const auto result = _driven->collaborationService().commitCompatibilityMutation(
        mutation, [&] { unrelated->Column.setValue("D"); });

    EXPECT_TRUE(result.committed()) << result.message;
    EXPECT_EQ(unrelated->Column.getStrValue(), "D");
    EXPECT_EQ(unrelated->Value.getValue(), App::decodeColumn("D"));
    EXPECT_EQ(driven->Value.getValue(), App::decodeColumn("C"));
    EXPECT_EQ(driven->ExpressionEngine.getExpressions().size(), 1U);
    EXPECT_FALSE(_driven->mustExecute());
    EXPECT_TRUE(_driven->testStatus(App::Document::SkipRecompute));
}

INSTANTIATE_TEST_SUITE_P(
    Round21,
    CrossDocumentUnrelatedMutationTest,
    ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) {
        return info.param ? "DefectCrossDocument" : "HappySameDocument";
    });

// A profile edit makes the directly bound feature dirty. A separate object
// sharing only a group (Round21 above) leaves that feature clean.
TEST_P(CrossDocumentUnrelatedMutationTest, profileEditReexecutesDirectlyBoundFeature)
{
    auto* source = (GetParam() ? _source : _driven)
        ->addObject<App::FeatureTestColumn>("SourceColumn");
    auto* profile = _driven->addObject<App::FeatureTestColumn>("Profile");
    auto* driven = _driven->addObject<App::FeatureTestColumn>("DrivenColumn");
    auto* profileLink = dynamic_cast<App::PropertyLink*>(
        driven->addDynamicProperty("App::PropertyLink", "Profile"));
    ASSERT_NE(profileLink, nullptr);
    profileLink->setValue(profile);
    source->Column.setValue("C");
    const std::string expression =
        (GetParam() ? _sourceName + "#" : "") + "SourceColumn.Column";
    driven->setExpression(App::ObjectIdentifier(driven->Column),
        std::shared_ptr<App::Expression>(App::Expression::parse(driven, expression)));
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    ASSERT_FALSE(_driven->mustExecute());
    ASSERT_FALSE(driven->ExpressionEngine.isTouched());

    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
    mutation.objectName = profile->getNameInDocument();
    mutation.stableObjectIdentity = _driven->collaborationObjectIdentity(*profile);
    const auto result = _driven->collaborationService().commitCompatibilityMutation(
        mutation, [&] { profile->Column.setValue("D"); });
    EXPECT_TRUE(result.committed()) << result.message;
    EXPECT_EQ(profile->Value.getValue(), App::decodeColumn("D"));
    EXPECT_EQ(driven->Value.getValue(), App::decodeColumn("C"));
    EXPECT_EQ(driven->ExpressionEngine.getExpressions().size(), 1U);
    EXPECT_FALSE(_driven->mustExecute());

    source->Column.setValue("E");
    if (GetParam()) {
        // Dirty external inputs still need an explicit source recompute.
        const auto unsettled = _driven->collaborationService().commitCompatibilityMutation(
            mutation, [&] { profile->Column.setValue("F"); });
        EXPECT_EQ(unsettled.status, App::DocumentCommitStatus::Busy) << unsettled.message;
        EXPECT_NE(unsettled.message.find("document has pending recompute work outside the prepared operation"),
                  std::string::npos) << unsettled.message;
        EXPECT_EQ(profile->Column.getStrValue(), "D");
    }
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    EXPECT_EQ(driven->Value.getValue(), App::decodeColumn("E"));
    EXPECT_FALSE(_driven->mustExecute());
}


// Exercise both sides of the detached preparation boundary without timing
// sleeps: a clean source at capture can change before the operation applies.
class DerivedExternalExpressionTest: public CrossDocumentExpressionRecomputeTest
{
protected:
    void SetUp() override
    {
        CrossDocumentExpressionRecomputeTest::SetUp();
        source = _source->addObject<App::FeatureTestColumn>("SourceColumn");
        profile = _driven->addObject<App::FeatureTestColumn>("Profile");
        driven = _driven->addObject<App::FeatureTestColumn>("DrivenColumn");
        auto* link = dynamic_cast<App::PropertyLink*>(
            driven->addDynamicProperty("App::PropertyLink", "Profile"));
        ASSERT_NE(link, nullptr);
        link->setValue(profile);
        source->Column.setValue("C");
        driven->setExpression(App::ObjectIdentifier(driven->Column),
            std::shared_ptr<App::Expression>(App::Expression::parse(
                driven, _sourceName + "#SourceColumn.Column")));
        static_cast<void>(_source->recompute());
        static_cast<void>(_driven->recompute());
        ASSERT_FALSE(_source->mustExecute());
        ASSERT_FALSE(_driven->mustExecute());
        ASSERT_TRUE(_source->save());
    }

    App::CollaborativeOperationPreparation prepareOnCurrentThread()
    {
        App::Internal::ensureGenericIsolatedRecomputeRegistered();
        return App::CollaborativeOperationRegistry::instance().prepare(*_driven,
            {std::string(App::GenericIsolatedRecomputeOperationType),
             {{"feature", "DrivenColumn"}, {"force_execution", "1"},
              {"owner_thread_execution", "1"}, {"derived_coordinator_recompute", "1"}}});
    }

    App::CollaborativeOperationPreparation prepare()
    {
        std::optional<App::CollaborativeOperationPreparation> preparation;
        static_cast<void>(_driven->collaborationService().runOnOwnerThread([&] {
            preparation.emplace(prepareOnCurrentThread());
            return true;
        }));
        return std::move(*preparation);
    }

    App::DocumentCommitResult apply(const App::CollaborativeOperation& operation)
    {
        App::CollaborationCompatibilityMutation mutation;
        mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
        mutation.objectName = profile->getNameInDocument();
        mutation.stableObjectIdentity = _driven->collaborationObjectIdentity(*profile);
        return _driven->collaborationService().commitCompatibilityMutationWithPolicy(
            mutation, [&] { operation.apply(*_driven); },
            App::CollaborationCompatibilityRecomputePolicy::Deferred);
    }

    void expectRefusal(const App::DocumentCommitResult& result)
    {
        EXPECT_FALSE(result.committed()) << result.message;
        EXPECT_NE(result.message.find("unresolved cross-document dependency"),
                  std::string::npos) << result.message;
        EXPECT_EQ(driven->Value.getValue(), App::decodeColumn("C"));
    }

    void expectPreparationRefusal()
    {
        try {
            static_cast<void>(prepare());
            FAIL() << "accepted an unsafe external source";
        }
        catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find("unresolved cross-document dependency"),
                      std::string::npos) << error.what();
        }
    }

    App::FeatureTestColumn* source {};
    App::FeatureTestColumn* profile {};
    App::FeatureTestColumn* driven {};
};

TEST_F(DerivedExternalExpressionTest, cleanSourceWithDirtyAncestorIsRefused)
{
    auto* ancestor = _source->addObject<App::FeatureTestColumn>("Ancestor");
    ancestor->Column.setValue("C");
    source->setExpression(App::ObjectIdentifier(source->Column),
        std::shared_ptr<App::Expression>(App::Expression::parse(source, "Ancestor.Column")));
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    ancestor->Column.setValue("E");
    ASSERT_FALSE(source->isTouched());
    ASSERT_EQ(source->mustRecompute(), 0);
    ASSERT_TRUE(ancestor->isTouched());
    expectPreparationRefusal();
    // The compatibility preflight must also refuse this unsettled dependent.
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
    mutation.objectName = "Profile";
    mutation.stableObjectIdentity = _driven->collaborationObjectIdentity(*profile);
    const auto result = _driven->collaborationService().commitCompatibilityMutation(
        mutation, [&] { profile->Column.setValue("D"); });
    EXPECT_EQ(result.status, App::DocumentCommitStatus::Busy) << result.message;
    EXPECT_NE(result.message.find("document has pending recompute work outside the prepared operation"),
              std::string::npos) << result.message;
    EXPECT_EQ(profile->Column.getStrValue(), "A");
}

TEST_F(DerivedExternalExpressionTest, pendingForcedSourceRecomputeIsRefusedAtPreparation)
{
    auto pending = _source->recomputeAsync({source}, true, 0, App::RecomputeVenue::Isolated);
    ASSERT_TRUE(_source->recomputeCoordinator().hasPendingWork());
    ASSERT_FALSE(source->isTouched());
    ASSERT_EQ(source->mustRecompute(), 0);
    expectPreparationRefusal();
    static_cast<void>(pending->cancel());
}

TEST_F(DerivedExternalExpressionTest, pendingForcedSourceRecomputeIsRefusedAtEvaluation)
{
    auto preparation = prepare();
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    auto pending = _source->recomputeAsync({source}, true, 0, App::RecomputeVenue::Isolated);
    ASSERT_TRUE(_source->recomputeCoordinator().hasPendingWork());
    ASSERT_FALSE(source->isTouched());
    expectRefusal(apply(*operation));
    static_cast<void>(pending->cancel());
}

TEST_F(DerivedExternalExpressionTest, settledSourceMutationAfterPreparationIsRefused)
{
    auto preparation = prepare();
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    source->Column.setValue("E");
    static_cast<void>(_source->recompute());
    ASSERT_FALSE(source->isTouched());
    ASSERT_FALSE(_source->mustExecute());
    expectRefusal(apply(*operation));
}

TEST_F(DerivedExternalExpressionTest, ownerFallbackStillRefusesChangedExternalSource)
{
    auto* unopted = _driven->addObject("App::FeaturePython", "OwnerOnlyInput");
    ASSERT_FALSE(unopted->canRecomputeOnWorker());
    auto* link = dynamic_cast<App::PropertyLink*>(
        driven->addDynamicProperty("App::PropertyLink", "OwnerOnlyInput"));
    ASSERT_NE(link, nullptr);
    link->setValue(unopted);
    static_cast<void>(_driven->recompute());
    auto preparation = prepare();
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    source->Column.setValue("E");
    static_cast<void>(_source->recompute());
    expectRefusal(apply(*operation));
}

TEST_F(DerivedExternalExpressionTest, sourceCloseAfterPreparationIsRefused)
{
    auto preparation = prepare();
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    ASSERT_TRUE(App::GetApplication().closeDocument(_sourceName.c_str()));
    _source = nullptr;
    _sourceObject = nullptr;
    source = nullptr;
    expectRefusal(apply(*operation));
}

TEST_F(DerivedExternalExpressionTest, sourceReloadAfterPreparationIsRefused)
{
    auto preparation = prepare();
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    _source->restore((_directory / "qa_hole.FCStd").string().c_str());
    static_cast<void>(_source->afterRestore());
    static_cast<void>(_source->recompute());
    ASSERT_FALSE(_source->mustExecute());
    expectRefusal(apply(*operation));
}

TEST_F(DerivedExternalExpressionTest, sourceReloadDuringEvaluationIsRefused)
{
    driven->Value.setValue(0);
    auto preparation = prepare();
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    bool attempted = false;
    bool refused = false;
    auto connection = driven->signalBeforeChange.connect(
        [&](const App::DocumentObject&, const App::Property& property) {
            if (attempted || &property != &driven->Value) {
                return;
            }
            attempted = true;
            try {
                _source->restore((_directory / "qa_hole.FCStd").string().c_str());
            }
            catch (const Base::Exception&) {
                refused = true;
            }
        });
    const auto result = apply(*operation);
    connection.disconnect();
    EXPECT_TRUE(attempted);
    EXPECT_TRUE(refused) << "reload invalidated a source during evaluation";
    EXPECT_EQ(_source->getObject("SourceColumn"), source);
    EXPECT_TRUE(result.committed()) << result.message;
    EXPECT_EQ(driven->Value.getValue(), App::decodeColumn("C"));
}

TEST_F(DerivedExternalExpressionTest, foreignThreadSourceReadIsRefused)
{
    // Both documents belong to the test thread. Unsupported foreign-thread
    // reads must be rejected before inspecting source objects or expressions.
    std::thread foreign([&] {
        try {
            static_cast<void>(prepareOnCurrentThread());
            FAIL() << "accepted a foreign-thread source read";
        }
        catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find("unresolved cross-document dependency"),
                      std::string::npos) << error.what();
        }
    });
    foreign.join();
}

// clang-format on
