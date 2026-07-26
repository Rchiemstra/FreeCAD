// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/GeometryRequestWorkspace.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstring>

class GeometryRequestWorkspaceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(_tempDir->isValid());
    }

    std::unique_ptr<QTemporaryDir> _tempDir;
};

TEST_F(GeometryRequestWorkspaceTest, StaleRequestRemovedOnReuse)
{
    const QString ws = _tempDir->path() + QStringLiteral("/reuse");
    QDir().mkpath(ws);
    {
        QFile stale(ws + QStringLiteral("/request.json"));
        ASSERT_TRUE(stale.open(QIODevice::WriteOnly | QIODevice::Truncate));
        stale.write("{\"stale\":true}");
        stale.close();
    }

    App::GeometryRequestWorkspace workspace(ws);
    EXPECT_FALSE(workspace.hasFailed());
    workspace.writeBytes("payload.bin", nullptr, 0);
    ASSERT_TRUE(workspace.publishRequestJson());
    EXPECT_TRUE(QFileInfo::exists(ws + QStringLiteral("/request.json")));
    QFile published(ws + QStringLiteral("/request.json"));
    ASSERT_TRUE(published.open(QIODevice::ReadOnly));
    const QByteArray body = published.readAll();
    EXPECT_FALSE(body.contains("stale"));
    EXPECT_TRUE(body.contains("FCGEO/1"));
}

TEST_F(GeometryRequestWorkspaceTest, RequestDirectoryBlocksPublish)
{
    const QString ws = _tempDir->path() + QStringLiteral("/blocked");
    QDir().mkpath(ws);
    QDir().mkpath(ws + QStringLiteral("/request.json"));

    App::GeometryRequestWorkspace workspace(ws);
    EXPECT_TRUE(workspace.hasFailed());
    EXPECT_EQ(workspace.failureCode(), "WorkspaceReplaceFailed");
    workspace.writeBytes("payload.bin", nullptr, 0);
    EXPECT_FALSE(workspace.publishRequestJson());
    EXPECT_TRUE(QFileInfo::exists(ws + QStringLiteral("/request.json")));
    EXPECT_TRUE(QFileInfo(ws + QStringLiteral("/request.json")).isDir());
}

TEST_F(GeometryRequestWorkspaceTest, StageSmallFileByteIdentical)
{
    const QString ws = _tempDir->path() + QStringLiteral("/stage");
    const QString srcPath = _tempDir->path() + QStringLiteral("/source.bin");
    {
        QFile src(srcPath);
        ASSERT_TRUE(src.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray payload("hello-staged-archive", 20);
        ASSERT_EQ(src.write(payload), payload.size());
        src.close();
    }

    App::GeometryRequestWorkspace workspace(ws);
    ASSERT_TRUE(workspace.stageFileAtomic(QStringLiteral("base.fcg"), srcPath));
    QFile staged(ws + QStringLiteral("/base.fcg"));
    ASSERT_TRUE(staged.open(QIODevice::ReadOnly));
    EXPECT_EQ(staged.readAll(), QByteArray("hello-staged-archive", 20));
}

#if defined(Q_OS_UNIX)
TEST_F(GeometryRequestWorkspaceTest, OversizedSparseSourceRejectedWithoutPublish)
{
    const QString ws = _tempDir->path() + QStringLiteral("/oversize");
    const QString srcPath = _tempDir->path() + QStringLiteral("/sparse.bin");
    const qint64 tooLarge =
        static_cast<qint64>(App::GeometryRequestWorkspace::maxWorkspaceSectionBytes()) + 1;
    {
        QFile src(srcPath);
        ASSERT_TRUE(src.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_TRUE(src.seek(tooLarge - 1));
        ASSERT_EQ(src.write("x", 1), 1);
        src.close();
    }

    App::GeometryRequestWorkspace workspace(ws);
    EXPECT_FALSE(workspace.stageFileAtomic(QStringLiteral("base.fcg"), srcPath));
    EXPECT_TRUE(workspace.hasFailed());
    EXPECT_EQ(workspace.failureCode(), "SectionTooLarge");
    EXPECT_FALSE(QFileInfo::exists(ws + QStringLiteral("/base.fcg")));
    EXPECT_FALSE(QFileInfo::exists(ws + QStringLiteral("/request.json")));
}
#endif

TEST_F(GeometryRequestWorkspaceTest, OperandDirectoryBlocksStaging)
{
    const QString ws = _tempDir->path() + QStringLiteral("/operand_dir");
    QDir().mkpath(ws);
    QDir().mkpath(ws + QStringLiteral("/base.fcg"));

    const QString srcPath = _tempDir->path() + QStringLiteral("/tiny.bin");
    {
        QFile src(srcPath);
        ASSERT_TRUE(src.open(QIODevice::WriteOnly | QIODevice::Truncate));
        src.write("x");
        src.close();
    }

    App::GeometryRequestWorkspace workspace(ws);
    EXPECT_FALSE(workspace.hasFailed());
    EXPECT_FALSE(workspace.stageFileAtomic(QStringLiteral("base.fcg"), srcPath));
    EXPECT_EQ(workspace.failureCode(), "WorkspaceReplaceFailed");
    EXPECT_TRUE(QFileInfo(ws + QStringLiteral("/base.fcg")).isDir());
}
