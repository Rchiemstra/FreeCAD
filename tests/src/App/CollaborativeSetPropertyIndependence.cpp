// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Base/Exception.h>

#include "App/Application.h"
#include "App/CollaborativeSetPropertyOperation.h"
#include "App/Document.h"
#include "App/DocumentCollaborationService.h"
#include "App/DocumentRevisionIndex.h"
#include "App/Expression.h"
#include "App/FeaturePython.h"
#include "App/FeatureTest.h"
#include "App/ObjectIdentifier.h"
#include "App/PropertyStandard.h"
#include <src/App/InitApplication.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace App;

namespace
{

class CollaborativeSetPropertyIndependenceTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        ensureCollaborativeSetPropertyOperationRegistered();
    }

    void SetUp() override
    {
        _documentName = App::GetApplication().getUniqueDocumentName("propertyIndependence");
        _document = App::GetApplication().newDocument(_documentName.c_str(),
                                                      "Property independence test");
        _target = _document->addObject<DocumentObject>("Target");
        ASSERT_NE(_target, nullptr);
        _first = addInteger(*_target, "First", Prop_NoRecompute);
        _second = addInteger(*_target, "Second", Prop_NoRecompute);
        _broad = addInteger(*_target, "Broad", Prop_None);
        ASSERT_NE(_first, nullptr);
        ASSERT_NE(_second, nullptr);
        ASSERT_NE(_broad, nullptr);
        _first->setValue(1);
        _second->setValue(2);
        _broad->setValue(3);
        _document->recompute();
        _session = _document->collaborationService().beginEditSession("actor-a");
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_documentName.c_str());
    }

    static PropertyInteger* addInteger(DocumentObject& object,
                                       const char* name,
                                       short propertyType)
    {
        return dynamic_cast<PropertyInteger*>(object.addDynamicProperty(
            "App::PropertyInteger", name, "Data", "", propertyType));
    }

    CollaborativeOperationIntent intent(std::string property,
                                        std::string value,
                                        std::string object = "Target") const
    {
        return {std::string(CollaborativeSetPropertyOperationType),
                {{"object", std::move(object)},
                 {"property", std::move(property)},
                 {"value_type", "integer"},
                 {"value", std::move(value)}}};
    }

    PreparedEdit prepare(std::string operationId,
                         const CollaborativeOperationIntent& operationIntent)
    {
        return _document->collaborationService().prepareEdit(_session.sessionId(),
                                                             std::move(operationId),
                                                             operationIntent,
                                                             "independence-test");
    }

    Document* _document {nullptr};
    DocumentObject* _target {nullptr};
    PropertyInteger* _first {nullptr};
    PropertyInteger* _second {nullptr};
    PropertyInteger* _broad {nullptr};
    EditSession _session {"placeholder", "placeholder", 1};
    std::string _documentName;
};

}  // namespace

TEST_F(CollaborativeSetPropertyIndependenceTest,
       independentPropertiesPreparedFromSameBaseCommitSequentially)
{
    auto first = prepare("first", intent("First", "10"));
    auto second = prepare("second", intent("Second", "20"));
    const auto firstKey = DocumentRevisionKey::objectProperty("Target", "First");
    const auto secondKey = DocumentRevisionKey::objectProperty("Target", "Second");

    EXPECT_EQ(first.readSet(),
              (std::vector<DocumentRevisionKey> {
                  DocumentRevisionKey::objectExistence("Target"),
                  DocumentRevisionKey::objectStructure("Target"),
                  DocumentRevisionKey::documentStructure(),
                  DocumentRevisionKey::unknownModelMutation(),
                  firstKey}));
    EXPECT_EQ(first.writeSet(), (std::vector<DocumentRevisionKey> {firstKey}));
    EXPECT_EQ(second.writeSet(), (std::vector<DocumentRevisionKey> {secondKey}));
    ASSERT_EQ(first.publicationEffects().size(), 1U);
    EXPECT_EQ(first.publicationEffects().front().key, firstKey);
    EXPECT_EQ(first.publicationEffects().front().stableObjectIdentity,
              std::optional<std::string>(
                  _document->collaborationObjectIdentity(*_target)));
    EXPECT_EQ(std::ranges::find(first.readSet(), DocumentRevisionKey::objectModel("Target")),
              first.readSet().end());
    EXPECT_EQ(std::ranges::find(second.readSet(), DocumentRevisionKey::objectModel("Target")),
              second.readSet().end());

    const auto firstResult =
        _document->collaborationService().commitEdit(_session.sessionId(), first);
    const auto secondResult =
        _document->collaborationService().commitEdit(_session.sessionId(), second);

    EXPECT_TRUE(firstResult.committed());
    EXPECT_TRUE(secondResult.committed());
    EXPECT_EQ(_first->getValue(), 10);
    EXPECT_EQ(_second->getValue(), 20);
}

