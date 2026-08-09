// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Gui/Application.h>
#include <Gui/AutoSaver.h>
#include <Gui/Document.h>
#include <Gui/DocumentRecovery.h>
#include <Gui/DocumentRecoveryInternal.h>
#include <Gui/MainWindow.h>
#include <src/App/InitApplication.h>

#include <zipios++/zipoutputstream.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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
    for (int pass = 0; pass < 20; ++pass) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        if (predicate()) {
            return true;
        }
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
        document->recompute();
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
        Gui::AutoSaver::instance()->flushPendingSave(QString::fromStdString(documentName));
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
    source->recompute();
    ASSERT_TRUE(source->saveAs(projectPath.toUtf8().constData()));

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
    document->recompute();
    ASSERT_TRUE(document->canWriteRecoverySnapshot());
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    EXPECT_FALSE(QFileInfo::exists(archivePath()));
    EXPECT_FALSE(QFileInfo::exists(metadataPath()));

    flush();
    EXPECT_TRUE(QFileInfo(archivePath()).isFile());
    EXPECT_TRUE(QFileInfo(metadataPath()).isFile());
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
    document->recompute();
    ASSERT_TRUE(document->canWriteRecoverySnapshot());
    EXPECT_TRUE(processEventsUntil([this] { return QFileInfo(archivePath()).isFile(); }));
    EXPECT_TRUE(QFileInfo(metadataPath()).isFile());
}

TEST_F(AutoSaverRecoveryTest, PendingRecomputeDefersAndRetriesAfterRecompute)
{
    object->touch();
    ASSERT_TRUE(document->mustExecute());

    flush();
    EXPECT_FALSE(QFileInfo::exists(archivePath()));
    EXPECT_FALSE(QFileInfo::exists(metadataPath()));

    document->recompute();
    ASSERT_TRUE(document->canWriteRecoverySnapshot());
    EXPECT_TRUE(processEventsUntil([this] { return QFileInfo(archivePath()).isFile(); }));
    EXPECT_TRUE(QFileInfo(metadataPath()).isFile());
}

TEST_F(AutoSaverRecoveryTest, QueuedRetryCannotAutosaveReplacementWithReusedName)
{
    document->openTransaction("queue identity-bound autosave retry");
    object->Label.setValue("old document pending retry");
    flush();
    ASSERT_FALSE(QFileInfo::exists(archivePath()));
    document->commitTransaction();
    document->recompute();

    ASSERT_TRUE(App::GetApplication().closeDocument(documentName.c_str()));

    App::DocumentInitFlags flags;
    flags.createView = false;
    document = App::GetApplication().newDocument(
        documentName.c_str(), "replacement with reused name", flags);
    ASSERT_NE(document, nullptr);
    guiDocument = Gui::Application::Instance->getDocument(document);
    ASSERT_NE(guiDocument, nullptr);
    object = document->addObject("App::FeatureTest", "ReplacementTarget");
    ASSERT_NE(object, nullptr);
    document->recompute();
    removeRecoveryOutputs();
    object->Label.setValue("replacement ordinary dirty work");

    QCoreApplication::sendPostedEvents(nullptr, 0);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_FALSE(QFileInfo::exists(archivePath()));
    EXPECT_FALSE(QFileInfo::exists(metadataPath()));
}

TEST_F(AutoSaverRecoveryTest, GuiTransactionDefersUntilGroupedUndoFinishes)
{
    document->setMaxUndoStackSize(8);
    document->openTransaction("gui transaction source");
    object->Label.setValue("undo me");
    document->commitTransaction();
    document->recompute();
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
    connection.disconnect();

    ASSERT_TRUE(flushedInsideGuiTransaction);
    EXPECT_FALSE(archiveExistedInsideGuiTransaction);
    document->recompute();
    ASSERT_TRUE(document->canWriteRecoverySnapshot());
    EXPECT_TRUE(processEventsUntil([this] { return QFileInfo(archivePath()).isFile(); }));
    EXPECT_TRUE(QFileInfo(metadataPath()).isFile());
}

TEST_F(AutoSaverRecoveryTest, FailedWriteRetainsDirtyWorkForNextRetry)
{
    object->Label.setValue("must survive failed recovery write");
    document->recompute();
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
