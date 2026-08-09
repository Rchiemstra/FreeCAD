// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <src/App/InitApplication.h>

#include <App/Application.h>
#include <App/CollaborativeSetPropertyOperation.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentObject.h>
#include <App/FeatureTest.h>
#include <App/PropertyStandard.h>
#include <App/RecoverySnapshot.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>
#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/Interpreter.h>
#include <Base/Writer.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace
{

constexpr auto MaximumUint64 = std::numeric_limits<std::uint64_t>::max();
constexpr std::string_view RecoveryDetachedOperationType =
    "App.Test.RecoveryDetachedOperation";

class RecoveryDetachedOperation final: public App::CollaborativeOperation
{
public:
    [[nodiscard]] std::string_view typeId() const noexcept override
    {
        return RecoveryDetachedOperationType;
    }

    void apply(App::Document&) const override {}

    [[nodiscard]] App::CollaborativePostconditionResult
    checkPostcondition(const App::Document&) const override
    {
        return {true, {}};
    }
};

class RecoveryDetachedControl
{
public:
    void run(std::stop_token stopToken)
    {
        std::unique_lock lock(_mutex);
        _entered = true;
        _changed.notify_all();
        _changed.wait(lock, stopToken, [] { return false; });
        _sawStop = stopToken.stop_requested();
        _changed.notify_all();
    }

    [[nodiscard]] bool waitUntilEntered()
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, std::chrono::seconds(2), [&] { return _entered; });
    }

    [[nodiscard]] bool waitUntilStopped()
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, std::chrono::seconds(2), [&] { return _sawStop; });
    }

private:
    std::mutex _mutex;
    std::condition_variable_any _changed;
    bool _entered {false};
    bool _sawStop {false};
};

std::shared_ptr<RecoveryDetachedControl> RecoveryControl;

void ensureRecoveryDetachedOperationRegistered()
{
    static std::once_flag once;
    std::call_once(once, [] {
        App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(RecoveryDetachedOperationType),
            [](const App::Document&, const App::CollaborativeOperationIntent&) {
                const auto control = RecoveryControl;
                if (!control) {
                    throw std::runtime_error("recovery detached control is missing");
                }
                App::CollaborativeOperationPreparation::DetachedTask task =
                    [control](std::stop_token stopToken) {
                        control->run(stopToken);
                        return std::make_unique<const RecoveryDetachedOperation>();
                    };
                return App::CollaborativeOperationPreparation {
                    {}, {}, {}, std::move(task)};
            });
    });
}

std::string recoveryFile(const App::Document& document, const char* leaf)
{
    return std::string(document.TransientDir.getValue()) + "/" + leaf;
}

class DocumentCollaborationRecoveryTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        App::ensureCollaborativeSetPropertyOperationRegistered();
        ensureRecoveryDetachedOperationRegistered();
    }

    void SetUp() override
    {
        _documentName = App::GetApplication().getUniqueDocumentName("RecoveryCollaborationTest");
        _document = App::GetApplication().newDocument(_documentName.c_str(), "recovery-test");
    }

    void TearDown() override
    {
        if (App::GetApplication().getDocument(_documentName.c_str())) {
            App::GetApplication().closeDocument(_documentName.c_str());
        }
    }

    App::Document& document() const
    {
        return *_document;
    }

    App::Document& reopenWithSameRuntimeName()
    {
        EXPECT_TRUE(App::GetApplication().closeDocument(_documentName.c_str()));
        _document = App::GetApplication().newDocument(_documentName.c_str(), "recovery-test");
        return *_document;
    }

    std::pair<App::EditSession, App::PreparedEdit> prepareFlagEdit()
    {
        auto* target = document().addObject<App::FeatureTest>("Target");
        auto* flag = dynamic_cast<App::PropertyBool*>(
            target->addDynamicProperty("App::PropertyBool", "Flag")
        );
        if (!flag) {
            throw std::runtime_error("failed to create test Flag property");
        }
        flag->setValue(false);
        document().recompute();

        auto session = document().collaborationService().beginEditSession("recovery-actor");
        App::CollaborativeOperationIntent intent {
            std::string(App::CollaborativeSetPropertyOperationType),
            {{"object", "Target"},
             {"property", "Flag"},
             {"value_type", "bool"},
             {"value", "true"}},
        };
        auto prepared = document().collaborationService().prepareEdit(session.sessionId(),
                                                                      "set-flag",
                                                                      intent,
                                                                      "recovery-test");
        return {std::move(session), std::move(prepared)};
    }