TEST_F(CollaborativeSetPropertyIndependenceTest, concurrentSamePropertyStillConflicts)
{
    auto first = prepare("first", intent("First", "10"));
    auto stale = prepare("stale", intent("First", "11"));

    ASSERT_TRUE(
        _document->collaborationService().commitEdit(_session.sessionId(), first).committed());
    const auto staleResult =
        _document->collaborationService().commitEdit(_session.sessionId(), stale);

    EXPECT_EQ(staleResult.status, DocumentCommitStatus::Conflict);
    EXPECT_EQ(_first->getValue(), 10);
    EXPECT_NE(std::ranges::find_if(staleResult.conflicts, [](const auto& conflict) {
                  return conflict.key
                      == DocumentRevisionKey::objectProperty("Target", "First");
              }),
              staleResult.conflicts.end());
}

TEST_F(CollaborativeSetPropertyIndependenceTest,
       fineAndBroadEditsConflictInEitherCommitOrder)
{
    {
        auto broad = prepare("broad-first", intent("Broad", "30"));
        auto fine = prepare("fine-stale", intent("First", "10"));
        EXPECT_EQ(broad.writeSet(),
                  (std::vector<DocumentRevisionKey> {
                      DocumentRevisionKey::objectModel("Target"),
                      DocumentRevisionKey::unknownModelMutation()}));
        ASSERT_EQ(broad.publicationEffects().size(), 2U);
        EXPECT_EQ(broad.publicationEffects()[0].key,
                  DocumentRevisionKey::objectModel("Target"));
        EXPECT_EQ(broad.publicationEffects()[0].stableObjectIdentity,
                  std::optional<std::string>(
                      _document->collaborationObjectIdentity(*_target)));
        EXPECT_EQ(broad.publicationEffects()[1].key,
                  DocumentRevisionKey::unknownModelMutation());
        EXPECT_EQ(broad.publicationEffects()[1].stableObjectIdentity, std::nullopt);

        const auto broadResult =
            _document->collaborationService().commitEdit(_session.sessionId(), broad);
        const auto fineResult =
            _document->collaborationService().commitEdit(_session.sessionId(), fine);
        EXPECT_TRUE(broadResult.committed());
        EXPECT_EQ(fineResult.status, DocumentCommitStatus::Conflict);
        EXPECT_EQ(_broad->getValue(), 30);
        EXPECT_EQ(_first->getValue(), 1);
    }

    {
        auto fine = prepare("fine-first", intent("Second", "20"));
        auto broad = prepare("broad-stale", intent("Broad", "31"));

        const auto fineResult =
            _document->collaborationService().commitEdit(_session.sessionId(), fine);
        const auto broadResult =
            _document->collaborationService().commitEdit(_session.sessionId(), broad);
        EXPECT_TRUE(fineResult.committed());
        EXPECT_EQ(broadResult.status, DocumentCommitStatus::Conflict);
        EXPECT_EQ(_second->getValue(), 20);
        EXPECT_EQ(_broad->getValue(), 30);
    }
}

TEST_F(CollaborativeSetPropertyIndependenceTest,
       broadExactBaseEditStalesUnrelatedFineEditThroughWildcard)
{
    auto* other = _document->addObject<DocumentObject>("Other");
    ASSERT_NE(other, nullptr);
    auto* otherValue = addInteger(*other, "Value", Prop_NoRecompute);
    ASSERT_NE(otherValue, nullptr);
    otherValue->setValue(4);
    _document->recompute();

    auto broad = prepare("broad-target", intent("Broad", "30"));
    auto fine = prepare("fine-other", intent("Value", "40", "Other"));

    ASSERT_TRUE(
        _document->collaborationService().commitEdit(_session.sessionId(), broad).committed());
    const auto fineResult =
        _document->collaborationService().commitEdit(_session.sessionId(), fine);
    EXPECT_EQ(fineResult.status, DocumentCommitStatus::Conflict);
    EXPECT_EQ(otherValue->getValue(), 4);
    EXPECT_NE(std::ranges::find_if(fineResult.conflicts, [](const auto& conflict) {
                  return conflict.key == DocumentRevisionKey::unknownModelMutation();
              }),
              fineResult.conflicts.end());
}

