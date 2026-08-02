// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/Application.h"
#include "App/CollaborativeSetPropertyOperation.h"
#include "App/Document.h"
#include "App/DocumentCollaborationService.h"
#include "App/DocumentRevisionIndex.h"
#include "App/FeatureTest.h"
#include "App/PropertyStandard.h"
#include <src/App/InitApplication.h>

#include <algorithm>
#include <stdexcept>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace App;

namespace
{

class CollaborativeSetPropertyOperationTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        ensureCollaborativeSetPropertyOperationRegistered();
    }

    void SetUp() override
    {
        _documentName = App::GetApplication().getUniqueDocumentName("collaborativeSetProperty");
        _document = App::GetApplication().newDocument(_documentName.c_str(), "Set property test");
        _target = _document->addObject<FeatureTest>("Target");
        _flag = dynamic_cast<PropertyBool*>(
            _target->addDynamicProperty("App::PropertyBool", "Flag"));
        _count = dynamic_cast<PropertyInteger*>(
            _target->addDynamicProperty("App::PropertyInteger", "Count"));
        _ratio = dynamic_cast<PropertyFloat*>(
            _target->addDynamicProperty("App::PropertyFloat", "Ratio"));
        _text = dynamic_cast<PropertyString*>(
            _target->addDynamicProperty("App::PropertyString", "Text"));
        ASSERT_NE(_flag, nullptr);
        ASSERT_NE(_count, nullptr);
        ASSERT_NE(_ratio, nullptr);
        ASSERT_NE(_text, nullptr);
        _flag->setValue(false);
        _count->setValue(3);
        _ratio->setValue(1.5);
        _text->setValue("before");
        _document->recompute();
        _session = _document->collaborationService().beginEditSession("actor-a");
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_documentName.c_str());
    }

    CollaborativeOperationIntent intent(std::string property,
                                        std::string valueType,
                                        std::string value) const
    {
        return {std::string(CollaborativeSetPropertyOperationType),
                {{"object", "Target"},
                 {"property", std::move(property)},
                 {"value_type", std::move(valueType)},
                 {"value", std::move(value)}}};
    }

    PreparedEdit prepare(std::string operationId,
                         const CollaborativeOperationIntent& operationIntent)
    {
        return _document->collaborationService().prepareEdit(_session.sessionId(),
                                                             std::move(operationId),
                                                             operationIntent,
                                                             "native-test");
    }

    Document* _document {nullptr};
    DocumentObject* _target {nullptr};
    PropertyBool* _flag {nullptr};
    PropertyInteger* _count {nullptr};
    PropertyFloat* _ratio {nullptr};
    PropertyString* _text {nullptr};
    EditSession _session {"placeholder", "placeholder", 1};
    std::string _documentName;
};

}  // namespace

TEST_F(CollaborativeSetPropertyOperationTest, preparesConservativeContractAndCommits)
{
    auto prepared = prepare("set-flag", intent("Flag", "bool", "true"));

    EXPECT_EQ(prepared.readSet(),
              (std::vector<DocumentRevisionKey> {DocumentRevisionKey::objectExistence("Target"),
                                                  DocumentRevisionKey::objectModel("Target"),
                                                  DocumentRevisionKey::objectStructure("Target"),
                                                  DocumentRevisionKey::documentStructure(),
                                                  DocumentRevisionKey::unknownModelMutation()}));
    EXPECT_EQ(prepared.writeSet(),
              (std::vector<DocumentRevisionKey> {DocumentRevisionKey::objectModel("Target")}));
    ASSERT_EQ(prepared.publicationEffects().size(), 1U);
    EXPECT_EQ(prepared.publicationEffects().front().key,
              DocumentRevisionKey::objectModel("Target"));
    EXPECT_EQ(prepared.publicationEffects().front().stableObjectIdentity,
              std::optional<std::string>(_document->collaborationObjectIdentity(*_target)));
    EXPECT_FALSE(prepared.operation().checkPostcondition(*_document).satisfied);

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    EXPECT_TRUE(result.committed());
    EXPECT_TRUE(_flag->getValue());
    EXPECT_TRUE(prepared.operation().checkPostcondition(*_document).satisfied);
}

