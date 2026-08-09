// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/DocumentRevisionIndex.h"
#include <3rdParty/json/single_include/nlohmann/json.hpp>

#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace App;

namespace
{

constexpr DocumentInstanceId TestDocumentInstanceId = 61;
constexpr DocumentLifecycleEpoch TestLifecycleEpoch = 6;

void bindTestIdentity(DocumentRevisionIndex& index)
{
    index.bindDocumentIdentity(TestDocumentInstanceId, TestLifecycleEpoch);
}

DocumentRevisionPublicationRequest objectChange(const DocumentRevisionKey& key,
                                                std::string stableObjectIdentity = "object-17")
{
    return {key, std::move(stableObjectIdentity)};
}

DocumentRevisionCursor testCursor(DocumentPublicationSequence afterSequence = 0)
{
    return {TestDocumentInstanceId, TestLifecycleEpoch, afterSequence};
}

}  // namespace

TEST(DocumentPropertyRevisionKeyTest, KeepsObjectAndPropertyAsSeparateValueScalars)
{
    const auto first = DocumentRevisionKey::objectProperty("ab", "c");
    const auto second = DocumentRevisionKey::objectProperty("a", "bc");
    const auto sameFirst = DocumentRevisionKey::objectProperty("ab", "c");
    const auto otherProperty = DocumentRevisionKey::objectProperty("ab", "d");

    static_assert(std::is_same_v<decltype(DocumentRevisionKey::subject), std::string>);
    static_assert(std::is_same_v<decltype(DocumentRevisionKey::propertyName), std::string>);
    EXPECT_EQ(first.subject, "ab");
    EXPECT_EQ(first.propertyName, "c");
    EXPECT_EQ(first, sameFirst);
    EXPECT_NE(first, second);
    EXPECT_NE(first, otherProperty);
    EXPECT_TRUE(first < otherProperty);

    std::unordered_set<DocumentRevisionKey, DocumentRevisionKeyHash> keys;
    keys.insert(first);
    keys.insert(second);
    keys.insert(sameFirst);
    keys.insert(otherProperty);
    EXPECT_EQ(keys.size(), 3U);
}

TEST(DocumentPropertyRevisionKeyTest, ValidatesPropertyAndLegacyScopes)
{
    EXPECT_TRUE(DocumentRevisionKey::objectProperty("Cube", "Length").valid());
    EXPECT_FALSE(DocumentRevisionKey::objectProperty({}, "Length").valid());
    EXPECT_FALSE(DocumentRevisionKey::objectProperty("Cube", {}).valid());
    EXPECT_FALSE(
        (DocumentRevisionKey {DocumentRevisionKind::ObjectModel, "Cube", "Length"}).valid());
    EXPECT_FALSE(
        (DocumentRevisionKey {DocumentRevisionKind::DocumentStructure, {}, "Length"}).valid());

    // Existing two-field aggregate initializers retain their original meaning.
    const DocumentRevisionKey legacyObject {DocumentRevisionKind::ObjectModel, "Cube"};
    const DocumentRevisionKey legacyDocument {DocumentRevisionKind::DocumentStructure, {}};
    EXPECT_TRUE(legacyObject.valid());
    EXPECT_TRUE(legacyObject.propertyName.empty());
    EXPECT_TRUE(legacyDocument.valid());
    EXPECT_TRUE(DocumentRevisionKey::objectExistence("Cube").valid());
    EXPECT_TRUE(DocumentRevisionKey::objectStructure("Cube").valid());
    EXPECT_TRUE(DocumentRevisionKey::unknownModelMutation().valid());
}

