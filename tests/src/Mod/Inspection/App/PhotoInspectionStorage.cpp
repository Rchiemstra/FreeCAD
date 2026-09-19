// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <Mod/Inspection/App/PhotoInspectionStorage.h>

namespace
{

using namespace Inspection::Photo;

std::string utf8(const QString& value)
{
    return value.toUtf8().toStdString();
}

TEST(PhotoInspectionStorageTest, writesOnlyInsideExplicitRoot)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString target = directory.filePath(QStringLiteral("report.json"));
    AtomicWriteOptions options;
    options.allowedRoot = utf8(directory.path());
    const auto result
        = writePhotoInspectionFileAtomically(utf8(target), "{\"status\":\"Complete\"}", options);
    ASSERT_TRUE(result.valid) << result.diagnostic.message;
    QFile file(target);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), QByteArray("{\"status\":\"Complete\"}"));
}

TEST(PhotoInspectionStorageTest, collisionDoesNotReplaceWithoutConsent)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString target = directory.filePath(QStringLiteral("report.json"));
    QFile existing(target);
    ASSERT_TRUE(existing.open(QIODevice::WriteOnly));
    ASSERT_EQ(existing.write("old"), 3);
    existing.close();

    AtomicWriteOptions options;
    options.allowedRoot = utf8(directory.path());
    const auto result = writePhotoInspectionFileAtomically(utf8(target), "new", options);
    EXPECT_FALSE(result.valid);
    ASSERT_TRUE(existing.open(QIODevice::ReadOnly));
    EXPECT_EQ(existing.readAll(), QByteArray("old"));
}

TEST(PhotoInspectionStorageTest, explicitReplacementIsComplete)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString target = directory.filePath(QStringLiteral("report.csv"));
    QFile existing(target);
    ASSERT_TRUE(existing.open(QIODevice::WriteOnly));
    ASSERT_EQ(existing.write("old"), 3);
    existing.close();

    AtomicWriteOptions options;
    options.allowedRoot = utf8(directory.path());
    options.replaceExisting = true;
    ASSERT_TRUE(writePhotoInspectionFileAtomically(utf8(target), "new", options).valid);
    ASSERT_TRUE(existing.open(QIODevice::ReadOnly));
    EXPECT_EQ(existing.readAll(), QByteArray("new"));
}

TEST(PhotoInspectionStorageTest, traversalAndRelativeTargetsReject)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    AtomicWriteOptions options;
    options.allowedRoot = utf8(directory.path());
    EXPECT_FALSE(writePhotoInspectionFileAtomically("relative.json", "x", options).valid);
    const QString escaped = QDir(directory.path()).absoluteFilePath(QStringLiteral("../escape.json"));
    EXPECT_FALSE(writePhotoInspectionFileAtomically(utf8(escaped), "x", options).valid);
}

TEST(PhotoInspectionStorageTest, byteLimitRejectsBeforeCreatingFile)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString target = directory.filePath(QStringLiteral("large.json"));
    AtomicWriteOptions options;
    options.allowedRoot = utf8(directory.path());
    options.maximumBytes = 3;
    const auto result = writePhotoInspectionFileAtomically(utf8(target), "four", options);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::ResourceLimit);
    EXPECT_FALSE(QFile::exists(target));
}

}  // namespace