TEST_F(CollaborativeSetPropertyIndependenceTest,
       initialExpressionUsesConservativeExactBaseFallback)
{
    _target->setExpression(
        ObjectIdentifier(*_second),
        std::shared_ptr<Expression>(Expression::parse(_target, "42")));
    _document->recompute();
    auto expressionFallback = prepare("expression", intent("First", "10"));
    EXPECT_EQ(expressionFallback.writeSet(),
              (std::vector<DocumentRevisionKey> {
                  DocumentRevisionKey::objectModel("Target"),
                  DocumentRevisionKey::unknownModelMutation()}));
}

TEST_F(CollaborativeSetPropertyIndependenceTest, expressionBoundTargetIsRejected)
{
    _target->setExpression(
        ObjectIdentifier(*_first),
        std::shared_ptr<Expression>(Expression::parse(_target, "42")));
    _document->recompute();
    EXPECT_THROW(static_cast<void>(prepare("bound-target", intent("First", "10"))),
                 std::invalid_argument);
}

TEST_F(CollaborativeSetPropertyIndependenceTest,
       independenceFactGateRejectsExtensionsAndEveryOtherMissingProof)
{
    Internal::CollaborativeSetPropertyIndependenceFacts facts {
        true, true, false, false, true, false};
    EXPECT_TRUE(Internal::hasCollaborativeSetPropertyIndependenceProof(facts));

    auto rejects = [&](auto member) {
        auto invalid = facts;
        invalid.*member = !(invalid.*member);
        EXPECT_FALSE(Internal::hasCollaborativeSetPropertyIndependenceProof(invalid));
    };
    rejects(&Internal::CollaborativeSetPropertyIndependenceFacts::exactBaseObject);
    rejects(&Internal::CollaborativeSetPropertyIndependenceFacts::exactDynamicProperty);
    rejects(&Internal::CollaborativeSetPropertyIndependenceFacts::hasExtensions);
    rejects(&Internal::CollaborativeSetPropertyIndependenceFacts::hasExpressions);
    rejects(&Internal::CollaborativeSetPropertyIndependenceFacts::noRecompute);
    rejects(&Internal::CollaborativeSetPropertyIndependenceFacts::hasReverseDependents);
}

TEST_F(CollaborativeSetPropertyIndependenceTest,
       initialReverseDependentUsesConservativeClosure)
{
    auto* dependent = _document->addObject<FeatureTest>("Dependent");
    ASSERT_NE(dependent, nullptr);
    dependent->Source1.setValue(_target);
    _document->recompute();
    auto dependentFallback = prepare("dependent", intent("First", "11"));
    EXPECT_EQ(dependentFallback.writeSet(),
              (std::vector<DocumentRevisionKey> {
                  DocumentRevisionKey::objectModel("Dependent"),
                  DocumentRevisionKey::objectModel("Target"),
                  DocumentRevisionKey::unknownModelMutation()}));
    EXPECT_EQ(std::ranges::find(dependentFallback.writeSet(),
                               DocumentRevisionKey::objectProperty("Target", "First")),
              dependentFallback.writeSet().end());
}

TEST_F(CollaborativeSetPropertyIndependenceTest,
       liveExpressionAndReverseDependencyInvalidateProofBeforeMutation)
{
    auto lateExpression = prepare("late-expression", intent("First", "10"));
    _target->setExpression(
        ObjectIdentifier(*_second),
        std::shared_ptr<Expression>(Expression::parse(_target, "42")));
    const long beforeExpressionApply = _first->getValue();
    EXPECT_THROW(lateExpression.operation().apply(*_document), std::runtime_error);
    EXPECT_EQ(_first->getValue(), beforeExpressionApply);
    _target->clearExpression(ObjectIdentifier(*_second));
    _document->recompute();

    auto lateDependent = prepare("late-dependent", intent("Second", "20"));
    auto* dependent = _document->addObject<FeatureTest>("LateDependent");
    ASSERT_NE(dependent, nullptr);
    dependent->Source1.setValue(_target);
    const long beforeDependentApply = _second->getValue();
    EXPECT_THROW(lateDependent.operation().apply(*_document), std::runtime_error);
    EXPECT_EQ(_second->getValue(), beforeDependentApply);
}