TEST(DocumentPropertyRevisionIndexTest, PropertyPublicationAdvancesPropertyAndAggregateAtomically)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto property = DocumentRevisionKey::objectProperty("Cube", "Length");
    const auto aggregate = DocumentRevisionKey::objectModel("Cube");
    const auto before = index.capture({property, aggregate});

    const auto published = index.publish({objectChange(property)});

    ASSERT_EQ(published.size(), 2U);
    EXPECT_EQ(published[0], DocumentRevisionObservation(property, 1));
    EXPECT_EQ(published[1], DocumentRevisionObservation(aggregate, 1));
    EXPECT_EQ(index.current(property), 1U);
    EXPECT_EQ(index.current(aggregate), 1U);

    const auto conflicts = index.validate(before);
    ASSERT_EQ(conflicts.size(), 2U);
    EXPECT_EQ(conflicts[0], DocumentRevisionConflict(property, 0, 1));
    EXPECT_EQ(conflicts[1], DocumentRevisionConflict(aggregate, 0, 1));

    const auto poll = index.pollPublications(testCursor());
    ASSERT_EQ(poll.events.size(), 1U);
    ASSERT_EQ(poll.events.front().changes.size(), 2U);
    for (const auto& change : poll.events.front().changes) {
        EXPECT_EQ(change.stableObjectIdentity,
                  std::optional<std::string> {"object-17"});
    }
}

TEST(DocumentPropertyRevisionIndexTest, IndependentPropertiesRemainFineGrained)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto length = DocumentRevisionKey::objectProperty("Cube", "Length");
    const auto width = DocumentRevisionKey::objectProperty("Cube", "Width");
    const auto aggregate = DocumentRevisionKey::objectModel("Cube");
    const auto widthOnly = index.capture({width});
    const auto broad = index.capture({aggregate});

    static_cast<void>(index.publish({objectChange(length)}));

    EXPECT_TRUE(index.validate(widthOnly).empty());
    ASSERT_EQ(index.validate(broad).size(), 1U);
    EXPECT_EQ(index.current(length), 1U);
    EXPECT_EQ(index.current(width), 0U);
    EXPECT_EQ(index.current(aggregate), 1U);

    static_cast<void>(index.publish({objectChange(width)}));
    EXPECT_EQ(index.current(length), 1U);
    EXPECT_EQ(index.current(width), 1U);
    EXPECT_EQ(index.current(aggregate), 2U);
}

TEST(DocumentPropertyRevisionIndexTest, ExpandedDuplicatesCanonicalizeOnce)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto length = DocumentRevisionKey::objectProperty("Cube", "Length");
    const auto width = DocumentRevisionKey::objectProperty("Cube", "Width");
    const auto aggregate = DocumentRevisionKey::objectModel("Cube");

    const auto published = index.publish(
        {objectChange(length),
         objectChange(length),
         objectChange(aggregate),
         objectChange(width)});

    ASSERT_EQ(published.size(), 3U);
    EXPECT_EQ(published[0], DocumentRevisionObservation(length, 1));
    EXPECT_EQ(published[1], DocumentRevisionObservation(aggregate, 1));
    EXPECT_EQ(published[2], DocumentRevisionObservation(width, 1));
    EXPECT_EQ(index.current(length), 1U);
    EXPECT_EQ(index.current(width), 1U);
    EXPECT_EQ(index.current(aggregate), 1U);
    const auto poll = index.pollPublications(testCursor());
    ASSERT_EQ(poll.events.size(), 1U);
    EXPECT_EQ(poll.events.front().changes.size(), 3U);
}

TEST(DocumentPropertyRevisionIndexTest, ExpandedIdentityConflictRejectsWholePublication)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto length = DocumentRevisionKey::objectProperty("Cube", "Length");
    const auto width = DocumentRevisionKey::objectProperty("Cube", "Width");
    const auto aggregate = DocumentRevisionKey::objectModel("Cube");

    EXPECT_THROW(
        static_cast<void>(index.publish(
            {objectChange(length, "object-17"), objectChange(width, "object-29")})),
        std::invalid_argument);
    EXPECT_EQ(index.current(length), 0U);
    EXPECT_EQ(index.current(width), 0U);
    EXPECT_EQ(index.current(aggregate), 0U);
    EXPECT_EQ(index.pollPublications(testCursor()).latestSequence, 0U);

    EXPECT_THROW(
        static_cast<void>(index.publish(
            {objectChange(length, "object-17"), objectChange(aggregate, "object-29")})),
        std::invalid_argument);
    EXPECT_EQ(index.pollPublications(testCursor()).latestSequence, 0U);
}

