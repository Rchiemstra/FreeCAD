// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QScopeGuard>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentObject.h>
#include <App/MainThreadSignal.h>
#include <App/RecoverySnapshot.h>
#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <App/MutationClassification.h>
#include <Gui/DocumentExecutionIngress.h>
#include <Gui/Application.h>
#include <Gui/AutoSaver.h>
#include <Gui/RecoveryWriteExecutor.h>
#include <QTimer>
#include <Gui/Document.h>
#include <Gui/DocumentRecovery.h>
#include <Gui/DocumentRecoveryInternal.h>
#include <Gui/MainWindow.h>
#include "CollaborationGuiTestHelpers.h"
#include "DocumentExecutionWaitTestHelpers.h"
#include <src/App/InitApplication.h>

#include <zipios++/zipoutputstream.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef __linux__
# include <filesystem>
#endif

using Gui::Dialog::DocumentRecoveryInternal::ProjectValidationResult;
using Gui::Dialog::DocumentRecoveryInternal::checkXmlFiles;
using Gui::Dialog::DocumentRecoveryInternal::checkZipData;
using Gui::Dialog::DocumentRecoveryInternal::parseRecoveryCollaborationProvenance;
using Gui::Dialog::DocumentRecoveryInternal::validateProjectArchive;

namespace
{

constexpr const char* kMinimalDocumentXml =
    "<?xml version='1.0' encoding='utf-8'?>\n"
    "<Document SchemaVersion=\"4\">\n"
    "</Document>\n";

constexpr const char* kMinimalGuiDocumentXml =
    "<?xml version='1.0' encoding='utf-8'?>\n"
    "<GuiDocument SchemaVersion=\"1\">\n"
    "</GuiDocument>\n";

constexpr const char* kMalformedXml = "<?xml version='1.0'?><Document>";

QString writeFcstdLikeZipAt(const QString& directoryPath, const QString& name,
                            int extraBinaryEntries, bool includeGuiDocument,
                            const char* documentXml, const char* guiXml = nullptr)
{
    const QString path = QDir(directoryPath).filePath(name);
    {
        zipios::ZipOutputStream zos(path.toStdString());
        zos.putNextEntry("Document.xml");
        zos << documentXml;
        zos.closeEntry();

        if (includeGuiDocument) {
            zos.putNextEntry("GuiDocument.xml");
            zos << (guiXml ? guiXml : kMinimalGuiDocumentXml);
            zos.closeEntry();
        }

        for (int i = 0; i < extraBinaryEntries; ++i) {
            const std::string entry = "Extra/Data" + std::to_string(i) + ".bin";
            zos.putNextEntry(entry);
            zos << "payload-" << i;
            zos.closeEntry();
        }
        zos.close();
    }
    return path;
}

QString writeFcstdLikeZip(const QTemporaryDir& dir, const QString& name,
                          int extraBinaryEntries, bool includeGuiDocument,
                          const char* documentXml, const char* guiXml = nullptr)
{
    return writeFcstdLikeZipAt(dir.path(), name, extraBinaryEntries, includeGuiDocument,
                               documentXml, guiXml);
}

void initializeRecoveryGui()
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    static int argc = 1;
    static char executable[] = "Gui_tests_run";
    static char* argv[] = {executable, nullptr};
    if (!QApplication::instance()) {
        new QApplication(argc, argv);
    }

    tests::initApplication();
    if (!Gui::Application::Instance) {
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        new Gui::Application(true);
    }
    if (!Gui::MainWindow::getInstance()) {
        new Gui::MainWindow();
    }
}

std::string metadataXmlWithProvenance(std::string_view provenanceXml)
{
    std::string xml =
        "<?xml version='1.0' encoding='utf-8'?>\n"
        "<AutoRecovery SchemaVersion=\"1\">\n"
        "  <Status>Created</Status>\n"
        "  <Label>Recovered</Label>\n"
        "  <FileName></FileName>\n";
    xml.append(provenanceXml);
    xml.append("\n</AutoRecovery>\n");
    return xml;
}

bool processEventsUntil(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

class RecoveryGuiTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        initializeRecoveryGui();
    }
};

class AutoSaverRecoveryTest: public RecoveryGuiTest
{
protected:
    static void SetUpTestSuite()
    {
        RecoveryGuiTest::SetUpTestSuite();
        Gui::AutoSaver::instance()->setTimeout(3600000);
        Gui::AutoSaver::instance()->setCompressed(true);
    }

    static void TearDownTestSuite()
    {
        Gui::AutoSaver::instance()->setTimeout(900000);
    }

    void SetUp() override
    {
        App::DocumentInitFlags flags;
        flags.createView = false;
        documentName = App::GetApplication().getUniqueDocumentName("guiRecoveryAutosave");
        document = App::GetApplication().newDocument(
            documentName.c_str(), "gui recovery autosave", flags);
        ASSERT_NE(document, nullptr);
        guiDocument = Gui::Application::Instance->getDocument(document);
        ASSERT_NE(guiDocument, nullptr);
        object = document->addObject("App::FeatureTest", "AutosaveTarget");
        ASSERT_NE(object, nullptr);
        Gui::Test::recomputeWithoutBlockingGui(*document);
        ASSERT_TRUE(document->canWriteRecoverySnapshot());
        removeRecoveryOutputs();
    }

    void TearDown() override
    {
        if (document) {
            const QFileInfo metadataInfo(metadataPath());
            if (metadataInfo.isDir()) {
                QDir(metadataInfo.absoluteFilePath()).removeRecursively();
            }
            if (App::GetApplication().getDocument(documentName.c_str())) {
                App::GetApplication().closeDocument(documentName.c_str());
                QCoreApplication::processEvents(QEventLoop::AllEvents);
            }
        }
        document = nullptr;
        guiDocument = nullptr;
        object = nullptr;
    }

    QString archivePath() const
    {
        return QDir(QString::fromUtf8(document->TransientDir.getValue()))
            .filePath(QStringLiteral("fc_recovery_file.fcstd"));
    }

    QString metadataPath() const
    {
        return QDir(QString::fromUtf8(document->TransientDir.getValue()))
            .filePath(QStringLiteral("fc_recovery_file.xml"));
    }

    void removeRecoveryOutputs() const
    {
        QFile::remove(archivePath());
        QFile::remove(metadataPath());
    }

    void flush() const
    {
        Gui::Test::flushAutoSaverWithoutBlockingGui(*document);
    }

    // The lane owner thread publishes the archive first and its metadata last,
    // so wait for both rather than racing the metadata write.
    bool waitForRecoveryArchive() const
    {
        return processEventsUntil([this] {
            if (!QFileInfo(archivePath()).isFile() && document->canWriteRecoverySnapshot()) {
                Gui::Test::flushAutoSaverWithoutBlockingGui(*document);
            }
            return QFileInfo(archivePath()).isFile() && QFileInfo(metadataPath()).isFile();
        });
    }

    App::Document* document {nullptr};
    Gui::Document* guiDocument {nullptr};
    App::DocumentObject* object {nullptr};
    std::string documentName;
};

#ifdef __linux__
int countOpenFds()
{
    namespace fs = std::filesystem;
    int count = 0;
    std::error_code ec;
    for (fs::directory_iterator it(fs::path("/proc/self/fd"), ec), end; !ec && it != end;
         it.increment(ec)) {
        ++count;
    }
    return count;
}
#endif

}  // namespace

