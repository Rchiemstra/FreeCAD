// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "Base/Quantity.h"

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentCollaborationService.h"
#include "App/DocumentObject.h"
#include "App/Expression.h"
#include "App/FeatureTest.h"
#include "App/ObjectIdentifier.h"
#include "App/PropertyExpressionEngine.h"

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

// clang-format on