private:
    std::string _documentName;
    App::Document* _document {nullptr};
};

}  // namespace

TEST(RecoverySnapshotMetadataTest, valueContractIsPointerFree)
{
    static_assert(std::is_trivially_copyable_v<App::RecoverySnapshotMetadata>);
    static_assert(!std::is_pointer_v<decltype(
                      App::RecoverySnapshotMetadata::sourceDocumentInstanceId)>);
    static_assert(!std::is_pointer_v<decltype(
                      App::RecoverySnapshotMetadata::sourceLifecycleEpoch)>);
    static_assert(!std::is_pointer_v<decltype(
                      App::RecoverySnapshotMetadata::latestPublicationSequence)>);

    SUCCEED();
}

TEST(RecoverySnapshotMetadataTest, exactUint64ValuesRoundTripThroughDecimalXml)
{
    const App::RecoverySnapshotMetadata expected {
        App::RecoverySnapshotMetadata::CurrentSchemaVersion,
        MaximumUint64,
        MaximumUint64 - 1,
        MaximumUint64,
    };

    const auto xml = App::serializeRecoverySnapshotMetadata("A <label>",
                                                            "file&name.FCStd",
                                                            expected);
    const auto parsed = App::parseRecoverySnapshotMetadata(xml);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, expected);
    EXPECT_NE(xml.find("18446744073709551615"), std::string::npos);
    EXPECT_NE(xml.find("A &lt;label&gt;"), std::string::npos);
    EXPECT_NE(xml.find("file&amp;name.FCStd"), std::string::npos);
}

TEST(RecoverySnapshotMetadataTest, acceptsLegacyMetadataWithoutCollaborationProvenance)
{
    constexpr std::string_view legacy = R"xml(<?xml version='1.0' encoding='utf-8'?>
<AutoRecovery SchemaVersion="1">
  <Status>Created</Status>
  <Label>Legacy</Label>
  <FileName>legacy.FCStd</FileName>
</AutoRecovery>)xml";

    EXPECT_FALSE(App::parseRecoverySnapshotMetadata(legacy).has_value());
}

TEST(RecoverySnapshotMetadataTest, malformedOptionalProvenanceIsIgnored)
{
    constexpr std::string_view missingField = R"xml(<AutoRecovery SchemaVersion="1">
  <CollaborationProvenance SchemaVersion="1">
    <SourceDocumentInstanceId>41</SourceDocumentInstanceId>
    <SourceLifecycleEpoch>7</SourceLifecycleEpoch>
  </CollaborationProvenance>
</AutoRecovery>)xml";
    constexpr std::string_view overflow = R"xml(<AutoRecovery SchemaVersion="1">
  <CollaborationProvenance SchemaVersion="1">
    <SourceDocumentInstanceId>18446744073709551616</SourceDocumentInstanceId>
    <SourceLifecycleEpoch>7</SourceLifecycleEpoch>
    <LatestPublicationSequence>0</LatestPublicationSequence>
  </CollaborationProvenance>
</AutoRecovery>)xml";
    constexpr std::string_view unsupportedSchema = R"xml(<AutoRecovery SchemaVersion="1">
  <CollaborationProvenance SchemaVersion="2">
    <SourceDocumentInstanceId>41</SourceDocumentInstanceId>
    <SourceLifecycleEpoch>7</SourceLifecycleEpoch>
    <LatestPublicationSequence>0</LatestPublicationSequence>
  </CollaborationProvenance>