TEST(DocumentRecoveryProvenance, CopiesExactDecimalUint64Values)
{
    const auto xml = metadataXmlWithProvenance(
        "  <CollaborationProvenance SchemaVersion=\"1\">\n"
        "    <SourceDocumentInstanceId>18446744073709551615</SourceDocumentInstanceId>\n"
        "    <SourceLifecycleEpoch>9223372036854775808</SourceLifecycleEpoch>\n"
        "    <LatestPublicationSequence>9007199254740993</LatestPublicationSequence>\n"
        "  </CollaborationProvenance>"
    );

    const auto provenance = parseRecoveryCollaborationProvenance(xml);
    ASSERT_TRUE(provenance.has_value());
    EXPECT_EQ(provenance->schemaVersion, 1U);
    EXPECT_EQ(provenance->sourceDocumentInstanceId,
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(provenance->sourceLifecycleEpoch, UINT64_C(9223372036854775808));
    EXPECT_EQ(provenance->latestPublicationSequence, UINT64_C(9007199254740993));
}

TEST(DocumentRecoveryProvenance, LegacyMetadataRemainsAcceptedWithoutProvenance)
{
    const auto xml = metadataXmlWithProvenance("");
    EXPECT_FALSE(parseRecoveryCollaborationProvenance(xml).has_value());
}

TEST(DocumentRecoveryProvenance, MalformedAndUnsupportedOptionalProvenanceAreIgnored)
{
    const std::vector<std::string_view> invalidProvenance {
        "  <CollaborationProvenance SchemaVersion=\"2\">\n"
        "    <SourceDocumentInstanceId>1</SourceDocumentInstanceId>\n"
        "    <SourceLifecycleEpoch>2</SourceLifecycleEpoch>\n"
        "    <LatestPublicationSequence>3</LatestPublicationSequence>\n"
        "  </CollaborationProvenance>",
        "  <CollaborationProvenance SchemaVersion=\"1\">\n"
        "    <SourceDocumentInstanceId>-1</SourceDocumentInstanceId>\n"
        "    <SourceLifecycleEpoch>2</SourceLifecycleEpoch>\n"
        "    <LatestPublicationSequence>3</LatestPublicationSequence>\n"
        "  </CollaborationProvenance>",
        "  <CollaborationProvenance SchemaVersion=\"1\">\n"
        "    <SourceDocumentInstanceId>18446744073709551616</SourceDocumentInstanceId>\n"
        "    <SourceLifecycleEpoch>2</SourceLifecycleEpoch>\n"
        "    <LatestPublicationSequence>3</LatestPublicationSequence>\n"
        "  </CollaborationProvenance>",
        "  <CollaborationProvenance SchemaVersion=\"1\">\n"
        "    <SourceDocumentInstanceId>1</SourceDocumentInstanceId>\n"
        "    <LatestPublicationSequence>3</LatestPublicationSequence>\n"
        "  </CollaborationProvenance>",
    };

    for (const auto provenance : invalidProvenance) {
        EXPECT_FALSE(parseRecoveryCollaborationProvenance(
                         metadataXmlWithProvenance(provenance))
                         .has_value())
            << provenance;
    }
}

TEST_F(RecoveryGuiTest, LegacyAndMalformedOptionalProvenanceRemainRecoverableEntries)
{
    const std::vector<std::string> metadataDocuments {
        metadataXmlWithProvenance(""),
        metadataXmlWithProvenance(
            "  <CollaborationProvenance SchemaVersion=\"1\">\n"
            "    <SourceDocumentInstanceId>not-a-number</SourceDocumentInstanceId>\n"
            "    <SourceLifecycleEpoch>2</SourceLifecycleEpoch>\n"
            "    <LatestPublicationSequence>3</LatestPublicationSequence>\n"
            "  </CollaborationProvenance>"
        ),
    };

    for (const auto& metadata : metadataDocuments) {
        QTemporaryDir temporary;
        ASSERT_TRUE(temporary.isValid());
        const QString recoveryDir = temporary.filePath(QStringLiteral("recovery"));
        ASSERT_TRUE(QDir().mkpath(recoveryDir));
        writeFcstdLikeZipAt(recoveryDir, QStringLiteral("fc_recovery_file.fcstd"), 0,
                            false, kMinimalDocumentXml);

        QFile metadataFile(QDir(recoveryDir).filePath(QStringLiteral("fc_recovery_file.xml")));
        ASSERT_TRUE(metadataFile.open(QIODevice::WriteOnly));
        ASSERT_EQ(metadataFile.write(metadata.data(), static_cast<qint64>(metadata.size())),
                  static_cast<qint64>(metadata.size()));
        metadataFile.close();

        Gui::Dialog::DocumentRecovery dialog({QFileInfo(recoveryDir)});
        EXPECT_TRUE(dialog.foundDocuments());
    }
}

TEST_F(RecoveryGuiTest, ReopenedDocumentKeepsProvenanceDiagnosticAndGetsFreshRuntimeIdentity)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString projectPath = temporary.filePath(QStringLiteral("source.FCStd"));

    App::DocumentInitFlags flags;
    flags.createView = false;
    const std::string sourceName =
        App::GetApplication().getUniqueDocumentName("recoveryIdentitySource");
    auto* source = App::GetApplication().newDocument(
        sourceName.c_str(), "recovery identity source", flags);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(source->addObject("App::FeatureTest", "SourceObject"), nullptr);
    Gui::Test::recomputeWithoutBlockingGui(*source);
    Gui::Test::saveAsWithoutBlockingGui(*source, projectPath.toUtf8().constData());
    ASSERT_TRUE(QFileInfo::exists(projectPath));

    const auto sourceIdentity = source->collaborationIdentity();
    App::RecoverySnapshotMetadata metadata;
    metadata.sourceDocumentInstanceId = sourceIdentity.instanceId;
    metadata.sourceLifecycleEpoch = sourceIdentity.lifecycleEpoch;
    metadata.latestPublicationSequence = std::numeric_limits<std::uint64_t>::max();
    const std::string metadataXml = App::serializeRecoverySnapshotMetadata(
        "Recovered source", {}, metadata);

    const QString recoveryDir = temporary.filePath(QStringLiteral("recovery"));
    ASSERT_TRUE(QDir().mkpath(recoveryDir));
    const QString recoveryArchive =
        QDir(recoveryDir).filePath(QStringLiteral("fc_recovery_file.fcstd"));
    ASSERT_TRUE(QFile::copy(projectPath, recoveryArchive));
    QFile metadataFile(QDir(recoveryDir).filePath(QStringLiteral("fc_recovery_file.xml")));
    ASSERT_TRUE(metadataFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(metadataFile.write(metadataXml.data(), static_cast<qint64>(metadataXml.size())),
              static_cast<qint64>(metadataXml.size()));
    metadataFile.close();

    App::GetApplication().closeDocument(sourceName.c_str());
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    const auto parsed = parseRecoveryCollaborationProvenance(metadataXml);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->sourceDocumentInstanceId, sourceIdentity.instanceId);
    ASSERT_EQ(parsed->sourceLifecycleEpoch, sourceIdentity.lifecycleEpoch);

    const auto documentsBeforeRecovery = App::GetApplication().getDocuments();
    Gui::Dialog::DocumentRecovery dialog({QFileInfo(recoveryDir)});
    ASSERT_TRUE(dialog.foundDocuments());
    dialog.accept();

    const auto documentsAfterRecovery = App::GetApplication().getDocuments();
    const auto recovered = std::find_if(
        documentsAfterRecovery.begin(), documentsAfterRecovery.end(),
        [&](const App::Document* candidate) {
            return std::find(documentsBeforeRecovery.begin(),
                             documentsBeforeRecovery.end(),
                             candidate)
                == documentsBeforeRecovery.end();
        });
    ASSERT_NE(recovered, documentsAfterRecovery.end());
    auto* reopened = *recovered;
    ASSERT_NE(reopened, nullptr);
    const auto reopenedIdentity = reopened->collaborationIdentity();
    EXPECT_NE(reopenedIdentity.instanceId, parsed->sourceDocumentInstanceId);
    EXPECT_NE(reopenedIdentity.lifecycleEpoch, parsed->sourceLifecycleEpoch);

    const auto revisions = reopened->collaborationRevisions().pollPublications(
        {reopenedIdentity.instanceId, reopenedIdentity.lifecycleEpoch, 0}, 0);
    EXPECT_NE(revisions.latestSequence, parsed->latestPublicationSequence)
        << "recovery provenance must not restore the old publication stream";

    App::GetApplication().closeDocument(reopened->getName());
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

TEST_F(RecoveryGuiTest, StatusRewritePreservesExactCollaborationProvenance)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString recoveryDir = temporary.filePath(QStringLiteral("recovery"));
    ASSERT_TRUE(QDir().mkpath(recoveryDir));
    writeFcstdLikeZipAt(recoveryDir, QStringLiteral("fc_recovery_file.fcstd"), 0,
                        false, kMinimalDocumentXml);

    const QString corruptOriginal = temporary.filePath(QStringLiteral("corrupt.FCStd"));
    QFile original(corruptOriginal);
    ASSERT_TRUE(original.open(QIODevice::WriteOnly));
    ASSERT_GT(original.write("not a project"), 0);
    original.close();

    const App::RecoverySnapshotMetadata expected {
        App::RecoverySnapshotMetadata::CurrentSchemaVersion,
        std::numeric_limits<std::uint64_t>::max(),
        UINT64_C(9223372036854775808),
        UINT64_C(9007199254740993),
    };
    const auto metadataXml = App::serializeRecoverySnapshotMetadata(
        "Recovered", corruptOriginal.toStdString(), expected);
    const QString metadataPath =
        QDir(recoveryDir).filePath(QStringLiteral("fc_recovery_file.xml"));
    QFile metadataFile(metadataPath);
    ASSERT_TRUE(metadataFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(metadataFile.write(metadataXml.data(), static_cast<qint64>(metadataXml.size())),
              static_cast<qint64>(metadataXml.size()));
    metadataFile.close();

    Gui::Dialog::DocumentRecovery dialog({QFileInfo(recoveryDir)});
    ASSERT_TRUE(dialog.foundDocuments());

    ASSERT_TRUE(metadataFile.open(QIODevice::ReadOnly));
    const QByteArray rewritten = metadataFile.readAll();
    metadataFile.close();
    EXPECT_TRUE(rewritten.contains("<Status>Corrupted</Status>"));
    const auto parsed = parseRecoveryCollaborationProvenance(
        std::string_view(rewritten.constData(), static_cast<std::size_t>(rewritten.size())));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, expected);
}

