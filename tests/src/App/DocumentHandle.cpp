// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/DocumentCommand.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentHandle.h>
#include <Gui/PresentationDelta.h>

#include <type_traits>

using namespace App;

static_assert(std::is_integral_v<DocumentCommandId>);
static_assert(!std::is_pointer_v<DocumentCommandId>);
static_assert(!std::is_constructible_v<DocumentHandle, Document*>);
static_assert(!std::is_constructible_v<DocumentCommandHandle, Document*>);
static_assert(!std::is_constructible_v<DocumentCommandHandle, DocumentObject*>);
static_assert(std::is_copy_constructible_v<DocumentHandle>);
static_assert(std::is_same_v<DocumentCommandSnapshot,
                              decltype(std::declval<const DocumentCommandHandle&>().status())>);
static_assert(std::is_copy_constructible_v<DocumentCommandRecomputeObservation>);
static_assert(!std::is_pointer_v<DocumentCommandRecomputeObservation>);
static_assert(!std::is_pointer_v<Gui::PresentationRenderBuffer>);
static_assert(std::is_same_v<decltype(std::declval<Gui::PresentationRenderBuffer>().topology),
                              std::vector<std::uint32_t>>);
static_assert(
    std::is_same_v<decltype(std::declval<Gui::PresentationRenderBuffer>().subelementMappings),
                   std::vector<Gui::PresentationRenderSubelementMapping>>);
static_assert(
    std::is_same_v<decltype(std::declval<Gui::PresentationRenderBuffer>().material),
                   Gui::PresentationRenderMaterial>);

TEST(DocumentCommandContractTest, exposesStableSubmitResultNames)
{
    EXPECT_STREQ(documentCommandSubmitResultName(DocumentCommandSubmitResult::Accepted),
                 "Accepted");
    EXPECT_STREQ(documentCommandSubmitResultName(DocumentCommandSubmitResult::Busy), "Busy");
    EXPECT_STREQ(documentCommandSubmitResultName(DocumentCommandSubmitResult::Closed), "Closed");
    EXPECT_STREQ(documentCommandSubmitResultName(DocumentCommandSubmitResult::Conflict),
                 "Conflict");
    EXPECT_STREQ(documentCommandSubmitResultName(DocumentCommandSubmitResult::Unsupported),
                 "Unsupported");
}

TEST(DocumentHandleContractTest, invalidHandleReportsClosedOnSubmit)
{
    DocumentHandle handle;
    DocumentCommand command;
    command.kind = DocumentCommandKind::Recompute;

    const auto outcome = handle.trySubmit(std::move(command));
    EXPECT_EQ(outcome.result, DocumentCommandSubmitResult::Closed);
    EXPECT_FALSE(outcome.accepted());
}

TEST(DocumentHandleContractTest, identityMismatchReturnsConflict)
{
    DocumentRevisionIdentityBinding identity {42, 7};
    DocumentHandle handle(identity);

    DocumentCommand command;
    command.kind = DocumentCommandKind::Save;
    command.document = {99, 7};

    const auto outcome = handle.trySubmit(std::move(command));
    EXPECT_EQ(outcome.result, DocumentCommandSubmitResult::Conflict);
}

TEST(DocumentHandleContractTest, matchingIdentityReturnsUnsupportedWithoutLane)
{
    DocumentRevisionIdentityBinding identity {42, 7};
    DocumentHandle handle(identity);

    DocumentCommand command;
    command.kind = DocumentCommandKind::Recompute;
    command.document = identity;
    command.recompute = DocumentCommandRecomputePayload {};
    command.recompute->coalescingKey = "all";

    const auto outcome = handle.trySubmit(std::move(command));
    EXPECT_EQ(outcome.result, DocumentCommandSubmitResult::Unsupported);
}

TEST(DocumentCommandHandleContractTest, statusDoesNotRequireLiveModelPointers)
{
    DocumentCommandHandle handle(17, DocumentRevisionIdentityBinding {42, 7});
    const auto snapshot = handle.status();

    EXPECT_EQ(snapshot.id, 17U);
    EXPECT_EQ(snapshot.document.documentInstanceId, 42U);
    EXPECT_EQ(snapshot.state, DocumentCommandState::Failed);
}

TEST(DocumentCommandHandleContractTest, recomputeObservationIsObservationOnly)
{
    DocumentCommandRecomputeObservation observation;
    observation.id = 9U;
    observation.state = DocumentCommandRecomputeState::Running;
    observation.features.push_back(
        DocumentCommandRecomputeFeatureObservation {"feature-a",
                                                    DocumentCommandRecomputeFeatureState::Waiting,
                                                    "waiting",
                                                    true});

    const auto copied = observation;
    EXPECT_EQ(copied.id, 9U);
    EXPECT_EQ(copied.features.size(), 1U);
    EXPECT_EQ(copied.features.front().featureId, "feature-a");
    EXPECT_TRUE(copied.features.front().executed);
}