TEST_F(CollaborativeSetPropertyIndependenceTest, pythonBackedObjectIsExcluded)
{
    auto* python = dynamic_cast<FeaturePython*>(
        _document->addObject("App::FeaturePython", "PythonTarget"));
    ASSERT_NE(python, nullptr);
    auto* value = addInteger(*python, "Value", Prop_NoRecompute);
    ASSERT_NE(value, nullptr);
    value->setValue(5);
    _document->recompute();

    EXPECT_THROW(static_cast<void>(prepare("python", intent("Value", "6", "PythonTarget"))),
                 Base::RuntimeError);
    EXPECT_EQ(value->getValue(), 5);
}

TEST_F(CollaborativeSetPropertyIndependenceTest,
       nativeBoolFloatAndStringUseFinePropertyContract)
{
    auto* flag = dynamic_cast<PropertyBool*>(_target->addDynamicProperty(
        "App::PropertyBool", "Flag", "Data", "", Prop_NoRecompute));
    auto* ratio = dynamic_cast<PropertyFloat*>(_target->addDynamicProperty(
        "App::PropertyFloat", "Ratio", "Data", "", Prop_NoRecompute));
    auto* text = dynamic_cast<PropertyString*>(_target->addDynamicProperty(
        "App::PropertyString", "Text", "Data", "", Prop_NoRecompute));
    ASSERT_NE(flag, nullptr);
    ASSERT_NE(ratio, nullptr);
    ASSERT_NE(text, nullptr);
    flag->setValue(false);
    ratio->setValue(1.5);
    text->setValue("before");
    _document->recompute();

    auto typedIntent = [&](std::string property, std::string type, std::string value) {
        auto operationIntent = intent(std::move(property), std::move(value));
        operationIntent.arguments["value_type"] = std::move(type);
        return operationIntent;
    };
    auto boolean = prepare("bool", typedIntent("Flag", "bool", "true"));
    auto floating = prepare("float", typedIntent("Ratio", "float", "2.75"));
    auto string = prepare("string", typedIntent("Text", "string", "after"));

    EXPECT_EQ(boolean.writeSet(),
              (std::vector<DocumentRevisionKey> {
                  DocumentRevisionKey::objectProperty("Target", "Flag")}));
    EXPECT_EQ(floating.writeSet(),
              (std::vector<DocumentRevisionKey> {
                  DocumentRevisionKey::objectProperty("Target", "Ratio")}));
    EXPECT_EQ(string.writeSet(),
              (std::vector<DocumentRevisionKey> {
                  DocumentRevisionKey::objectProperty("Target", "Text")}));
    ASSERT_TRUE(
        _document->collaborationService().commitEdit(_session.sessionId(), boolean).committed());
    ASSERT_TRUE(
        _document->collaborationService().commitEdit(_session.sessionId(), floating).committed());
    ASSERT_TRUE(
        _document->collaborationService().commitEdit(_session.sessionId(), string).committed());
    EXPECT_TRUE(flag->getValue());
    EXPECT_DOUBLE_EQ(ratio->getValue(), 2.75);
    EXPECT_EQ(text->getStrValue(), "after");
}

TEST_F(CollaborativeSetPropertyIndependenceTest,
       propertyPublicationAlsoAdvancesAggregateObjectModel)
{
    const auto propertyKey = DocumentRevisionKey::objectProperty("Target", "First");
    const auto objectKey = DocumentRevisionKey::objectModel("Target");
    const auto propertyBefore = _document->collaborationRevisions().current(propertyKey);
    const auto objectBefore = _document->collaborationRevisions().current(objectKey);
    const auto identity = _document->collaborationIdentity();
    const auto journalBefore = _document->collaborationRevisions().pollPublications(
        {identity.instanceId, identity.lifecycleEpoch, 0}, 0);
    const DocumentRevisionCursor cursor {
        identity.instanceId, identity.lifecycleEpoch, journalBefore.latestSequence};
    auto prepared = prepare("aggregate", intent("First", "10"));

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);

    ASSERT_TRUE(result.committed());
    EXPECT_EQ(_document->collaborationRevisions().current(propertyKey), propertyBefore + 1);
    EXPECT_EQ(_document->collaborationRevisions().current(objectKey), objectBefore + 1);
    EXPECT_NE(std::ranges::find_if(result.publishedRevisions, [&](const auto& published) {
                  return published.key == propertyKey;
              }),
              result.publishedRevisions.end());
    EXPECT_NE(std::ranges::find_if(result.publishedRevisions, [&](const auto& published) {
                  return published.key == objectKey;
              }),
              result.publishedRevisions.end());

    const auto publications = _document->collaborationRevisions().pollPublications(cursor);
    ASSERT_EQ(publications.events.size(), 1U);
    const auto& changes = publications.events.front().changes;
    const auto propertyChange = std::ranges::find_if(changes, [&](const auto& change) {
        return change.key == propertyKey;
    });
    const auto objectChange = std::ranges::find_if(changes, [&](const auto& change) {
        return change.key == objectKey;
    });
    ASSERT_NE(propertyChange, changes.end());
    ASSERT_NE(objectChange, changes.end());
    const auto stableIdentity = std::optional<std::string>(
        _document->collaborationObjectIdentity(*_target));
    EXPECT_EQ(propertyChange->stableObjectIdentity, stableIdentity);
    EXPECT_EQ(objectChange->stableObjectIdentity, stableIdentity);
}