TEST_F(AutoSaverRecoveryTest, OrdinaryChangesWaitForConfiguredTimer)
{
    object->Label.setValue("ordinary dirty work");
    Gui::Test::recomputeWithoutBlockingGui(*document);
    ASSERT_TRUE(document->canWriteRecoverySnapshot());
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    EXPECT_FALSE(QFileInfo::exists(archivePath()));
    EXPECT_FALSE(QFileInfo::exists(metadataPath()));

    flush();
    EXPECT_TRUE(QFileInfo(archivePath()).isFile());
    EXPECT_TRUE(QFileInfo(metadataPath()).isFile());
}

TEST_F(AutoSaverRecoveryTest, LaterMutationInSameCategorySchedulesAnotherSnapshot)
{
    object->Label.setValue("first model mutation");
    Gui::Test::recomputeWithoutBlockingGui(*document);
    flush();
    ASSERT_TRUE(QFileInfo(archivePath()).isFile());
    ASSERT_TRUE(QFileInfo(metadataPath()).isFile());

    removeRecoveryOutputs();
    object->Label.setValue("second model mutation");
    Gui::Test::recomputeWithoutBlockingGui(*document);
    flush();

    EXPECT_TRUE(QFileInfo(archivePath()).isFile())
        << "Model-to-Model mutations must re-arm auto-recovery after a successful snapshot";
    EXPECT_TRUE(QFileInfo(metadataPath()).isFile());
}

TEST_F(AutoSaverRecoveryTest, RecoveryOutcomeDoesNotAdvanceCanonicalSavepoint)
{
    std::optional<App::DocumentSaveOutcome> recoveryOutcome;
    auto connection = document->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome& outcome) {
            if (outcome.intent == App::DocumentSaveIntent::Recovery) {
                recoveryOutcome = outcome;
            }
        });

    object->Label.setValue("recovery outcome source");
    Gui::Test::recomputeWithoutBlockingGui(*document);
    flush();
    connection.disconnect();

    ASSERT_TRUE(recoveryOutcome.has_value());
    EXPECT_EQ(recoveryOutcome->disposition, App::DocumentSaveDisposition::CopyWritten);
    EXPECT_TRUE(recoveryOutcome->fileWritten);
    EXPECT_TRUE(document->hasPendingFileChanges());
    EXPECT_FALSE(document->lastCanonicalSaveFailed());
}

TEST_F(AutoSaverRecoveryTest, AppTransactionDefersAndRetriesAfterStableSignal)
{
    document->openTransaction("defer recovery write");
    object->Label.setValue("inside app transaction");
    ASSERT_FALSE(document->canWriteRecoverySnapshot());

    flush();
    EXPECT_FALSE(QFileInfo::exists(archivePath()));
    EXPECT_FALSE(QFileInfo::exists(metadataPath()));

    document->commitTransaction();
    Gui::Test::recomputeWithoutBlockingGui(*document);
    ASSERT_TRUE(document->canWriteRecoverySnapshot());
    EXPECT_TRUE(waitForRecoveryArchive());
    EXPECT_TRUE(QFileInfo(metadataPath()).isFile());
}

TEST_F(AutoSaverRecoveryTest, PendingRecomputeDefersAndRetriesAfterRecompute)
{
    object->touch();
    ASSERT_TRUE(document->mustExecute());

    flush();
    EXPECT_FALSE(QFileInfo::exists(archivePath()));
    EXPECT_FALSE(QFileInfo::exists(metadataPath()));

    Gui::Test::recomputeWithoutBlockingGui(*document);
    ASSERT_TRUE(document->canWriteRecoverySnapshot());
    EXPECT_TRUE(waitForRecoveryArchive());
    EXPECT_TRUE(QFileInfo(metadataPath()).isFile());
}

TEST_F(AutoSaverRecoveryTest, QueuedRetryCannotAutosaveReplacementWithReusedName)
{
    const auto oldIdentity = document->collaborationIdentity();
    document->openTransaction("queue identity-bound autosave retry");
    object->Label.setValue("old document pending retry");
    flush();
    ASSERT_FALSE(QFileInfo::exists(archivePath()));
    document->commitTransaction();
    Gui::Test::recomputeWithoutBlockingGui(*document);

    // The stable-state retry may already have dispatched during recompute's
    // test wait. Use the owner's close policy before replacing the identity;
    // an executing serializer must retire before direct App close can succeed.
    Gui::AutoSaver::instance()->recoveryWrites().cancelDocument(*document);
    ASSERT_TRUE(processEventsUntil([&] {
        return App::GetApplication().closeDocument(documentName.c_str());
    }));

    App::DocumentInitFlags flags;
    flags.createView = false;
    document = App::GetApplication().newDocument(
        documentName.c_str(), "replacement with reused name", flags);
    ASSERT_NE(document, nullptr);
    guiDocument = Gui::Application::Instance->getDocument(document);
    ASSERT_NE(guiDocument, nullptr);
    object = document->addObject("App::FeatureTest", "ReplacementTarget");
    ASSERT_NE(object, nullptr);
    Gui::Test::recomputeWithoutBlockingGui(*document);
    removeRecoveryOutputs();
    object->Label.setValue("replacement ordinary dirty work");

    // Deliver an old identity explicitly after replacement so the close wait
    // cannot accidentally consume the only stale retry before this assertion.
    bool retried = false;
    QTimer::singleShot(0, QCoreApplication::instance(), [&] {
        Gui::Test::deliverRecoveryRetry(
            documentName, oldIdentity.instanceId, oldIdentity.lifecycleEpoch);
        retried = true;
    });
    ASSERT_TRUE(processEventsUntil([&] {
        return retried && document->executionLane()->isIdle();
    }));
    EXPECT_FALSE(QFileInfo::exists(archivePath()));
    EXPECT_FALSE(QFileInfo::exists(metadataPath()));
}

TEST_F(AutoSaverRecoveryTest, GuiTransactionDefersUntilGroupedUndoFinishes)
{
    document->setMaxUndoStackSize(8);
    document->openTransaction("gui transaction source");
    object->Label.setValue("undo me");
    document->commitTransaction();
    Gui::Test::recomputeWithoutBlockingGui(*document);
    removeRecoveryOutputs();

    bool flushedInsideGuiTransaction = false;
    bool archiveExistedInsideGuiTransaction = true;
    auto connection = document->signalUndo.connect([&](const App::Document&) {
        ASSERT_TRUE(guiDocument->isPerformingTransaction());
        flushedInsideGuiTransaction = true;
        flush();
        archiveExistedInsideGuiTransaction = QFileInfo::exists(archivePath());
    });

    guiDocument->undo(1);
    ASSERT_TRUE(processEventsUntil([&] { return flushedInsideGuiTransaction; }));
    ASSERT_TRUE(processEventsUntil([this] { return !guiDocument->isPerformingTransaction(); }));
    connection.disconnect();

    ASSERT_TRUE(flushedInsideGuiTransaction);
    EXPECT_FALSE(archiveExistedInsideGuiTransaction);
    Gui::Test::recomputeWithoutBlockingGui(*document);
    ASSERT_TRUE(document->canWriteRecoverySnapshot());
    EXPECT_TRUE(waitForRecoveryArchive());
    EXPECT_TRUE(QFileInfo(metadataPath()).isFile());
}