</AutoRecovery>)xml";
    constexpr std::string_view zeroIdentity = R"xml(<AutoRecovery SchemaVersion="1">
  <CollaborationProvenance SchemaVersion="1">
    <SourceDocumentInstanceId>0</SourceDocumentInstanceId>
    <SourceLifecycleEpoch>7</SourceLifecycleEpoch>
    <LatestPublicationSequence>0</LatestPublicationSequence>
  </CollaborationProvenance>
</AutoRecovery>)xml";

    EXPECT_FALSE(App::parseRecoverySnapshotMetadata(missingField).has_value());
    EXPECT_FALSE(App::parseRecoverySnapshotMetadata(overflow).has_value());
    EXPECT_FALSE(App::parseRecoverySnapshotMetadata(unsupportedSchema).has_value());
    EXPECT_FALSE(App::parseRecoverySnapshotMetadata(zeroIdentity).has_value());
    EXPECT_FALSE(App::parseRecoverySnapshotMetadata("<not-xml").has_value());
}

TEST_F(DocumentCollaborationRecoveryTest, capturesStableIdentityEpochAndLatestPublication)
{
    const auto identity = document().collaborationIdentity();
    const auto baseline = document().collaborationRevisions().pollPublications(
        {identity.instanceId, identity.lifecycleEpoch, 0},
        0
    );
    static_cast<void>(document().collaborationRevisions().publishUnknownModelMutation());
    static_cast<void>(document().collaborationRevisions().publishUnknownModelMutation());

    const auto captured = App::captureRecoverySnapshotMetadata(document());

    EXPECT_EQ(captured.sourceDocumentInstanceId, identity.instanceId);
    EXPECT_EQ(captured.sourceLifecycleEpoch, identity.lifecycleEpoch);
    EXPECT_EQ(captured.latestPublicationSequence, baseline.latestSequence + 2);

    static_cast<void>(document().collaborationRevisions().publishUnknownModelMutation());
    EXPECT_EQ(captured.latestPublicationSequence, baseline.latestSequence + 2);
}

