// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/Application.h"
#include "App/CollaborativeOperation.h"
#include "App/Document.h"
#include "App/PreparedEdit.h"
#include <src/App/InitApplication.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace App;

namespace
{

constexpr DocumentInstanceId TestDocumentInstanceId = 41;
constexpr DocumentLifecycleEpoch TestLifecycleEpoch = 7;

const DocumentRevisionKey ReadKey = DocumentRevisionKey::objectExistence("Input");
const DocumentRevisionKey SharedKey = DocumentRevisionKey::objectModel("Result");
const DocumentRevisionKey WriteKey = DocumentRevisionKey::documentStructure();

struct FakeOperationState
{
    bool applied {false};
    bool applyDocumentSeen {false};
    bool postconditionDocumentSeen {false};
};

class FakeOperation final: public CollaborativeOperation
{
public:
    explicit FakeOperation(std::shared_ptr<FakeOperationState> state,
                           std::string type = "test.fake")
        : _state(std::move(state))
        , _type(std::move(type))
    {}

    std::string_view typeId() const noexcept override
    {
        return _type;
    }

    void apply(Document&) const override
    {
        _state->applyDocumentSeen = true;
        _state->applied = true;
    }

    CollaborativePostconditionResult checkPostcondition(const Document&) const override
    {
        _state->postconditionDocumentSeen = true;
        return {_state->applied, _state->applied ? "applied" : "not applied"};
    }

private:
    std::shared_ptr<FakeOperationState> _state;
    std::string _type;
};

std::vector<DocumentRevisionObservation> expectedRevisions()
{
    return {{WriteKey, 8}, {SharedKey, 5}, {ReadKey, 3}};
}

std::vector<DocumentRevisionKey> readSet()
{
    return {SharedKey, ReadKey};
}

std::vector<DocumentRevisionKey> writeSet()
{
    return {WriteKey, SharedKey};
}

std::vector<DocumentRevisionPublicationRequest> publicationEffects()
{
    return {{WriteKey, std::nullopt}, {SharedKey, std::string("result-17")}};
}

PreparedEditCanonicalContract validateContract(
    const CollaborativeOperation& operation,
    std::vector<DocumentRevisionObservation> expected = expectedRevisions(),
    std::vector<DocumentRevisionKey> reads = readSet(),
    std::vector<DocumentRevisionKey> writes = writeSet(),
    std::vector<DocumentRevisionPublicationRequest> effects = publicationEffects(),
    std::string_view operationId = "operation-17",
    DocumentInstanceId instanceId = TestDocumentInstanceId,
    DocumentLifecycleEpoch epoch = TestLifecycleEpoch,
    std::string_view operationType = "test.fake",
    std::string_view provenance = "native-test-preparation")
{
    return validatePreparedEditContract(operationId,
                                        instanceId,
                                        epoch,
                                        operationType,
                                        std::move(expected),
                                        std::move(reads),
                                        std::move(writes),
                                        std::move(effects),
                                        provenance,
                                        operation);
}

class PreparedEditOperationTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _documentName = App::GetApplication().getUniqueDocumentName("preparedEdit");
        _document = App::GetApplication().newDocument(_documentName.c_str(), "preparedEditTest");
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_documentName.c_str());
    }

    Document& document()
    {
        return *_document;
    }

private:
    std::string _documentName;
    Document* _document {nullptr};
};

}  // namespace

static_assert(!std::is_default_constructible_v<PreparedEdit>);
static_assert(!std::is_copy_constructible_v<PreparedEdit>);
static_assert(std::is_move_constructible_v<PreparedEdit>);
static_assert(!std::is_copy_assignable_v<PreparedEdit>);
static_assert(!std::is_move_assignable_v<PreparedEdit>);
static_assert(!std::is_default_constructible_v<PreparedEdit::ConstructionKey>);
static_assert(!std::is_copy_constructible_v<PreparedEdit::ConstructionKey>);
static_assert(!std::is_move_constructible_v<PreparedEdit::ConstructionKey>);
static_assert(std::is_same_v<decltype(std::declval<const PreparedEdit&>().operation()),
                             const CollaborativeOperation&>);
static_assert(!std::is_constructible_v<PreparedEdit, Document*>);
static_assert(!std::is_constructible_v<PreparedEdit, DocumentObject*>);

TEST(PreparedEditContractTest, canonicalizesEverySemanticSequence)
{
    const auto state = std::make_shared<FakeOperationState>();
    const FakeOperation operation(state);
    const auto canonical = validateContract(operation);

    ASSERT_EQ(canonical.expectedRevisions.size(), 3U);
    EXPECT_EQ(canonical.expectedRevisions[0], DocumentRevisionObservation(ReadKey, 3));
    EXPECT_EQ(canonical.expectedRevisions[1], DocumentRevisionObservation(SharedKey, 5));
    EXPECT_EQ(canonical.expectedRevisions[2], DocumentRevisionObservation(WriteKey, 8));
    EXPECT_EQ(canonical.readSet, (std::vector<DocumentRevisionKey> {ReadKey, SharedKey}));
    EXPECT_EQ(canonical.writeSet, (std::vector<DocumentRevisionKey> {SharedKey, WriteKey}));
    ASSERT_EQ(canonical.publicationEffects.size(), 2U);
    EXPECT_EQ(canonical.publicationEffects[0].key, SharedKey);
    EXPECT_EQ(canonical.publicationEffects[0].stableObjectIdentity,
              std::optional<std::string>("result-17"));
    EXPECT_EQ(canonical.publicationEffects[1].key, WriteKey);
    EXPECT_EQ(canonical.publicationEffects[1].stableObjectIdentity, std::nullopt);
}