TEST_F(AutoSaverRecoveryTest, FailedWriteRetainsDirtyWorkForNextRetry)
{
    object->Label.setValue("must survive failed recovery write");
    Gui::Test::recomputeWithoutBlockingGui(*document);
    ASSERT_TRUE(document->canWriteRecoverySnapshot());
    ASSERT_TRUE(QDir().mkpath(metadataPath()));
    QFile blocker(QDir(metadataPath()).filePath(QStringLiteral("keep-directory-nonempty")));
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    ASSERT_EQ(blocker.write("block"), 5);
    blocker.close();

    flush();
    EXPECT_FALSE(QFileInfo(metadataPath()).isFile());

    ASSERT_TRUE(QDir(metadataPath()).removeRecursively());
    flush();
    EXPECT_TRUE(QFileInfo(archivePath()).isFile());
    EXPECT_TRUE(QFileInfo(metadataPath()).isFile());
}

TEST_F(AutoSaverRecoveryTest, SuccessfulAttemptConsumesOnlyItsCapturedDirtyState)
{
    Gui::AutoSaveProperty state(document);
    state.markDirtyForAutosave();
    ASSERT_TRUE(state.beginSaveAttempt());
    EXPECT_FALSE(state.beginSaveAttempt());

    state.markDirtyForAutosave();
    state.finishSuccessfulSaveAttempt();
    EXPECT_TRUE(state.beginSaveAttempt())
        << "a notification received during serialization must remain pending";
    state.finishSuccessfulSaveAttempt();
    EXPECT_FALSE(state.beginSaveAttempt());

    state.markDirtyForAutosave();
    ASSERT_TRUE(state.beginSaveAttempt());
    state.restoreFailedSaveAttempt();
    EXPECT_TRUE(state.beginSaveAttempt()) << "a failed write must not consume dirty work";
    state.finishSuccessfulSaveAttempt();
}

TEST(DocumentRecoveryValidation, CheckZipDataRejectsMissingFile)
{
    EXPECT_EQ(checkZipData(QStringLiteral("Z:/definitely/missing/project.FCStd")),
              ProjectValidationResult::OpenFailed);
}

TEST(DocumentRecoveryValidation, CheckZipDataRejectsEmptyArchive)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path = dir.filePath(QStringLiteral("empty.FCStd"));
    {
        zipios::ZipOutputStream zos(path.toStdString());
        zos.close();
    }

    EXPECT_EQ(checkZipData(path), ProjectValidationResult::InvalidContent);
    EXPECT_EQ(validateProjectArchive(path), ProjectValidationResult::InvalidContent);
}

TEST(DocumentRecoveryValidation, CheckZipDataRejectsGarbageBytes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path = dir.filePath(QStringLiteral("garbage.FCStd"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("this is not a zip archive"), 25);
    }

    EXPECT_EQ(checkZipData(path), ProjectValidationResult::OpenFailed);
    EXPECT_EQ(validateProjectArchive(path), ProjectValidationResult::OpenFailed);
}

TEST(DocumentRecoveryValidation, CheckZipDataRejectsDirectoryPath)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    EXPECT_EQ(checkZipData(dir.path()), ProjectValidationResult::OpenFailed);
    EXPECT_EQ(validateProjectArchive(dir.path()), ProjectValidationResult::OpenFailed);
}

TEST(DocumentRecoveryValidation, CheckZipDataAcceptsMultiEntryArchive)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path =
        writeFcstdLikeZip(dir, QStringLiteral("ok.FCStd"), 8, false, kMinimalDocumentXml);
    EXPECT_EQ(checkZipData(path), ProjectValidationResult::Ok);
}

TEST(DocumentRecoveryValidation, CheckXmlFilesAcceptsDocumentXml)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path =
        writeFcstdLikeZip(dir, QStringLiteral("doc.FCStd"), 0, false, kMinimalDocumentXml);
    EXPECT_EQ(checkXmlFiles(path), ProjectValidationResult::Ok);
    EXPECT_EQ(validateProjectArchive(path), ProjectValidationResult::Ok);
}

TEST(DocumentRecoveryValidation, CheckXmlFilesAcceptsOptionalGuiDocument)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path =
        writeFcstdLikeZip(dir, QStringLiteral("gui.FCStd"), 0, true, kMinimalDocumentXml);
    EXPECT_EQ(checkXmlFiles(path), ProjectValidationResult::Ok);
    EXPECT_EQ(validateProjectArchive(path), ProjectValidationResult::Ok);
}

TEST(DocumentRecoveryValidation, ValidateProjectArchiveAcceptsMultiEntryWithGui)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path =
        writeFcstdLikeZip(dir, QStringLiteral("full.FCStd"), 12, true, kMinimalDocumentXml);
    EXPECT_EQ(checkZipData(path), ProjectValidationResult::Ok);
    EXPECT_EQ(checkXmlFiles(path), ProjectValidationResult::Ok);
    EXPECT_EQ(validateProjectArchive(path), ProjectValidationResult::Ok);
}

TEST(DocumentRecoveryValidation, CheckXmlFilesRejectsMalformedDocumentXml)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path =
        writeFcstdLikeZip(dir, QStringLiteral("bad.FCStd"), 0, false, kMalformedXml);
    EXPECT_EQ(checkXmlFiles(path), ProjectValidationResult::InvalidContent);
    EXPECT_EQ(validateProjectArchive(path), ProjectValidationResult::InvalidContent);
}

TEST(DocumentRecoveryValidation, CheckXmlFilesRejectsMalformedGuiDocumentXml)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path = writeFcstdLikeZip(dir, QStringLiteral("badgui.FCStd"), 0, true,
                                           kMinimalDocumentXml, kMalformedXml);
    EXPECT_EQ(checkXmlFiles(path), ProjectValidationResult::InvalidContent);
    EXPECT_EQ(validateProjectArchive(path), ProjectValidationResult::InvalidContent);
}

TEST(DocumentRecoveryValidation, CheckXmlFilesRejectsMissingDocumentXml)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString path = dir.filePath(QStringLiteral("nodoc.FCStd"));
    {
        zipios::ZipOutputStream zos(path.toStdString());
        zos.putNextEntry("Other.xml");
        zos << "<Other/>";
        zos.closeEntry();
        zos.close();
    }

    EXPECT_EQ(checkXmlFiles(path), ProjectValidationResult::InvalidContent);
    EXPECT_EQ(validateProjectArchive(path), ProjectValidationResult::InvalidContent);
}

// Regression: ZipFile::getInputStream() returns heap streams. Without unique_ptr ownership,
// validating a multi-entry FCStd repeatedly exhausts the Windows CRT 512 fopen limit and
// falsely reports OpenFailed. Stress checkZipData (one stream per entry) as the primary leak vector.
TEST(DocumentRecoveryValidation, CheckZipDataDoesNotLeakHandlesAcrossRepeatedValidation)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    constexpr int kExtraEntries = 40;
    constexpr int kIterations = 800;  // 40 * 800 >> 512 if streams leaked

    const QString path = writeFcstdLikeZip(dir, QStringLiteral("leakprobe.FCStd"), kExtraEntries,
                                           true, kMinimalDocumentXml);

    for (int i = 0; i < kIterations; ++i) {
        ASSERT_EQ(checkZipData(path), ProjectValidationResult::Ok) << "iteration " << i;
    }

    // Combined pre-check must stay healthy after the zip-only stress.
    EXPECT_EQ(validateProjectArchive(path), ProjectValidationResult::Ok);
}