TEST(DocumentPropertyRevisionIndexTest, JsonCarriesSeparateUnicodePropertyAndStableIdentity)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const std::string subject = "C\xc3\xba" "be";
    const std::string propertyName = "L\xc3\xa4nge\"\n";
    const std::string identity = "identity-\xf0\x9f\x94\xa9";
    const auto property = DocumentRevisionKey::objectProperty(subject, propertyName);

    static_cast<void>(index.publish({objectChange(property, identity)}));
    const auto poll = index.pollPublications(testCursor());
    ASSERT_EQ(poll.events.size(), 1U);
    const auto parsed = nlohmann::json::parse(poll.events.front().toJson());
    ASSERT_EQ(parsed.at("changes").size(), 2U);
    const auto& propertyJson = parsed.at("changes").at(0);
    EXPECT_EQ(propertyJson.at("kind").get<std::string>(), "ObjectProperty");
    EXPECT_EQ(propertyJson.at("subject").get<std::string>(), subject);
    EXPECT_EQ(propertyJson.at("property_name").get<std::string>(), propertyName);
    EXPECT_EQ(propertyJson.at("stable_object_identity").get<std::string>(), identity);
    const auto& aggregateJson = parsed.at("changes").at(1);
    EXPECT_EQ(aggregateJson.at("kind").get<std::string>(), "ObjectModel");
    EXPECT_FALSE(aggregateJson.contains("property_name"));
    EXPECT_EQ(aggregateJson.at("stable_object_identity").get<std::string>(), identity);

    DocumentRevisionPublicationEvent invalidEvent;
    invalidEvent.changes.push_back(
        {{DocumentRevisionKind::ObjectProperty, "Cube", {}}, 1, "object-17"});
    EXPECT_THROW(static_cast<void>(invalidEvent.toJson()), std::invalid_argument);
}

TEST(DocumentPropertyRevisionIndexTest, InvalidPropertyPublicationIsAtomic)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto valid = DocumentRevisionKey::objectProperty("Cube", "Length");
    const auto missingName = DocumentRevisionKey::objectProperty("Cube", {});
    const std::string invalidUtf8 {"\xc3\x28", 2};
    const auto invalidEncoding = DocumentRevisionKey::objectProperty("Cube", invalidUtf8);

    EXPECT_FALSE(invalidEncoding.valid());
    EXPECT_THROW(static_cast<void>(index.publish({objectChange(valid, {})})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(index.publish({objectChange(missingName)})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(index.publish({objectChange(invalidEncoding)})),
                 std::invalid_argument);
    EXPECT_EQ(index.current(valid), 0U);
    EXPECT_EQ(index.current(DocumentRevisionKey::objectModel("Cube")), 0U);
    EXPECT_EQ(index.pollPublications(testCursor()).latestSequence, 0U);
}

TEST(DocumentPropertyRevisionIndexTest, AggregateOverflowRejectsNewPropertyAtomically)
{
    DocumentRevisionIndex index(1);
    bindTestIdentity(index);
    const auto length = DocumentRevisionKey::objectProperty("Cube", "Length");
    const auto width = DocumentRevisionKey::objectProperty("Cube", "Width");
    const auto aggregate = DocumentRevisionKey::objectModel("Cube");
    static_cast<void>(index.publish({objectChange(length)}));
    const auto before = index.pollPublications(testCursor()).toJson();

    EXPECT_THROW(static_cast<void>(index.publish({objectChange(width)})),
                 std::overflow_error);
    EXPECT_EQ(index.current(length), 1U);
    EXPECT_EQ(index.current(width), 0U);
    EXPECT_EQ(index.current(aggregate), 1U);
    EXPECT_EQ(index.pollPublications(testCursor()).toJson(), before);
}

TEST(DocumentPropertyRevisionIndexTest, SequenceOverflowRejectsExpandedPublicationAtomically)
{
    DocumentRevisionIndex index(
        std::numeric_limits<DocumentRevision>::max(), 1, 4);
    bindTestIdentity(index);
    const auto first = DocumentRevisionKey::objectProperty("Cube", "Length");
    const auto second = DocumentRevisionKey::objectProperty("Sphere", "Radius");
    static_cast<void>(index.publish({objectChange(first)}));
    const auto before = index.pollPublications(testCursor()).toJson();

    EXPECT_THROW(static_cast<void>(index.publish({objectChange(second, "object-29")})),
                 std::overflow_error);
    EXPECT_EQ(index.current(second), 0U);
    EXPECT_EQ(index.current(DocumentRevisionKey::objectModel("Sphere")), 0U);
    EXPECT_EQ(index.pollPublications(testCursor()).toJson(), before);
}

TEST(DocumentPropertyRevisionReservationTest, CancelAndCommitCoverExpandedBoundary)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto property = DocumentRevisionKey::objectProperty("Cube", "Length");
    const auto aggregate = DocumentRevisionKey::objectModel("Cube");

    {
        auto reservation = index.reservePublication(
            {DocumentRevisionObservation(property, 0)}, {objectChange(property)});
        ASSERT_TRUE(reservation.ready());
        reservation.cancel();
    }
    EXPECT_EQ(index.current(property), 0U);
    EXPECT_EQ(index.current(aggregate), 0U);
    EXPECT_EQ(index.pollPublications(testCursor()).latestSequence, 0U);

    auto reservation = index.reservePublication(
        {DocumentRevisionObservation(property, 0)}, {objectChange(property)});
    ASSERT_TRUE(reservation.ready());
    const auto committed = reservation.commit();
    ASSERT_EQ(committed.size(), 2U);
    EXPECT_EQ(committed[0], DocumentRevisionObservation(property, 1));
    EXPECT_EQ(committed[1], DocumentRevisionObservation(aggregate, 1));
    EXPECT_EQ(index.current(property), 1U);
    EXPECT_EQ(index.current(aggregate), 1U);
    EXPECT_EQ(index.pollPublications(testCursor()).latestSequence, 1U);
}