TEST(PreparedEditContractTest, rejectsEmptyIdentityTypeProvenanceAndTypeMismatch)
{
    const auto state = std::make_shared<FakeOperationState>();
    const FakeOperation operation(state);
    const FakeOperation otherType(state, "another.type");
    const FakeOperation emptyType(state, "");

    EXPECT_THROW(static_cast<void>(validateContract(operation,
                                                    expectedRevisions(),
                                                    readSet(),
                                                    writeSet(),
                                                    publicationEffects(),
                                                    "")),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(operation,
                                                    expectedRevisions(),
                                                    readSet(),
                                                    writeSet(),
                                                    publicationEffects(),
                                                    "operation-17",
                                                    0)),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(operation,
                                                    expectedRevisions(),
                                                    readSet(),
                                                    writeSet(),
                                                    publicationEffects(),
                                                    "operation-17",
                                                    TestDocumentInstanceId,
                                                    0)),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(operation,
                                                    expectedRevisions(),
                                                    readSet(),
                                                    writeSet(),
                                                    publicationEffects(),
                                                    "operation-17",
                                                    TestDocumentInstanceId,
                                                    TestLifecycleEpoch,
                                                    "")),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(operation,
                                                    expectedRevisions(),
                                                    readSet(),
                                                    writeSet(),
                                                    publicationEffects(),
                                                    "operation-17",
                                                    TestDocumentInstanceId,
                                                    TestLifecycleEpoch,
                                                    "test.fake",
                                                    "")),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(otherType)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(emptyType)), std::invalid_argument);
}

TEST(PreparedEditContractTest, rejectsInvalidOrDuplicateDependencyKeys)
{
    const auto state = std::make_shared<FakeOperationState>();
    const FakeOperation operation(state);
    const DocumentRevisionKey invalidKey {DocumentRevisionKind::DocumentStructure,
                                          "must-be-empty"};

    EXPECT_THROW(static_cast<void>(validateContract(operation,
                                                    {{invalidKey, 0}},
                                                    {invalidKey},
                                                    {},
                                                    {})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(operation,
                                                    expectedRevisions(),
                                                    {ReadKey, ReadKey, SharedKey})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(operation,
                                                    expectedRevisions(),
                                                    readSet(),
                                                    {WriteKey, SharedKey, WriteKey})),
                 std::invalid_argument);
}

TEST(PreparedEditContractTest, expectedRevisionsMustExactlyCoverDependencyUnion)
{
    const auto state = std::make_shared<FakeOperationState>();
    const FakeOperation operation(state);

    EXPECT_THROW(static_cast<void>(validateContract(
                     operation,
                     {{ReadKey, 3}, {SharedKey, 5}})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(
                     operation,
                     {{ReadKey, 3},
                      {SharedKey, 5},
                      {WriteKey, 8},
                      {DocumentRevisionKey::unknownModelMutation(), 1}})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(
                     operation,
                     {{ReadKey, 3}, {ReadKey, 4}, {SharedKey, 5}, {WriteKey, 8}})),
                 std::invalid_argument);
}

TEST(PreparedEditContractTest, publicationEffectsMustExactlyCoverWriteSet)
{
    const auto state = std::make_shared<FakeOperationState>();
    const FakeOperation operation(state);

    EXPECT_THROW(static_cast<void>(validateContract(
                     operation,
                     expectedRevisions(),
                     readSet(),
                     writeSet(),
                     {{SharedKey, std::string("result-17")}})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(
                     operation,
                     expectedRevisions(),
                     readSet(),
                     writeSet(),
                     {{SharedKey, std::string("result-17")},
                      {WriteKey, std::nullopt},
                      {ReadKey, std::string("input-3")}})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(
                     operation,
                     expectedRevisions(),
                     readSet(),
                     writeSet(),
                     {{SharedKey, std::string("result-17")},
                      {SharedKey, std::string("result-17")},
                      {WriteKey, std::nullopt}})),
                 std::invalid_argument);
}

TEST(PreparedEditContractTest, publicationEffectsRequireCorrectIdentityScope)
{
    const auto state = std::make_shared<FakeOperationState>();
    const FakeOperation operation(state);

    EXPECT_THROW(static_cast<void>(validateContract(
                     operation,
                     expectedRevisions(),
                     readSet(),
                     writeSet(),
                     {{SharedKey, std::nullopt}, {WriteKey, std::nullopt}})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(validateContract(
                     operation,
                     expectedRevisions(),
                     readSet(),
                     writeSet(),
                     {{SharedKey, std::string("result-17")},
                      {WriteKey, std::string("document-has-no-object-identity")}})),
                 std::invalid_argument);
}

TEST_F(PreparedEditOperationTest, fakeFinalBatchUsesOnlyShortLivedDocumentArguments)
{
    auto state = std::make_shared<FakeOperationState>();
    const FakeOperation operation(state);

    EXPECT_EQ(operation.typeId(), "test.fake");
    const auto beforeApply = operation.checkPostcondition(document());
    EXPECT_FALSE(beforeApply.satisfied);
    EXPECT_EQ(beforeApply.message, "not applied");

    operation.apply(document());
    const auto afterApply = operation.checkPostcondition(document());
    EXPECT_TRUE(afterApply.satisfied);
    EXPECT_EQ(afterApply.message, "applied");
    EXPECT_TRUE(state->applyDocumentSeen);
    EXPECT_TRUE(state->postconditionDocumentSeen);
    EXPECT_TRUE(state->applied);
}