#ifdef __linux__
// Catches stream ownership regressions even when ulimit -n is high enough that EMFILE never fires.
TEST(DocumentRecoveryValidation, CheckZipDataDoesNotGrowOpenFdCount)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    constexpr int kExtraEntries = 30;
    constexpr int kIterations = 400;

    const QString path = writeFcstdLikeZip(dir, QStringLiteral("fdprobe.FCStd"), kExtraEntries,
                                           true, kMinimalDocumentXml);

    // Warm caches / one-time allocations before measuring.
    ASSERT_EQ(checkZipData(path), ProjectValidationResult::Ok);
    ASSERT_EQ(validateProjectArchive(path), ProjectValidationResult::Ok);

    const int fdsBefore = countOpenFds();
    ASSERT_GT(fdsBefore, 0);

    for (int i = 0; i < kIterations; ++i) {
        ASSERT_EQ(checkZipData(path), ProjectValidationResult::Ok) << "iteration " << i;
        ASSERT_EQ(validateProjectArchive(path), ProjectValidationResult::Ok) << "iteration " << i;
    }

    const int fdsAfter = countOpenFds();
    EXPECT_LE(fdsAfter - fdsBefore, 2)
        << "open FD count grew from " << fdsBefore << " to " << fdsAfter
        << " across repeated validation (likely ZipInputStream leak)";
}
#endif

TEST_F(AutoSaverRecoveryTest, ForeignPostconditionDefersRecoveryWithoutPoisoningCommit)
{
    using namespace std::chrono_literals;
    std::optional<App::DocumentSaveOutcome> recoveryOutcome;
    auto connection = document->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome& outcome) {
            if (outcome.intent == App::DocumentSaveIntent::Recovery) {
                recoveryOutcome = outcome;
            }
        });
    const auto disconnect = qScopeGuard([&] { connection.disconnect(); });
    object->Label.setValue("pending recovery during another document's commit");
    Gui::Test::recomputeWithoutBlockingGui(*document);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    ASSERT_TRUE(document->canWriteRecoverySnapshot());

    App::DocumentInitFlags flags;
    flags.createView = false;
    const auto foreignName = App::GetApplication().getUniqueDocumentName("foreignPostcondition");
    auto* foreign = App::GetApplication().newDocument(foreignName.c_str(), "foreign", flags);
    ASSERT_NE(foreign, nullptr);
    const auto closeForeign = qScopeGuard([&] {
        App::GetApplication().closeDocument(foreignName.c_str());
    });
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future();
    auto audit = foreign->executionLane()->postToOwner([&] {
        foreign->beginCollaborationReadOnlyPostconditionAudit();
        const auto endAudit = qScopeGuard([&] {
            foreign->endCollaborationAtomicPresentationAudit();
        });
        entered.set_value();
        released.wait();
    });
    bool didRelease = false;
    auto releaseAudit = [&] {
        if (!didRelease) {
            release.set_value();
            didRelease = true;
        }
        audit.wait();
    };
    const auto finishAudit = qScopeGuard(releaseAudit);
    ASSERT_EQ(enteredFuture.wait_for(5s), std::future_status::ready);

    // Exercise production's asynchronous scheduler, not the synchronous test
    // flush helper. No recovery IO may poison an unrelated read-only audit.
    Gui::AutoSaver::instance()->flushPendingSave(QString::fromStdString(documentName));
    // Pending recovery now retains its lane work while waiting for a fair
    // reservation. It must not serialize or report an outcome under the audit.
    ASSERT_FALSE(document->executionLane()->isIdle());
    const bool violated = foreign->collaborationAtomicPresentationAuditViolated();
    EXPECT_FALSE(QFileInfo::exists(archivePath()));
    releaseAudit();
    ASSERT_FALSE(violated) << "background recovery must not invalidate another document's commit";

    ASSERT_TRUE(processEventsUntil([&] {
        return QFileInfo::exists(archivePath()) && QFileInfo::exists(metadataPath())
            && recoveryOutcome && recoveryOutcome->fileWritten;
    })) << "deferred recovery must resume after native admission is released";
}

TEST_F(AutoSaverRecoveryTest, RecoveryRetainsEligibilityUnderTransactionLock)
{
    bool reported = false;
    auto connection = document->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome& outcome) {
            reported = outcome.intent == App::DocumentSaveIntent::Recovery && outcome.fileWritten;
        });
    const auto disconnect = qScopeGuard([&] { connection.disconnect(); });
    document->lockTransaction();
    const auto unlock = qScopeGuard([&] { document->unlockTransaction(); });
    ASSERT_TRUE(document->canWriteRecoverySnapshot());
    ASSERT_FALSE(document->getMutationReadiness().ready);
    Gui::AutoSaver::instance()->flushPendingSave(QString::fromStdString(documentName));
    ASSERT_TRUE(processEventsUntil([&] { return QFileInfo::exists(metadataPath()) && reported; }));
}

TEST_F(AutoSaverRecoveryTest, ClosedDocumentCompletionCannotResetReplacementAttempt)
{
    const auto oldIdentity = document->collaborationIdentity();
    App::GetApplication().closeDocument(documentName.c_str());
    document = App::GetApplication().newDocument(documentName.c_str());
    object = document->addObject("App::FeatureTest", "Replacement");
    Gui::Test::recomputeWithoutBlockingGui(*document);
    QCoreApplication::processEvents();
    ASSERT_TRUE(Gui::Test::beginAutoSaveAttempt(*document));
    Gui::Test::deliverRecoveryCompletion(
        documentName, oldIdentity.instanceId, oldIdentity.lifecycleEpoch);
    EXPECT_FALSE(Gui::Test::beginAutoSaveAttempt(*document))
        << "late completion must leave the replacement's in-flight attempt alone";
}

TEST_F(AutoSaverRecoveryTest, TimedOutRecoveryWaitCanFinishAfterCallerReturns)
{
    using namespace std::chrono_literals;
    auto lane = document->executionLane()->shared_from_this();
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future();
    auto blocker = lane->postToOwner([&] {
        entered.set_value();
        released.wait();
    });
    bool didRelease = false;
    const auto releaseWorker = [&] {
        if (!didRelease) {
            release.set_value();
            didRelease = true;
        }
    };
    // Even an assertion failure must unblock the owner before fixture teardown.
    const auto cleanup = qScopeGuard([&] {
        releaseWorker();
        if (blocker.valid()) {
            blocker.wait();
        }
        EXPECT_TRUE(processEventsUntil([&] {
            return lane->isIdle() && !document->collaborationService().closeAdmissionActive();
        }));
    });
    ASSERT_EQ(enteredFuture.wait_for(5s), std::future_status::ready);

    // The owner cannot start the write until after this call has thrown. Both
    // the helper's stack and these temporary options are gone when it resumes.
    EXPECT_THROW(
        Gui::Test::writeRecoverySnapshotAwaitingOwnerThread(*document, {}, 10ms),
        Base::RuntimeError);
    EXPECT_FALSE(lane->isIdle());
    EXPECT_FALSE(QFileInfo(archivePath()).isFile());
    EXPECT_FALSE(App::GetApplication().closeDocument(documentName.c_str()))
        << "the timed-out write must retain document lifetime until completion";
    EXPECT_EQ(App::GetApplication().getDocument(documentName.c_str()), document);

    releaseWorker();
    blocker.get();
    ASSERT_TRUE(processEventsUntil([&] {
        return lane->isIdle() && !document->collaborationService().closeAdmissionActive();
    }));
    EXPECT_TRUE(QFileInfo(archivePath()).isFile());
    EXPECT_TRUE(QFileInfo(metadataPath()).isFile());
}