TEST(DocumentPropertyRevisionReservationTest, FineExpectationNeedNotCaptureAggregate)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto length = DocumentRevisionKey::objectProperty("Cube", "Length");
    const auto width = DocumentRevisionKey::objectProperty("Cube", "Width");
    static_cast<void>(index.publish({objectChange(length)}));

    auto reservation = index.reservePublication(
        {DocumentRevisionObservation(width, 0)}, {objectChange(width)});
    ASSERT_TRUE(reservation.ready());
    EXPECT_TRUE(reservation.conflicts().empty());
    static_cast<void>(reservation.commit());
    EXPECT_EQ(index.current(length), 1U);
    EXPECT_EQ(index.current(width), 1U);
    EXPECT_EQ(index.current(DocumentRevisionKey::objectModel("Cube")), 2U);
}

TEST(DocumentPropertyRevisionIndexTest, EmptyPublicationHasExplicitNamedPath)
{
    DocumentRevisionIndex unbound;
    EXPECT_THROW(static_cast<void>(unbound.publishEmpty()), std::logic_error);

    DocumentRevisionIndex index;
    bindTestIdentity(index);
    EXPECT_TRUE(index.publishEmpty().empty());
    EXPECT_EQ(index.pollPublications(testCursor()).latestSequence, 0U);
    EXPECT_TRUE(index.pollPublications(testCursor()).events.empty());

    // Both existing publication overloads remain callable through explicit types.
    EXPECT_TRUE(index.publish(std::vector<DocumentRevisionKey> {}).empty());
    EXPECT_TRUE(
        index.publish(std::vector<DocumentRevisionPublicationRequest> {}).empty());
}

TEST(DocumentPropertyRevisionIndexTest, PropertyRevisionsAreStrictlyMonotonic)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto property = DocumentRevisionKey::objectProperty("Cube", "Length");
    const auto aggregate = DocumentRevisionKey::objectModel("Cube");
    const auto captured = index.capture({property});

    for (DocumentRevision expected = 1; expected <= 3; ++expected) {
        const auto published = index.publish({objectChange(property)});
        ASSERT_EQ(published.size(), 2U);
        EXPECT_EQ(published[0], DocumentRevisionObservation(property, expected));
        EXPECT_EQ(published[1], DocumentRevisionObservation(aggregate, expected));
    }
    EXPECT_EQ(index.current(property), 3U);
    EXPECT_EQ(index.current(aggregate), 3U);
    ASSERT_EQ(index.validate(captured).size(), 1U);
}