TEST_F(CollaborativeSetPropertyOperationTest, preparedOperationOwnsCallerIntentByValue)
{
    auto operationIntent = intent("Text", "string", "prepared");
    auto prepared = prepare("set-text", operationIntent);
    operationIntent.arguments["object"] = "Missing";
    operationIntent.arguments["property"] = "Flag";
    operationIntent.arguments["value_type"] = "bool";
    operationIntent.arguments["value"] = "true";

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    EXPECT_TRUE(result.committed());
    EXPECT_EQ(_text->getStrValue(), "prepared");
    EXPECT_FALSE(_flag->getValue());
}

TEST_F(CollaborativeSetPropertyOperationTest, supportsNativeScalarPropertyTypes)
{
    auto boolean = prepare("set-bool", intent("Flag", "bool", "true"));
    ASSERT_TRUE(
        _document->collaborationService().commitEdit(_session.sessionId(), boolean).committed());
    auto integer = prepare("set-integer", intent("Count", "integer", "-42"));
    ASSERT_TRUE(
        _document->collaborationService().commitEdit(_session.sessionId(), integer).committed());
    auto floating = prepare("set-float", intent("Ratio", "float", "2.75"));
    ASSERT_TRUE(
        _document->collaborationService().commitEdit(_session.sessionId(), floating).committed());
    auto string = prepare("set-string", intent("Text", "string", "after"));
    ASSERT_TRUE(
        _document->collaborationService().commitEdit(_session.sessionId(), string).committed());

    EXPECT_TRUE(_flag->getValue());
    EXPECT_EQ(_count->getValue(), -42);
    EXPECT_DOUBLE_EQ(_ratio->getValue(), 2.75);
    EXPECT_EQ(_text->getStrValue(), "after");
}