TEST_F(AutoSaverRecoveryTest, WorkerAdmissionRaceWaitsWithoutReportingFailedWrite)
{
    using namespace std::chrono_literals;
    auto* lane = document->executionLane();
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future();
    auto blocker = lane->postToOwner([&] { entered.set_value(); released.wait(); });
    ASSERT_EQ(enteredFuture.wait_for(5s), std::future_status::ready);
    bool completed = false, written = false;
    App::DocumentInitFlags flags;
    flags.createView = false;
    auto* foreign = App::GetApplication().newDocument("workerAdmissionForeign", "foreign", flags);
    App::beginCollaborationReadOnlyMutationTarget(*foreign);
    Gui::scheduleRecoverySnapshotWrite(*document, {}, [&](bool result, std::exception_ptr failure) {
        completed = true;
        written = result;
        EXPECT_EQ(failure, nullptr);
    });
    release.set_value();
    blocker.wait();
    // Old code returns false from the worker while admission is held. A fair
    // reservation retains the write instead of declaring an IO failure.
    const auto deadline = std::chrono::steady_clock::now() + 100ms;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_FALSE(completed);
    App::endCollaborationReadOnlyMutationTarget(*foreign);
    ASSERT_TRUE(processEventsUntil([&] { return completed; }));
    EXPECT_TRUE(written);
    App::GetApplication().closeDocument("workerAdmissionForeign");
}

TEST_F(AutoSaverRecoveryTest, PendingRecoveryGetsFairAdmissionUnderSustainedContention)
{
    bool reported = false;
    auto connection = document->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome& outcome) {
            reported = outcome.intent == App::DocumentSaveIntent::Recovery && outcome.fileWritten;
        });
    const auto disconnect = qScopeGuard([&] { connection.disconnect(); });
    App::DocumentInitFlags flags;
    flags.createView = false;
    auto* foreign = App::GetApplication().newDocument("fairAdmissionForeign", "foreign", flags);
    const auto close = qScopeGuard([&] { App::GetApplication().closeDocument("fairAdmissionForeign"); });
    App::beginCollaborationReadOnlyMutationTarget(*foreign);
    Gui::AutoSaver::instance()->flushPendingSave(QString::fromStdString(documentName));
    App::endCollaborationReadOnlyMutationTarget(*foreign);
    bool saved = false;
    // No idle timer gap: repeated foreign commands must yield to queued recovery.
    for (int attempt = 0; attempt < 100 && !saved; ++attempt) {
        bool admitted = false;
        try {
            App::beginCollaborationReadOnlyMutationTarget(*foreign);
            admitted = true;
        }
        catch (const App::LivePresentationRefreshInProgress&) {}
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (admitted) {
            App::endCollaborationReadOnlyMutationTarget(*foreign);
        }
        saved = QFileInfo::exists(metadataPath()) && reported;
    }
    EXPECT_TRUE(saved) << "pending recovery was starved by repeated mutation admission";
    // Let the asynchronous completion retire before fixture teardown.
    ASSERT_TRUE(processEventsUntil([&] { return QFileInfo::exists(metadataPath()) && reported; }));
}

TEST_F(AutoSaverRecoveryTest, CompletionAdmissionRaceRetainsAttemptUntilReporting)
{
    const auto identity = document->collaborationIdentity();
    ASSERT_TRUE(Gui::Test::beginAutoSaveAttempt(*document));
    App::DocumentInitFlags flags;
    flags.createView = false;
    auto* foreign = App::GetApplication().newDocument("completionAdmissionForeign", "foreign", flags);
    App::beginCollaborationReadOnlyMutationTarget(*foreign);
    Gui::Test::deliverRecoveryCompletion(documentName, identity.instanceId, identity.lifecycleEpoch);
    EXPECT_FALSE(Gui::Test::beginAutoSaveAttempt(*document));
    App::endCollaborationReadOnlyMutationTarget(*foreign);
    // A queued completion must get a turn before another foreign command.
    bool admitted = false;
    try {
        App::beginCollaborationReadOnlyMutationTarget(*foreign);
        admitted = true;
    }
    catch (const App::LivePresentationRefreshInProgress&) {}
    EXPECT_FALSE(admitted);
    if (admitted) App::endCollaborationReadOnlyMutationTarget(*foreign);
    ASSERT_TRUE(processEventsUntil([&] { return Gui::Test::beginAutoSaveAttempt(*document); }));
    App::GetApplication().closeDocument("completionAdmissionForeign");
}

TEST_F(AutoSaverRecoveryTest, CompletionDoesNotBlockAlreadyActiveDocumentLane)
{
    using namespace std::chrono_literals;
    const auto identity = document->collaborationIdentity();
    ASSERT_TRUE(Gui::Test::beginAutoSaveAttempt(*document));
    auto* lane = document->executionLane();
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future();
    auto busy = lane->postToOwner([&] {
        lane->beginRecoverySnapshotOwnerWork();
        const auto finish = qScopeGuard([&] { lane->endRecoverySnapshotOwnerWork(); });
        entered.set_value();
        released.wait();
    });
    const auto finishBusy = qScopeGuard([&] { release.set_value(); busy.wait(); });
    ASSERT_EQ(enteredFuture.wait_for(5s), std::future_status::ready);
    Gui::Test::deliverRecoveryCompletion(documentName, identity.instanceId, identity.lifecycleEpoch);
    // A pending completion cannot hold process admission hostage to a busy
    // lane: that lane may need mutation admission itself in order to finish.
    bool admitted = false;
    try {
        App::beginCollaborationReadOnlyMutationTarget(*document);
        admitted = true;
    }
    catch (const App::LivePresentationRefreshInProgress&) {}
    EXPECT_TRUE(admitted);
    if (admitted) App::endCollaborationReadOnlyMutationTarget(*document);
}


TEST_F(AutoSaverRecoveryTest, RecoveryOwnerRejectsDuplicateDocumentWrite)
{
    using namespace std::chrono_literals;
    Gui::RecoveryWriteExecutor owner;
    auto lane = document->executionLane()->shared_from_this();
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future();
    auto blocker = lane->postToOwner([&] { entered.set_value(); released.wait(); });
    bool completed = false;
    const auto cleanup = qScopeGuard([&] {
        release.set_value(); blocker.wait();
        EXPECT_TRUE(processEventsUntil([&] { return completed; }));
    });
    ASSERT_EQ(enteredFuture.wait_for(5s), std::future_status::ready);
    EXPECT_TRUE(owner.schedule(*document, {}, [&](bool, std::exception_ptr) { completed = true; }));
    EXPECT_FALSE(owner.schedule(*document, {}, [](bool, std::exception_ptr) {}));
}

TEST_F(AutoSaverRecoveryTest, RecoveryOwnerClosePendingWriteDropsCompletion)
{
    using namespace std::chrono_literals;
    Gui::RecoveryWriteExecutor owner;
    auto lane = document->executionLane()->shared_from_this();
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future();
    auto blocker = lane->postToOwner([&] { entered.set_value(); released.wait(); });
    bool called = false;
    bool didRelease = false;
    const auto cleanup = qScopeGuard([&] {
        if (!didRelease) release.set_value();
        blocker.wait();
        EXPECT_TRUE(processEventsUntil([&] { return lane->isIdle(); }));
        owner.shutdown();
    });
    ASSERT_EQ(enteredFuture.wait_for(5s), std::future_status::ready);
    ASSERT_TRUE(owner.schedule(*document, {}, [&](bool, std::exception_ptr) { called = true; }));
    // Direct close must refuse while a queued write retains document access.
    EXPECT_FALSE(App::GetApplication().closeDocument(documentName.c_str()));
    owner.cancelDocument(*document);
    release.set_value(); didRelease = true; blocker.wait();
    ASSERT_TRUE(processEventsUntil([&] { return lane->isIdle(); }));
    ASSERT_TRUE(processEventsUntil([&] {
        return App::GetApplication().closeDocument(documentName.c_str());
    }));
    document = nullptr; guiDocument = nullptr; object = nullptr;
    QCoreApplication::processEvents();
    EXPECT_FALSE(called);
}