TEST_F(DocumentCollaborationRecoveryTest, successfulWritePublishesCapturedProvenanceLast)
{
    static_cast<void>(document().collaborationRevisions().publishUnknownModelMutation());
    const auto expected = App::captureRecoverySnapshotMetadata(document());

    App::RecoverySnapshotSaveOptions options;
    options.compressed = true;
    ASSERT_TRUE(App::writeRecoverySnapshotToTransientDir(document(), options));

    const std::string metadataFile = recoveryFile(document(), "fc_recovery_file.xml");
    std::ifstream input(metadataFile, std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const std::string metadataXml((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
    const auto parsed = App::parseRecoverySnapshotMetadata(metadataXml);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, expected);
    EXPECT_TRUE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.fcstd")).exists());
}

TEST_F(DocumentCollaborationRecoveryTest, switchingRecoveryFormatsRetiresSupersededSnapshot)
{
    App::RecoverySnapshotSaveOptions options;
    options.compressed = true;
    ASSERT_TRUE(App::writeRecoverySnapshotToTransientDir(document(), options));
    ASSERT_TRUE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.fcstd")).exists());

    document().Label.setValue("new uncompressed recovery state");
    options.compressed = false;
    ASSERT_TRUE(App::writeRecoverySnapshotToTransientDir(document(), options));
    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.fcstd")).exists());
    EXPECT_TRUE(Base::FileInfo(recoveryFile(document(), "fc_recovery_files/Document.xml")).exists());
    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_files.tmp")).exists());

    document().Label.setValue("new compressed recovery state");
    options.compressed = true;
    ASSERT_TRUE(App::writeRecoverySnapshotToTransientDir(document(), options));
    EXPECT_TRUE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.fcstd")).exists());
    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_files")).exists());
    EXPECT_TRUE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.xml")).exists());
}

TEST_F(DocumentCollaborationRecoveryTest, pendingRecomputeRejectsWithoutCreatedMetadata)
{
    App::DocumentObject* pending = document().addObject<App::FeatureTest>("Pending");
    ASSERT_NE(pending, nullptr);
    pending->touch();
    ASSERT_TRUE(document().mustExecute());

    const std::string metadataFile = recoveryFile(document(), "fc_recovery_file.xml");
    ASSERT_FALSE(Base::FileInfo(metadataFile).exists());

    App::RecoverySnapshotSaveOptions options;
    EXPECT_THROW(App::writeRecoverySnapshotToTransientDir(document(), options),
                 Base::RuntimeError);

    EXPECT_FALSE(Base::FileInfo(metadataFile).exists());
    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.fcstd")).exists());
    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.fcstd.tmp")).exists());
}

TEST_F(DocumentCollaborationRecoveryTest, failedArchiveWriteLeavesNoCreatedMetadata)
{
    const std::string metadataFile = recoveryFile(document(), "fc_recovery_file.xml");
    {
        std::ofstream staleMetadata(metadataFile, std::ios::binary);
        ASSERT_TRUE(staleMetadata.is_open());
        staleMetadata << "<AutoRecovery><Status>Created</Status></AutoRecovery>";
    }
    ASSERT_TRUE(Base::FileInfo(metadataFile).exists());

    auto connection = document().signalSaveDocument.connect([](Base::Writer&) {
        throw Base::RuntimeError("injected recovery archive serialization failure");
    });

    App::RecoverySnapshotSaveOptions options;
    options.compressed = true;
    EXPECT_ANY_THROW(App::writeRecoverySnapshotToTransientDir(document(), options));
    connection.disconnect();

    EXPECT_FALSE(Base::FileInfo(metadataFile).exists());
    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.fcstd.tmp")).exists());
}

TEST_F(DocumentCollaborationRecoveryTest, failedUncompressedWriteLeavesNoPublishedSnapshot)
{
    auto connection = document().signalSaveDocument.connect([](Base::Writer&) {
        throw Base::RuntimeError("injected uncompressed recovery failure");
    });

    App::RecoverySnapshotSaveOptions options;
    options.compressed = false;
    EXPECT_ANY_THROW(App::writeRecoverySnapshotToTransientDir(document(), options));
    connection.disconnect();

    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.xml")).exists());
    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_files")).exists());
    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_files.tmp")).exists());
}

TEST_F(DocumentCollaborationRecoveryTest, reentrantPublicationFailsClosedAfterSerialization)
{
    auto* target = document().addObject<App::FeatureTest>("ReentrantTarget");
    ASSERT_NE(target, nullptr);
    document().recompute();
    auto connection = document().signalSaveDocument.connect([this](Base::Writer&) {
        document().getObject("ReentrantTarget")->Label.setValue("changed during recovery write");
    });

    App::RecoverySnapshotSaveOptions options;
    options.compressed = true;
    EXPECT_THROW(App::writeRecoverySnapshotToTransientDir(document(), options),
                 Base::RuntimeError);
    connection.disconnect();

    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.xml")).exists());
    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.fcstd.tmp")).exists());
}

TEST_F(DocumentCollaborationRecoveryTest, successfulRecoveryWriteIsRevisionNeutralAndKeepsEditValid)
{
    auto [session, prepared] = prepareFlagEdit();
    const auto identity = document().collaborationIdentity();
    const auto before = document().collaborationRevisions().pollPublications(
        {identity.instanceId, identity.lifecycleEpoch, 0}, 0);

    App::RecoverySnapshotSaveOptions options;
    ASSERT_TRUE(App::writeRecoverySnapshotToTransientDir(document(), options));

    const auto after = document().collaborationRevisions().pollPublications(
        {identity.instanceId, identity.lifecycleEpoch, 0}, 0);
    EXPECT_EQ(after.latestSequence, before.latestSequence);
    const auto commit = document().collaborationService().commitEdit(session.sessionId(), prepared);
    EXPECT_TRUE(commit.committed());
}

TEST_F(DocumentCollaborationRecoveryTest, reentrantCloseIsRejectedForWriterLifetime)
{
    bool closeResult = true;
    auto connection = document().signalSaveDocument.connect([&](Base::Writer&) {
        closeResult = App::GetApplication().closeDocument(document().getName());
    });

    App::RecoverySnapshotSaveOptions options;
    ASSERT_TRUE(App::writeRecoverySnapshotToTransientDir(document(), options));
    connection.disconnect();

    EXPECT_FALSE(closeResult);
    EXPECT_EQ(App::GetApplication().getDocument(document().getName()), &document());
    EXPECT_TRUE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.xml")).exists());
}

TEST_F(DocumentCollaborationRecoveryTest, administrativeAdvanceStalesPriorNativeWork)
{
    auto [session, prepared] = prepareFlagEdit();
    const auto before = document().collaborationIdentity();
    const auto expectedMetadata = App::captureRecoverySnapshotMetadata(document());

    App::RecoverySnapshotSaveOptions options;
    const auto advanced = App::GetApplication().advanceDocumentCollaborationEpoch(
        document(),
        options,
        "administrative recovery test"
    );

    EXPECT_EQ(advanced.instanceId, before.instanceId);
    EXPECT_NE(advanced.lifecycleEpoch, before.lifecycleEpoch);
    EXPECT_EQ(advanced.state, App::DocumentLifecycleState::Live);

    const auto status = document().collaborationService().sessionStatus(session.sessionId());
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->status(), App::EditSessionStatus::Cancelled);

    const auto commit = document().collaborationService().commitEdit(session.sessionId(),
                                                                     prepared);
    EXPECT_EQ(commit.status, App::DocumentCommitStatus::StaleDocument);

    const std::string metadataFile = recoveryFile(document(), "fc_recovery_file.xml");
    std::ifstream input(metadataFile, std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const std::string metadataXml((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
    const auto parsed = App::parseRecoverySnapshotMetadata(metadataXml);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, expectedMetadata);
    EXPECT_EQ(parsed->sourceDocumentInstanceId, before.instanceId);
    EXPECT_EQ(parsed->sourceLifecycleEpoch, before.lifecycleEpoch);
}

TEST_F(DocumentCollaborationRecoveryTest, administrativeAdvanceAbandonsRunningPreparation)
{
    auto* target = document().addObject<App::FeatureTest>("Target");
    ASSERT_NE(target, nullptr);
    document().recompute();
    const auto session = document().collaborationService().beginEditSession("async recovery");
    App::CollaborativeOperationIntent intent;
    intent.operationType = std::string(RecoveryDetachedOperationType);
    RecoveryControl = std::make_shared<RecoveryDetachedControl>();
    const auto executionId = document().collaborationService().prepareEditAsync(
        session.sessionId(), "running recovery preparation", intent, "recovery-test");
    ASSERT_TRUE(RecoveryControl->waitUntilEntered());

    App::RecoverySnapshotSaveOptions options;
    static_cast<void>(App::GetApplication().advanceDocumentCollaborationEpoch(
        document(), options, "cancel running preparation"));

    EXPECT_TRUE(RecoveryControl->waitUntilStopped());
    EXPECT_FALSE(document().collaborationService().preparedEditStatus(executionId).has_value());
    const auto status = document().collaborationService().sessionStatus(session.sessionId());
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->status(), App::EditSessionStatus::Cancelled);
    RecoveryControl.reset();
}

TEST_F(DocumentCollaborationRecoveryTest, failedAdministrativeWritePreservesEpochAndSession)
{
    auto [session, prepared] = prepareFlagEdit();
    static_cast<void>(prepared);
    const auto before = document().collaborationIdentity();

    auto connection = document().signalSaveDocument.connect([](Base::Writer&) {
        throw Base::RuntimeError("injected administrative recovery write failure");
    });
    App::RecoverySnapshotSaveOptions options;
    EXPECT_THROW(App::GetApplication().advanceDocumentCollaborationEpoch(
                     document(),
                     options,
                     "must not cancel"),
                 Base::RuntimeError);
    connection.disconnect();

    EXPECT_EQ(document().collaborationIdentity(), before);
    const auto status = document().collaborationService().sessionStatus(session.sessionId());
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->status(), App::EditSessionStatus::Active);
    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.xml")).exists());
    EXPECT_FALSE(Base::FileInfo(recoveryFile(document(), "fc_recovery_file.fcstd.tmp")).exists());
}

TEST_F(DocumentCollaborationRecoveryTest, crashReopenKeepsProvenanceStaleAndDiagnosticOnly)
{
    auto [oldSession, oldPrepared] = prepareFlagEdit();
    const auto source = App::captureRecoverySnapshotMetadata(document());
    const auto persistedXml = App::serializeRecoverySnapshotMetadata("Crash source", {}, source);

    App::Document& reopened = reopenWithSameRuntimeName();
    const auto reopenedIdentity = reopened.collaborationIdentity();
    const auto parsed = App::parseRecoverySnapshotMetadata(persistedXml);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, source);
    EXPECT_NE(reopenedIdentity.instanceId, parsed->sourceDocumentInstanceId);
    EXPECT_NE(reopenedIdentity.lifecycleEpoch, parsed->sourceLifecycleEpoch);
    EXPECT_NE(reopened.collaborationRevisions().pollPublications(
                  {reopenedIdentity.instanceId, reopenedIdentity.lifecycleEpoch, 0},
                  0)
                  .latestSequence,
              parsed->latestPublicationSequence)
        << "normal reopen activity must not restore the source publication counter";

    const auto oldCommit = reopened.collaborationService().commitEdit(
        oldSession.sessionId(), oldPrepared);
    EXPECT_EQ(oldCommit.status, App::DocumentCommitStatus::StaleDocument);
}

