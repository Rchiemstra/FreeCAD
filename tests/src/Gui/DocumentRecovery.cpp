// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <Gui/DocumentRecoveryInternal.h>

#include <zipios++/zipoutputstream.h>

#include <string>

#ifdef __linux__
# include <filesystem>
#endif

using Gui::Dialog::DocumentRecoveryInternal::ProjectValidationResult;
using Gui::Dialog::DocumentRecoveryInternal::checkXmlFiles;
using Gui::Dialog::DocumentRecoveryInternal::checkZipData;
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

QString writeFcstdLikeZip(const QTemporaryDir& dir, const QString& name,
                          int extraBinaryEntries, bool includeGuiDocument,
                          const char* documentXml, const char* guiXml = nullptr)
{
    const QString path = dir.filePath(name);
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