TEST_F(AutoSaverRecoveryTest, RecoveryOwnerShutdownIsBoundedAndDropsCallbacks)
{
    using namespace std::chrono_literals;
    auto owner = std::make_unique<Gui::RecoveryWriteExecutor>();
    auto lane = document->executionLane()->shared_from_this();
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future();
    auto blocker = lane->postToOwner([&] { entered.set_value(); released.wait(); });
    bool called = false;
    bool didRelease = false;
    const auto cleanup = qScopeGuard([&] {
        if (!didRelease) release.set_value();
        blocker.wait();
        EXPECT_TRUE(processEventsUntil([&] { return lane->isIdle(); }));
    });
    ASSERT_EQ(enteredFuture.wait_for(5s), std::future_status::ready);
    ASSERT_TRUE(owner->schedule(*document, {}, [&](bool, std::exception_ptr) { called = true; }));
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(owner->shutdown(20ms));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
    EXPECT_FALSE(owner->schedule(*document, {}, [](bool, std::exception_ptr) {}));
    owner.reset();
    // Unblock after destroying the owner, then close before servicing completion.
    release.set_value(); didRelease = true; blocker.wait();
    auto barrier = lane->postToOwner([] {});
    ASSERT_EQ(barrier.wait_for(5s), std::future_status::ready);
    ASSERT_TRUE(processEventsUntil([&] {
        return App::GetApplication().closeDocument(documentName.c_str());
    }));
    document = nullptr; guiDocument = nullptr; object = nullptr;
    QCoreApplication::processEvents();
    EXPECT_FALSE(called);
}

TEST_F(AutoSaverRecoveryTest, RecoveryOwnerHonoursGlobalDispatchCap)
{
    using namespace std::chrono_literals;
    Gui::RecoveryWriteExecutor owner(nullptr, 1);
    auto lane = document->executionLane()->shared_from_this();
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future();
    auto blocker = lane->postToOwner([&] { entered.set_value(); released.wait(); });
    bool releasedOnce = false;
    App::DocumentInitFlags flags; flags.createView = false;
    auto* second = App::GetApplication().newDocument("recoveryCapSecond", "second", flags);
    second->addObject("App::FeatureTest", "Target");
    Gui::Test::recomputeWithoutBlockingGui(*second);
    const auto secondArchive = QDir(QString::fromUtf8(second->TransientDir.getValue()))
        .filePath(QStringLiteral("fc_recovery_file.fcstd"));
    bool firstDone = false, secondDone = false;
    const auto cleanup = qScopeGuard([&] {
        if (!releasedOnce) release.set_value();
        blocker.wait();
        EXPECT_TRUE(processEventsUntil([&] { return firstDone && secondDone; }));
        App::GetApplication().closeDocument("recoveryCapSecond");
    });
    ASSERT_EQ(enteredFuture.wait_for(5s), std::future_status::ready);
    ASSERT_TRUE(owner.schedule(*document, {}, [&](bool, std::exception_ptr) { firstDone = true; }));
    ASSERT_TRUE(owner.schedule(*second, {}, [&](bool, std::exception_ptr) { secondDone = true; }));
    // The cap bounds lane dispatches, including writers waiting for admission.
    // With an unbounded worker per document this marker is stuck behind the
    // second recovery's acquire(), even though global IO is already serialized.
    const auto deadline = std::chrono::steady_clock::now() + 100ms;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(); std::this_thread::sleep_for(1ms);
    }
    auto marker = second->executionLane()->postToOwner([] {});
    EXPECT_EQ(marker.wait_for(100ms), std::future_status::ready);
    EXPECT_FALSE(QFileInfo::exists(secondArchive));
    EXPECT_FALSE(secondDone);
    release.set_value(); releasedOnce = true; blocker.wait();
    ASSERT_TRUE(processEventsUntil([&] { return firstDone && secondDone; }));
    EXPECT_TRUE(QFileInfo::exists(secondArchive));
}

TEST_F(AutoSaverRecoveryTest, RecoveryOwnerPreservesArchiveAndMetadata)
{
    Gui::RecoveryWriteExecutor owner;
    bool completed = false, written = false;
    std::exception_ptr error;
    ASSERT_TRUE(owner.schedule(*document, {}, [&](bool result, std::exception_ptr failure) {
        written = result; error = failure; completed = true;
    }));
    ASSERT_TRUE(processEventsUntil([&] { return completed; }));
    EXPECT_TRUE(written); EXPECT_EQ(error, nullptr);
    EXPECT_EQ(validateProjectArchive(archivePath()), ProjectValidationResult::Ok);
    QFile metadata(metadataPath()); ASSERT_TRUE(metadata.open(QIODevice::ReadOnly));
    const auto xml = metadata.readAll();
    EXPECT_TRUE(xml.contains("<AutoRecovery SchemaVersion=\"1\">"));
    EXPECT_TRUE(xml.contains("<CollaborationProvenance SchemaVersion=\"1\">"));
}

TEST_F(AutoSaverRecoveryTest, RecoveryOwnerShutdownWhileAdmissionBlockedRetainsDocument)
{
    using namespace std::chrono_literals;
    Gui::RecoveryWriteExecutor owner;
    auto lane = document->executionLane()->shared_from_this();
    App::DocumentInitFlags flags; flags.createView = false;
    auto* foreign = App::GetApplication().newDocument("recoveryShutdownForeign", "foreign", flags);
    bool admitted = false, called = false;
    const auto cleanup = qScopeGuard([&] {
        if (admitted) App::endCollaborationReadOnlyMutationTarget(*foreign);
        owner.shutdown();
        EXPECT_TRUE(processEventsUntil([&] { return lane->isIdle(); }));
        App::GetApplication().closeDocument("recoveryShutdownForeign");
    });
    App::beginCollaborationReadOnlyMutationTarget(*foreign); admitted = true;
    ASSERT_TRUE(owner.schedule(*document, {}, [&](bool, std::exception_ptr) { called = true; }));
    // This marker is queued behind the writer, which cannot acquire admission.
    auto barrier = lane->postToOwner([] {});
    ASSERT_EQ(barrier.wait_for(100ms), std::future_status::timeout);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(owner.shutdown(20ms));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
    EXPECT_FALSE(App::GetApplication().closeDocument(documentName.c_str()))
        << "the executing task must retain its own lifecycle pin after owner shutdown";
    App::endCollaborationReadOnlyMutationTarget(*foreign); admitted = false;
    ASSERT_EQ(barrier.wait_for(5s), std::future_status::ready);
    ASSERT_TRUE(processEventsUntil([&] {
        return App::GetApplication().closeDocument(documentName.c_str());
    }));
    document = nullptr; guiDocument = nullptr; object = nullptr;
    QCoreApplication::processEvents();
    EXPECT_FALSE(called);
}

TEST_F(AutoSaverRecoveryTest, RecoveryOwnerIngressCloseCancelsPendingWrite)
{
    using namespace std::chrono_literals;
    auto lane = document->executionLane()->shared_from_this();
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future();
    auto blocker = lane->postToOwner([&] { entered.set_value(); released.wait(); });
    bool called = false, didRelease = false;
    const auto cleanup = qScopeGuard([&] {
        if (!didRelease) release.set_value();
        blocker.wait();
        EXPECT_TRUE(processEventsUntil([&] { return lane->isIdle(); }));
    });
    ASSERT_EQ(enteredFuture.wait_for(5s), std::future_status::ready);
    Gui::scheduleRecoverySnapshotWrite(*document, {}, [&](bool, std::exception_ptr) { called = true; });
    ASSERT_TRUE(Gui::submitDocumentClose(*document, /*allowApplicationClose=*/false));
    release.set_value(); didRelease = true; blocker.wait();
    ASSERT_TRUE(processEventsUntil([&] {
        return !App::GetApplication().getDocument(documentName.c_str());
    }));
    document = nullptr; guiDocument = nullptr; object = nullptr;
    QCoreApplication::processEvents();
    EXPECT_FALSE(called);
}