TEST_F(DocumentCollaborationRecoveryTest, nativePythonAdministrativeBindingReturnsNewEpoch)
{
    const auto before = document().collaborationIdentity();
    Base::PyGILStateLocker gil;
    PyObject* app = PyImport_ImportModule("FreeCAD");
    ASSERT_NE(app, nullptr);
    PyObject* pythonDocument = document().getPyObject();
    ASSERT_NE(pythonDocument, nullptr);
    PyObject* result = PyObject_CallMethod(
        app, "advanceDocumentCollaborationEpoch", "O", pythonDocument);
    Py_DECREF(pythonDocument);
    Py_DECREF(app);
    ASSERT_NE(result, nullptr);
    ASSERT_TRUE(PyDict_Check(result));

    PyObject* instanceObject = PyDict_GetItemString(result, "document_instance_id");
    PyObject* epochObject = PyDict_GetItemString(result, "lifecycle_epoch");
    ASSERT_NE(instanceObject, nullptr);
    ASSERT_NE(epochObject, nullptr);
    const auto returnedInstance = PyLong_AsUnsignedLongLong(instanceObject);
    const auto returnedEpoch = PyLong_AsUnsignedLongLong(epochObject);
    Py_DECREF(result);
    ASSERT_FALSE(PyErr_Occurred());

    const auto after = document().collaborationIdentity();
    EXPECT_EQ(after.instanceId, before.instanceId);
    EXPECT_GT(after.lifecycleEpoch, before.lifecycleEpoch);
    EXPECT_EQ(returnedInstance, after.instanceId);
    EXPECT_EQ(returnedEpoch, after.lifecycleEpoch);
}
#include <memory>
#include <mutex>