TEST_F(CollaborativeSetPropertyOperationTest, rejectsInvalidObjectPropertyTypeAndValue)
{
    auto missingObject = intent("Text", "string", "after");
    missingObject.arguments["object"] = "Missing";
    EXPECT_THROW(static_cast<void>(prepare("missing-object", std::move(missingObject))),
                 std::invalid_argument);

    EXPECT_THROW(static_cast<void>(prepare("missing-property",
                                           intent("Missing", "string", "after"))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(prepare("wrong-type", intent("Text", "integer", "3"))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(prepare("bad-bool", intent("Flag", "bool", "yes"))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(prepare("bad-integer", intent("Count", "integer", "3.5"))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(prepare("bad-float", intent("Ratio", "float", "nan"))),
                 std::invalid_argument);
}

TEST_F(CollaborativeSetPropertyOperationTest,
       rejectsSemanticSubclassAndNonEditablePropertyTargets)
{
    ASSERT_NE(_target->addDynamicProperty("App::PropertyPersistentObject", "Persistent"), nullptr);
    EXPECT_THROW(static_cast<void>(prepare("constraint-integer",
                                           intent("ConstraintInt", "integer", "4"))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(prepare("constraint-float",
                                           intent("ConstraintFloat", "float", "4.0"))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(prepare("quantity", intent("QuantityLength", "float", "4"))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(prepare("persistent",
                                           intent("Persistent", "string", "Target"))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(prepare("label", intent("Label", "string", "after"))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(prepare("read-only",
                                           intent("TypeReadOnly", "integer", "4"))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(prepare("output", intent("TypeOutput", "integer", "4"))),
                 std::invalid_argument);
}

TEST_F(CollaborativeSetPropertyOperationTest,
       liveResolutionRejectsStaleIdentityPropertyTypeAndEditability)
{
    auto staleIdentity = prepare("stale-identity", intent("Text", "string", "after"));
    _document->removeObject("Target");
    auto* replacement = _document->addObject<FeatureTest>("Target");
    ASSERT_NE(replacement, nullptr);
    ASSERT_STREQ(replacement->getNameInDocument(), "Target");
    auto* replacementFlag = replacement->addDynamicProperty("App::PropertyBool", "Flag");
    auto* replacementText = replacement->addDynamicProperty("App::PropertyString", "Text");
    ASSERT_NE(replacementFlag, nullptr);
    ASSERT_NE(replacementText, nullptr);
    EXPECT_NE(_document->collaborationObjectIdentity(*replacement),
              staleIdentity.publicationEffects().front().stableObjectIdentity.value());
    EXPECT_THROW(staleIdentity.operation().apply(*_document), std::runtime_error);
    EXPECT_FALSE(staleIdentity.operation().checkPostcondition(*_document).satisfied);

    auto staleType = prepare("stale-type", intent("Flag", "bool", "true"));
    ASSERT_TRUE(replacement->removeDynamicProperty("Flag"));
    replacementFlag = replacement->addDynamicProperty("App::PropertyInteger", "Flag");
    ASSERT_NE(replacementFlag, nullptr);
    EXPECT_THROW(staleType.operation().apply(*_document), std::runtime_error);
    EXPECT_FALSE(staleType.operation().checkPostcondition(*_document).satisfied);

    auto staleEditable = prepare("stale-editable", intent("Text", "string", "after"));
    replacementText = replacement->getPropertyByName("Text");
    ASSERT_NE(replacementText, nullptr);
    replacementText->setReadOnly(true);
    EXPECT_THROW(staleEditable.operation().apply(*_document), std::runtime_error);
    EXPECT_FALSE(staleEditable.operation().checkPostcondition(*_document).satisfied);
    replacementText->setReadOnly(false);

    auto staleOutput = prepare("stale-output", intent("Text", "string", "after"));
    replacementText->setStatus(Property::Output, true);
    EXPECT_THROW(staleOutput.operation().apply(*_document), std::runtime_error);
    EXPECT_FALSE(staleOutput.operation().checkPostcondition(*_document).satisfied);
}

TEST_F(CollaborativeSetPropertyOperationTest,
       freezesAndPublishesRecursiveDependentModelsForRecomputeSideEffects)
{
    auto* dependent = _document->addObject<FeatureTest>("Dependent");
    ASSERT_NE(dependent, nullptr);
    dependent->Source1.setValue(_target);
    _document->recompute();
    const long beforeExecutions = dependent->ExecCount.getValue();
    const auto targetModel = DocumentRevisionKey::objectModel("Target");
    const auto dependentModel = DocumentRevisionKey::objectModel("Dependent");
    const auto wildcard = DocumentRevisionKey::unknownModelMutation();
    const auto targetModelBefore = _document->collaborationRevisions().current(targetModel);
    const auto dependentModelBefore =
        _document->collaborationRevisions().current(dependentModel);
    const auto wildcardBefore = _document->collaborationRevisions().current(wildcard);
    const auto identity = _document->collaborationIdentity();
    const auto journalBefore = _document->collaborationRevisions().pollPublications(
        {identity.instanceId, identity.lifecycleEpoch, 0}, 0);
    const DocumentRevisionCursor cursor {
        identity.instanceId, identity.lifecycleEpoch, journalBefore.latestSequence};

    auto prepared = prepare("source-change", intent("Integer", "integer", "7"));
    EXPECT_EQ(prepared.writeSet(),
              (std::vector<DocumentRevisionKey> {dependentModel, targetModel}));
    ASSERT_EQ(prepared.publicationEffects().size(), 2U);
    EXPECT_EQ(prepared.publicationEffects()[0].stableObjectIdentity,
              std::optional<std::string>(
                  _document->collaborationObjectIdentity(*dependent)));
    EXPECT_EQ(prepared.publicationEffects()[1].stableObjectIdentity,
              std::optional<std::string>(_document->collaborationObjectIdentity(*_target)));
    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);

    ASSERT_TRUE(result.committed());
    EXPECT_GT(dependent->ExecCount.getValue(), beforeExecutions);
    EXPECT_EQ(_document->collaborationRevisions().current(targetModel), targetModelBefore + 1);
    EXPECT_EQ(_document->collaborationRevisions().current(dependentModel),
              dependentModelBefore + 1);
    EXPECT_EQ(_document->collaborationRevisions().current(wildcard), wildcardBefore);
    EXPECT_NE(std::find_if(result.publishedRevisions.begin(),
                           result.publishedRevisions.end(),
                           [&](const auto& published) {
                               return published.key == dependentModel;
                           }),
              result.publishedRevisions.end());

    const auto publications =
        _document->collaborationRevisions().pollPublications(cursor);
    ASSERT_EQ(publications.events.size(), 1U);
    const auto& changes = publications.events.front().changes;
    const auto dependentChange =
        std::find_if(changes.begin(), changes.end(), [&](const auto& change) {
            return change.key == dependentModel;
        });
    ASSERT_NE(dependentChange, changes.end());
    EXPECT_EQ(dependentChange->stableObjectIdentity,
              std::optional<std::string>(
                  _document->collaborationObjectIdentity(*dependent)));
}

TEST_F(CollaborativeSetPropertyOperationTest, independentObjectEditsDoNotConflict)
{
    auto* independent = _document->addObject<FeatureTest>("Independent");
    ASSERT_NE(independent, nullptr);
    _document->recompute();

    auto targetEdit = prepare("target", intent("Count", "integer", "4"));
    auto independentIntent = intent("Integer", "integer", "8");
    independentIntent.arguments["object"] = "Independent";
    auto independentEdit = prepare("independent", independentIntent);

    const auto targetResult =
        _document->collaborationService().commitEdit(_session.sessionId(), targetEdit);
    const auto independentResult =
        _document->collaborationService().commitEdit(_session.sessionId(), independentEdit);
    EXPECT_TRUE(targetResult.committed());
    EXPECT_TRUE(independentResult.committed());
    EXPECT_EQ(_count->getValue(), 4);
    EXPECT_EQ(independent->Integer.getValue(), 8);
}

TEST_F(CollaborativeSetPropertyOperationTest,
       noTouchLateReverseDependencyConflictsThroughDocumentStructure)
{
    auto* lateDependent = _document->addObject<FeatureTest>("LateDependent");
    ASSERT_NE(lateDependent, nullptr);
    lateDependent->setStatus(ObjectStatus::NoTouch, true);
    _document->recompute();

    auto prepared = prepare("late-dependent", intent("Count", "integer", "9"));
    ASSERT_NE(std::ranges::find(prepared.readSet(),
                               DocumentRevisionKey::documentStructure()),
              prepared.readSet().end());
    lateDependent->Source1.setValue(_target);
    EXPECT_FALSE(_document->mustExecute())
        << "NoTouch fixture must exercise the no-pending-recompute admission path";

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    EXPECT_EQ(result.status, DocumentCommitStatus::Conflict);
    EXPECT_NE(std::ranges::find_if(result.conflicts, [](const auto& conflict) {
                  return conflict.key == DocumentRevisionKey::documentStructure();
              }),
              result.conflicts.end());
    EXPECT_EQ(_count->getValue(), 3);
}

TEST_F(CollaborativeSetPropertyOperationTest, sameObjectPreparedEditsConflict)
{
    auto first = prepare("first", intent("Count", "integer", "4"));
    auto stale = prepare("second", intent("Text", "string", "stale"));

    EXPECT_TRUE(
        _document->collaborationService().commitEdit(_session.sessionId(), first).committed());
    const auto result = _document->collaborationService().commitEdit(_session.sessionId(), stale);
    EXPECT_EQ(result.status, DocumentCommitStatus::Conflict);
    EXPECT_EQ(_text->getStrValue(), "before");
}