TEST_F(AutoSaverRecoveryTest, RecoveryOwnerAsyncCloseBeforeCompletionPoll)
{
    using namespace std::chrono_literals;
    auto lane = document->executionLane()->shared_from_this();
    auto& owner = Gui::AutoSaver::instance()->recoveryWrites();
    const auto cancelOnFailure = qScopeGuard([&] {
        if (auto* doc = App::GetApplication().getDocument(documentName.c_str())) {
            owner.cancelDocument(*doc);
        }
    });
    bool called = false;
    ASSERT_TRUE(Gui::scheduleRecoverySnapshotWrite(*document, {},
        [&](bool, std::exception_ptr) { called = true; }));
    auto barrier = lane->postToOwner([] {});
    // Service serialization's marshalled GUI work, but never the executor's
    // Qt completion timer. The write retires while its GUI pin remains held.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (barrier.wait_for(0ms) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline) {
        App::MainThreadSignalConfig::serviceMarshalledTasks(2ms);
    }
    ASSERT_EQ(barrier.wait_for(0ms), std::future_status::ready);
    ASSERT_TRUE(lane->isIdle());
    ASSERT_TRUE(QFileInfo::exists(metadataPath()));
    ASSERT_TRUE(document->collaborationService().closeAdmissionActive());
    ASSERT_FALSE(called);
    App::DocumentCommandHandle closeHandle;
    {
        Base::PyGILStateLocker gil;
        Py::Object pythonDocument(document->getPyObject());
        Py::Object handle = pythonDocument.callMemberFunction("closeAsync");
        Py::Dict status(handle.callMemberFunction("status"));
        closeHandle = App::DocumentCommandHandle(
            Py::Long(status.getItem("id")).as_long(), document->executionHandle().identity());
    }
    const auto closeDeadline = std::chrono::steady_clock::now() + 5s;
    while (!closeHandle.status().terminal()
           && std::chrono::steady_clock::now() < closeDeadline) {
        App::MainThreadSignalConfig::serviceMarshalledTasks(2ms);
    }
    EXPECT_EQ(closeHandle.status().state, App::DocumentCommandState::Completed)
        << closeHandle.status().diagnostic;
    ASSERT_EQ(App::GetApplication().getDocument(documentName.c_str()), nullptr);
    document = nullptr; guiDocument = nullptr; object = nullptr;
    QCoreApplication::processEvents();
    EXPECT_FALSE(called);
}

TEST_F(AutoSaverRecoveryTest, RecoveryOwnerPythonCloseCancelsUndispatchedThirdWrite)
{
    using namespace std::chrono_literals;
    auto& owner = Gui::AutoSaver::instance()->recoveryWrites();
    App::DocumentInitFlags flags; flags.createView = false;
    std::vector<App::Document*> blockers;
    std::vector<std::future<void>> tasks;
    std::promise<void> release;
    auto released = release.get_future().share();
    const auto cleanup = qScopeGuard([&] {
        for (auto* doc : blockers) owner.cancelDocument(*doc);
        release.set_value();
        for (auto& task : tasks) task.wait();
        for (auto* doc : blockers) {
            auto lane = doc->executionLane()->shared_from_this();
            EXPECT_TRUE(processEventsUntil([&] { return lane->isIdle(); }));
            App::GetApplication().closeDocument(doc->getName());
        }
    });
    for (int i = 0; i < 2; ++i) {
        const auto name = App::GetApplication().getUniqueDocumentName("recoveryCloseCap");
        auto* doc = App::GetApplication().newDocument(name.c_str(), "cap blocker", flags);
        blockers.push_back(doc);
        doc->addObject("App::FeatureTest", "Target");
        Gui::Test::recomputeWithoutBlockingGui(*doc);
    }
    for (auto* doc : blockers) {
        std::promise<void> entered;
        auto ready = entered.get_future();
        tasks.push_back(doc->executionLane()->postToOwner([&entered, released] {
            entered.set_value();
            released.wait();
        }));
        ASSERT_EQ(ready.wait_for(5s), std::future_status::ready);
        ASSERT_TRUE(owner.schedule(*doc, {}, [](bool, std::exception_ptr) {}));
    }
    bool called = false;
    ASSERT_TRUE(owner.schedule(*document, {}, [&](bool, std::exception_ptr) { called = true; }));
    // Both dispatch slots are occupied. The third job has no lane task yet.
    auto marker = document->executionLane()->postToOwner([] {});
    ASSERT_EQ(marker.wait_for(5s), std::future_status::ready);

    bool rescued = false;
    std::promise<void> closed;
    auto closedFuture = closed.get_future();
    std::thread rescue([&] {
        if (closedFuture.wait_for(2s) == std::future_status::timeout) {
            // Bound a broken close without pumping Qt: use precisely the
            // marshalled work serviced by Python's synchronous admission loop.
            App::MainThreadSignalConfig::invoke([&] {
                if (auto* doc = App::GetApplication().getDocument(documentName.c_str())) {
                    rescued = true;
                    owner.cancelDocument(*doc);
                }
            }, false);
        }
    });
    {
        Base::PyGILStateLocker gil;
        Base::Interpreter().runString(
            ("import FreeCAD\nFreeCAD.closeDocument('" + documentName + "')").c_str());
    }
    closed.set_value();
    rescue.join();
    App::MainThreadSignalConfig::serviceMarshalledTasks(0ms);
    EXPECT_FALSE(rescued) << "Python close needed rescue: queued recovery never cancelled";
    EXPECT_EQ(App::GetApplication().getDocument(documentName.c_str()), nullptr);
    document = nullptr; guiDocument = nullptr; object = nullptr;
    QCoreApplication::processEvents();
    EXPECT_FALSE(called);
}

TEST_F(AutoSaverRecoveryTest, RejectedCloseDuringRecoveryKeepsAutosaveSchedulable)
{
    int writes = 0;
    bool completed = false;
    auto outcomeConnection = document->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome& outcome) {
            if (outcome.intent == App::DocumentSaveIntent::Recovery && outcome.fileWritten) {
                completed = true;
            }
        });
    auto connection = document->signalSaveDocument.connect([&](Base::Writer&) {
        if (++writes == 1) {
            // Executing IO owns its lifecycle pin and recovery flag. Close
            // must be refused, but dropping completion must not strand autosave.
            EXPECT_FALSE(Gui::submitDocumentClose(*document, /*allowApplicationClose=*/false));
        }
    });
    const auto disconnect = qScopeGuard([&] {
        connection.disconnect();
        outcomeConnection.disconnect();
    });
    Gui::AutoSaver::instance()->flushPendingSave(QString::fromStdString(documentName));
    ASSERT_TRUE(processEventsUntil([&] {
        return writes == 1 && document->executionLane()->isIdle()
            && QFileInfo::exists(metadataPath());
    }));
    ASSERT_EQ(App::GetApplication().getDocument(documentName.c_str()), document);
    EXPECT_FALSE(completed);
    // No new edit: the cancelled attempt's original dirty work must survive.
    removeRecoveryOutputs();
    EXPECT_TRUE(processEventsUntil([&] {
        // A later timer pass may precede executor retirement; keep retrying
        // as a real autosave timer would until the cancelled job is retired.
        if (writes == 1 && document->executionLane()->isIdle()) {
            Gui::AutoSaver::instance()->flushPendingSave(QString::fromStdString(documentName));
        }
        return completed && writes == 2 && document->executionLane()->isIdle()
            && QFileInfo::exists(metadataPath());
    }));
    EXPECT_EQ(writes, 2);
    EXPECT_EQ(validateProjectArchive(archivePath()), ProjectValidationResult::Ok);
    EXPECT_FALSE(Gui::Test::beginAutoSaveAttempt(*document));
}

TEST_F(AutoSaverRecoveryTest, RecoveryOwnerShutdownDuringSerializationDoesNotWaitForGuiCallback)
{
    using namespace std::chrono_literals;
    auto owner = std::make_unique<Gui::RecoveryWriteExecutor>();
    auto lane = document->executionLane()->shared_from_this();
    bool serializing = false, called = false;
    auto connection = document->signalSaveDocument.connect([&](Base::Writer&) {
        // The writer is now inside IO, waiting for this marshalled GUI signal.
        // Shutdown must return even though the writer cannot retire until this
        // callback returns; its own pin must still refuse recursive close.
        serializing = true;
        const auto start = std::chrono::steady_clock::now();
        EXPECT_FALSE(owner->shutdown(20ms));
        EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
        owner.reset();
        EXPECT_FALSE(App::GetApplication().closeDocument(documentName.c_str()));
    });
    const auto disconnect = qScopeGuard([&] { connection.disconnect(); });
    ASSERT_TRUE(owner->schedule(*document, {}, [&](bool, std::exception_ptr) { called = true; }));
    ASSERT_TRUE(processEventsUntil([&] {
        return serializing && lane->isIdle() && QFileInfo::exists(metadataPath());
    }));
    EXPECT_TRUE(QFileInfo::exists(archivePath()));
    QCoreApplication::processEvents();
    EXPECT_FALSE(called);
}