TEST_F(CollaborativeSetPropertyIndependenceTest, visibilityIsRejectedForEveryTargetClass)
{
    auto visibility = intent("Visibility", "true");
    visibility.arguments["value_type"] = "bool";
    EXPECT_THROW(static_cast<void>(prepare("base-visibility", visibility)),
                 std::invalid_argument);

    auto* featureTest = _document->addObject<FeatureTest>("FeatureTestTarget");
    ASSERT_NE(featureTest, nullptr);
    _document->recompute();
    visibility.arguments["object"] = "FeatureTestTarget";
    EXPECT_THROW(static_cast<void>(prepare("feature-visibility", visibility)),
                 std::invalid_argument);
}

TEST_F(CollaborativeSetPropertyIndependenceTest,
       featureTestRemainsConservativeObjectLevel)
{
    auto* featureTest = _document->addObject<FeatureTest>("FeatureTestTarget");
    ASSERT_NE(featureTest, nullptr);
    _document->recompute();

    auto prepared = prepare("feature-test", intent("Integer", "9", "FeatureTestTarget"));

    EXPECT_EQ(prepared.writeSet(),
              (std::vector<DocumentRevisionKey> {
                  DocumentRevisionKey::objectModel("FeatureTestTarget")}));
    ASSERT_EQ(prepared.publicationEffects().size(), 1U);
    EXPECT_EQ(prepared.publicationEffects().front().key,
              DocumentRevisionKey::objectModel("FeatureTestTarget"));
    ASSERT_TRUE(
        _document->collaborationService().commitEdit(_session.sessionId(), prepared).committed());
    EXPECT_EQ(featureTest->Integer.getValue(), 9);
}

TEST_F(CollaborativeSetPropertyIndependenceTest,
       liveProofRejectsStaleIdentitySchemaTypeAndEditabilityWithoutMutation)
{
    auto staleIdentity = prepare("identity", intent("First", "10"));
    _document->removeObject("Target");
    auto* replacement = _document->addObject<DocumentObject>("Target");
    ASSERT_NE(replacement, nullptr);
    auto* replacementFirst = addInteger(*replacement, "First", Prop_NoRecompute);
    ASSERT_NE(replacementFirst, nullptr);
    replacementFirst->setValue(101);
    EXPECT_THROW(staleIdentity.operation().apply(*_document), std::runtime_error);
    EXPECT_EQ(replacementFirst->getValue(), 101);

    _document->recompute();
    auto staleSchema = prepare("schema", intent("First", "11"));
    ASSERT_TRUE(replacement->removeDynamicProperty("First"));
    EXPECT_THROW(staleSchema.operation().apply(*_document), std::runtime_error);

    replacementFirst = addInteger(*replacement, "First", Prop_NoRecompute);
    ASSERT_NE(replacementFirst, nullptr);
    replacementFirst->setValue(102);
    _document->recompute();
    auto staleType = prepare("type", intent("First", "12"));
    ASSERT_TRUE(replacement->removeDynamicProperty("First"));
    auto* changedType = dynamic_cast<PropertyString*>(replacement->addDynamicProperty(
        "App::PropertyString", "First", "Data", "", Prop_NoRecompute));
    ASSERT_NE(changedType, nullptr);
    changedType->setValue("unchanged");
    EXPECT_THROW(staleType.operation().apply(*_document), std::runtime_error);
    EXPECT_EQ(changedType->getStrValue(), "unchanged");

    ASSERT_TRUE(replacement->removeDynamicProperty("First"));
    replacementFirst = addInteger(*replacement, "First", Prop_NoRecompute);
    ASSERT_NE(replacementFirst, nullptr);
    replacementFirst->setValue(103);
    _document->recompute();
    auto staleEditable = prepare("editable", intent("First", "13"));
    replacementFirst->setReadOnly(true);
    EXPECT_THROW(staleEditable.operation().apply(*_document), std::runtime_error);
    EXPECT_EQ(replacementFirst->getValue(), 103);
}
