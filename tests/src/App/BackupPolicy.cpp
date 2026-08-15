// SPDX-License-Identifier: LGPL-2.1-or-later

/****************************************************************************
 *   Copyright (c) 2025 The FreeCAD project association AISBL               *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "InitApplication.h"
#include <src/TempDirectory.h>

#include <App/BackupPolicy.h>

#include <filesystem>
#include <array>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L && defined(_LIBCPP_VERSION) \
    && _LIBCPP_VERSION >= 13000
// Apple's clang compiler did not support timezones fully until a quite recent version:
// before removing this preprocessor check, verify that it compiles on our oldest-supported
// macOS version.
# define CAN_USE_CHRONO_AND_FORMAT
# include <chrono>
# include <format>
#endif


class BackupPolicyTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void TearDown() override
    {
        App::Internal::setBackupPolicyBeforeInstallHookForTesting({});
        App::Internal::setBackupPolicyCheckpointHookForTesting({});
    }

    void apply(const std::string& sourcename, const std::string& targetname)
    {
        _policy.apply(sourcename, targetname);
    }

    App::BackupPolicy::PostReplacementResult
    applyAfterReplacement(const std::string& sourcename, const std::string& targetname)
    {
        return _policy.applyAfterReplacement(sourcename, targetname);
    }

    void setPolicyTerms(App::BackupPolicy::Policy p, int count, bool useExt, const std::string& fmt)
    {
        _policy.setPolicy(p);
        _policy.setNumberOfFiles(count);
        _policy.useBackupExtension(useExt);
        _policy.setDateFormat(fmt);
    }

    // Create a named temporary file: returns the full path to the new file. Deleted by the TearDown
    // method at the end of the test.
    std::filesystem::path createTempFile(const std::string& filename)
    {
        std::filesystem::path p = _tempDir.path() / filename;
        std::ofstream fileStream(p.string());
        fileStream << "Test data";
        fileStream.close();
        return p;
    }


protected:
    std::string filenameFromDateFormatString(const std::string& fmt)
    {
#if CAN_USE_CHRONO_AND_FORMAT
        std::chrono::zoned_time local_time {
            std::chrono::current_zone(),
            std::chrono::system_clock::now()
        };
        std::string fmt_str = "{:" + fmt + "}";
        std::string result = std::vformat(fmt_str, std::make_format_args(local_time));
#else
        std::time_t now = std::time(nullptr);
        std::tm local_tm {};
# if defined(_WIN32)
        localtime_s(&local_tm, &now);  // Windows
# else
        localtime_r(&now, &local_tm);  // POSIX
# endif
        constexpr size_t bufferLength = 128;
        std::array<char, bufferLength> buffer {};
        size_t bytes = std::strftime(buffer.data(), bufferLength, fmt.c_str(), &local_tm);
        if (bytes == 0) {
            throw std::runtime_error("failed to format time");
        }
        std::string result {buffer.data()};
#endif

        return result;
    }

    std::filesystem::path getTempPath()
    {
        return _tempDir.path();
    }

private:
    App::BackupPolicy _policy;
    tests::TempDirectory _tempDir {"fc_backup_policy"};
};

TEST_F(BackupPolicyTest, StandardSourceDoesNotExist)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 1, false, "%Y-%m-%d_%H-%M-%S");

    // Act & Assert
    EXPECT_THROW(apply("nonexistent.fcstd", "backup.fcstd"), Base::FileException);
}

TEST_F(BackupPolicyTest, LegacyApplyMissingDestinationParentKeepsBaseFileException)
{
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 1, false, "%Y-%m-%d_%H-%M-%S");
    const auto source = createTempFile("source.fcstd");
    const auto destination = getTempPath() / "missing-parent" / "document.FCStd";

    EXPECT_THROW(apply(source.string(), destination.string()), Base::FileException);
}

TEST_F(BackupPolicyTest, StandardAfterReplacementBacksUpDisplacedFileWithoutTouchingCanonical)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 1, false, "%Y-%m-%d_%H-%M-%S");
    const auto displaced = createTempFile("document.displaced");
    const auto canonical = createTempFile("document.FCStd");
    {
        std::ofstream stream(displaced, std::ios::out | std::ios::trunc);
        stream << "old bytes";
    }
#if !defined(_WIN32)
    std::error_code permissionError;
    constexpr auto expectedPermissions = std::filesystem::perms::owner_read
        | std::filesystem::perms::owner_write | std::filesystem::perms::group_read;
    std::filesystem::permissions(displaced,
                                 expectedPermissions,
                                 std::filesystem::perm_options::replace,
                                 permissionError);
    ASSERT_FALSE(permissionError) << permissionError.message();
#endif
    {
        std::ofstream stream(canonical, std::ios::out | std::ios::trunc);
        stream << "new bytes";
    }

    // Act
    const auto result = applyAfterReplacement(displaced.string(), canonical.string());

    // Assert
    EXPECT_TRUE(result.backupCreated);
    EXPECT_TRUE(result.displacedFileConsumed);
    EXPECT_TRUE(result.warnings.empty());
    EXPECT_FALSE(std::filesystem::exists(displaced));
    EXPECT_TRUE(std::filesystem::exists(canonical.string() + "1"));
    std::ifstream canonicalStream(canonical);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(canonicalStream),
                          std::istreambuf_iterator<char>()),
              "new bytes");
    std::ifstream backupStream(canonical.string() + "1");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(backupStream),
                          std::istreambuf_iterator<char>()),
              "old bytes");
#if !defined(_WIN32)
    EXPECT_EQ(std::filesystem::status(canonical.string() + "1").permissions()
                  & std::filesystem::perms::all,
              expectedPermissions);
#endif
}

TEST_F(BackupPolicyTest, AfterReplacementWithBackupsDisabledDeletesOnlyDisplacedFile)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 0, false, "%Y-%m-%d_%H-%M-%S");
    const auto displaced = createTempFile("document.displaced");
    const auto canonical = createTempFile("document.FCStd");

    // Act
    const auto result = applyAfterReplacement(displaced.string(), canonical.string());

    // Assert
    EXPECT_FALSE(result.backupCreated);
    EXPECT_TRUE(result.displacedFileConsumed);
    EXPECT_TRUE(result.warnings.empty());
    EXPECT_FALSE(std::filesystem::exists(displaced));
    EXPECT_TRUE(std::filesystem::exists(canonical));
}

TEST_F(BackupPolicyTest, TimestampAfterReplacementCreatesBackupWithoutMovingCanonical)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, true, "%Y-%m-%d_%H-%M-%S");
    const auto displaced = createTempFile("document.displaced");
    const auto canonical = createTempFile("document.FCStd");
    {
        std::ofstream stream(displaced, std::ios::out | std::ios::trunc);
        stream << "old bytes";
    }
    {
        std::ofstream stream(canonical, std::ios::out | std::ios::trunc);
        stream << "new bytes";
    }

    // Act
    const auto result = applyAfterReplacement(displaced.string(), canonical.string());

    // Assert
    EXPECT_TRUE(result.backupCreated);
    EXPECT_TRUE(result.displacedFileConsumed);
    EXPECT_TRUE(result.warnings.empty());
    std::ifstream canonicalStream(canonical);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(canonicalStream),
                          std::istreambuf_iterator<char>()),
              "new bytes");
    std::vector<std::filesystem::path> backups;
    for (const auto& entry : std::filesystem::directory_iterator(getTempPath())) {
        if (entry.path().extension() == ".FCBak") {
            backups.push_back(entry.path());
        }
    }
    ASSERT_EQ(backups.size(), 1);
    std::ifstream backupStream(backups.front());
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(backupStream),
                          std::istreambuf_iterator<char>()),
              "old bytes");
}

TEST_F(BackupPolicyTest, AfterReplacementReportsMissingSnapshotAsWarning)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, true, "%Y-%m-%d_%H-%M-%S");
    const auto canonical = createTempFile("document.FCStd");

    // Act
    const auto result = applyAfterReplacement(
        (getTempPath() / "missing.displaced").string(), canonical.string());

    // Assert
    EXPECT_FALSE(result.backupCreated);
    EXPECT_FALSE(result.displacedFileConsumed);
    ASSERT_EQ(result.warnings.size(), 1);
    EXPECT_TRUE(std::filesystem::exists(canonical));
}

TEST_F(BackupPolicyTest, AfterReplacementRejectsCanonicalSourceForEveryPolicy)
{
    for (const auto policy : {App::BackupPolicy::Standard, App::BackupPolicy::TimeStamp}) {
        setPolicyTerms(policy, 0, true, "%Y-%m-%d_%H-%M-%S");
        const auto canonical = createTempFile(
            policy == App::BackupPolicy::Standard ? "standard.FCStd" : "timestamp.FCStd");
        {
            std::ofstream stream(canonical, std::ios::out | std::ios::trunc);
            stream << "canonical bytes";
        }

        const auto result = applyAfterReplacement(canonical.string(), canonical.string());

        EXPECT_FALSE(result.backupCreated);
        EXPECT_FALSE(result.displacedFileConsumed);
        EXPECT_FALSE(result.warnings.empty());
        EXPECT_TRUE(std::filesystem::exists(canonical));
        std::ifstream stream(canonical);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>()),
                  "canonical bytes");
    }
}

TEST_F(BackupPolicyTest, AfterReplacementRejectsNormalizedAndHardlinkAliasesForEveryPolicy)
{
    for (const auto policy : {App::BackupPolicy::Standard, App::BackupPolicy::TimeStamp}) {
        setPolicyTerms(policy, 1, true, "%Y-%m-%d_%H-%M-%S");
        const std::string stem =
            policy == App::BackupPolicy::Standard ? "standard-alias" : "timestamp-alias";
        const auto canonical = createTempFile(stem + ".FCStd");
        const auto normalizedAlias = canonical.parent_path() / "." / canonical.filename();
        auto result = applyAfterReplacement(normalizedAlias.string(), canonical.string());
        EXPECT_FALSE(result.backupCreated);
        EXPECT_FALSE(result.displacedFileConsumed);
        EXPECT_FALSE(result.warnings.empty());
        EXPECT_TRUE(std::filesystem::exists(canonical));

        const auto hardlink = getTempPath() / (stem + ".hardlink");
        std::error_code error;
        std::filesystem::create_hard_link(canonical, hardlink, error);
        if (error) {
            GTEST_SKIP() << "Hard links are unavailable: " << error.message();
        }
        result = applyAfterReplacement(hardlink.string(), canonical.string());
        EXPECT_FALSE(result.backupCreated);
        EXPECT_FALSE(result.displacedFileConsumed);
        EXPECT_FALSE(result.warnings.empty());
        EXPECT_TRUE(std::filesystem::exists(canonical));
        EXPECT_TRUE(std::filesystem::exists(hardlink));
    }
}

TEST_F(BackupPolicyTest, StandardLateCandidateCollisionIsRetriedWithoutClobber)
{
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 2, false, "%Y-%m-%d_%H-%M-%S");
    const auto displaced = createTempFile("document.displaced");
    const auto canonical = createTempFile("document.FCStd");
    {
        std::ofstream stream(displaced, std::ios::out | std::ios::trunc);
        stream << "old bytes";
    }
    std::string collision;
    bool injected = false;
    App::Internal::setBackupPolicyBeforeInstallHookForTesting(
        [&](const std::string& candidate) {
            if (!injected) {
                injected = true;
                collision = candidate;
                std::ofstream stream(candidate, std::ios::out | std::ios::trunc);
                stream << "foreign candidate";
            }
        });

    const auto result = applyAfterReplacement(displaced.string(), canonical.string());

    ASSERT_TRUE(injected);
    ASSERT_TRUE(result.backupCreated);
    EXPECT_TRUE(result.displacedFileConsumed);
    EXPECT_TRUE(result.warnings.empty());
    std::ifstream collisionStream(collision);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(collisionStream),
                          std::istreambuf_iterator<char>()),
              "foreign candidate");
    EXPECT_TRUE(std::filesystem::exists(canonical.string() + "2"));
    std::ifstream installedStream(canonical.string() + "2");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(installedStream),
                          std::istreambuf_iterator<char>()),
              "old bytes");
}

TEST_F(BackupPolicyTest, InstallFailurePreservesHistoryAndDisplacedSnapshot)
{
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 1, false, "%Y-%m-%d_%H-%M-%S");
    const auto displaced = createTempFile("document.displaced");
    const auto canonical = createTempFile("document.FCStd");
    const auto oldBackup = createTempFile("document.FCStd1");
    App::Internal::setBackupPolicyBeforeInstallHookForTesting(
        [](const std::string&) { throw std::runtime_error("injected install failure"); });

    const auto result = applyAfterReplacement(displaced.string(), canonical.string());

    EXPECT_FALSE(result.backupCreated);
    EXPECT_FALSE(result.displacedFileConsumed);
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_TRUE(std::filesystem::exists(displaced));
    EXPECT_TRUE(std::filesystem::exists(oldBackup));
    EXPECT_TRUE(std::filesystem::exists(canonical));
}

TEST_F(BackupPolicyTest, ProtectedLateCollisionIsNeverPruned)
{
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 1, false, "%Y-%m-%d_%H-%M-%S");
    const auto displaced = createTempFile("document.displaced");
    const auto canonical = createTempFile("document.FCStd");
    const auto oldBackup = createTempFile("document.FCStd1");
    bool injected = false;
    App::Internal::setBackupPolicyBeforeInstallHookForTesting(
        [&](const std::string& candidate) {
            if (!injected) {
                injected = true;
                std::ofstream stream(candidate, std::ios::out | std::ios::trunc);
                stream << "foreign collision";
            }
        });

    const auto result = applyAfterReplacement(displaced.string(), canonical.string());

    ASSERT_TRUE(injected);
    EXPECT_TRUE(result.backupCreated);
    EXPECT_TRUE(result.displacedFileConsumed);
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_FALSE(std::filesystem::exists(oldBackup));
    EXPECT_TRUE(std::filesystem::exists(canonical.string() + "2"));
    EXPECT_TRUE(std::filesystem::exists(canonical.string() + "3"));
    std::ifstream collision(canonical.string() + "2");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(collision),
                          std::istreambuf_iterator<char>()),
              "foreign collision");
}

#if !defined(_WIN32)
TEST_F(BackupPolicyTest, PreConsumptionDurabilityFailureRetainsSourceAndHistory)
{
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 1, false, "%Y-%m-%d_%H-%M-%S");
    const auto displaced = createTempFile("document.displaced");
    const auto canonical = createTempFile("document.FCStd");
    const auto oldBackup = createTempFile("document.FCStd1");
    App::Internal::setBackupPolicyCheckpointHookForTesting(
        [](const App::Internal::BackupPolicyTestCheckpoint checkpoint,
           const std::string&,
           const std::string&) {
            if (checkpoint
                == App::Internal::BackupPolicyTestCheckpoint::
                    AfterDirectoryFlushBeforeSourceUnlink) {
                throw std::runtime_error("injected pre-consumption durability failure");
            }
        });

    const auto result = applyAfterReplacement(displaced.string(), canonical.string());

    EXPECT_TRUE(result.backupCreated);
    EXPECT_FALSE(result.displacedFileConsumed);
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_TRUE(std::filesystem::exists(displaced));
    EXPECT_TRUE(std::filesystem::exists(oldBackup));
    EXPECT_TRUE(std::filesystem::exists(canonical.string() + "2"));
}

TEST_F(BackupPolicyTest, PostConsumptionDurabilityFailureDoesNotPruneHistory)
{
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 1, false, "%Y-%m-%d_%H-%M-%S");
    const auto displaced = createTempFile("document.displaced");
    const auto canonical = createTempFile("document.FCStd");
    const auto oldBackup = createTempFile("document.FCStd1");
    App::Internal::setBackupPolicyCheckpointHookForTesting(
        [](const App::Internal::BackupPolicyTestCheckpoint checkpoint,
           const std::string&,
           const std::string&) {
            if (checkpoint
                == App::Internal::BackupPolicyTestCheckpoint::
                    AfterSourceUnlinkBeforeDirectoryFlush) {
                throw std::runtime_error("injected post-consumption durability failure");
            }
        });

    const auto result = applyAfterReplacement(displaced.string(), canonical.string());

    EXPECT_TRUE(result.backupCreated);
    EXPECT_TRUE(result.displacedFileConsumed);
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_FALSE(std::filesystem::exists(displaced));
    EXPECT_TRUE(std::filesystem::exists(oldBackup));
    EXPECT_TRUE(std::filesystem::exists(canonical.string() + "2"));
}
#endif

TEST_F(BackupPolicyTest, ConcurrentRotationsUseDistinctAtomicCandidates)
{
    const auto firstDisplaced = createTempFile("first.displaced");
    const auto secondDisplaced = createTempFile("second.displaced");
    const auto canonical = createTempFile("document.FCStd");
    {
        std::ofstream stream(firstDisplaced, std::ios::out | std::ios::trunc);
        stream << "first old bytes";
    }
    {
        std::ofstream stream(secondDisplaced, std::ios::out | std::ios::trunc);
        stream << "second old bytes";
    }

    App::BackupPolicy firstPolicy;
    firstPolicy.setPolicy(App::BackupPolicy::Standard);
    firstPolicy.setNumberOfFiles(3);
    App::BackupPolicy secondPolicy;
    secondPolicy.setPolicy(App::BackupPolicy::Standard);
    secondPolicy.setNumberOfFiles(3);
    App::BackupPolicy::PostReplacementResult firstResult;
    App::BackupPolicy::PostReplacementResult secondResult;
    std::mutex startMutex;
    std::condition_variable startCondition;
    int ready = 0;
    bool start = false;
    const auto waitForStart = [&] {
        std::unique_lock lock(startMutex);
        ++ready;
        startCondition.notify_all();
        startCondition.wait(lock, [&] { return start; });
    };
    std::thread first([&] {
        waitForStart();
        firstResult = firstPolicy.applyAfterReplacement(firstDisplaced.string(),
                                                        canonical.string());
    });
    std::thread second([&] {
        waitForStart();
        secondResult = secondPolicy.applyAfterReplacement(secondDisplaced.string(),
                                                          canonical.string());
    });
    {
        std::unique_lock lock(startMutex);
        startCondition.wait(lock, [&] { return ready == 2; });
        start = true;
    }
    startCondition.notify_all();
    first.join();
    second.join();

    EXPECT_TRUE(firstResult.backupCreated);
    EXPECT_TRUE(firstResult.displacedFileConsumed);
    EXPECT_TRUE(secondResult.backupCreated);
    EXPECT_TRUE(secondResult.displacedFileConsumed);
    EXPECT_TRUE(std::filesystem::exists(canonical.string() + "1"));
    EXPECT_TRUE(std::filesystem::exists(canonical.string() + "2"));
}

TEST_F(BackupPolicyTest, StandardWithZeroFilesDeletesExisting)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 0, false, "%Y-%m-%d_%H-%M-%S");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");

    // Act
    apply(source.string(), target.string());

    // Assert
    GTEST_SKIP();  // Can't test on a real filesystem, too much caching for reliable results
    EXPECT_FALSE(std::filesystem::exists(target));
}

TEST_F(BackupPolicyTest, StandardWithOneFileNoPreviousBackups)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 1, false, "%Y-%m-%d_%H-%M-%S");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");

    // Act
    apply(source.string(), target.string());

    // Assert
    EXPECT_TRUE(std::filesystem::exists(target.string() + "1"));
}

TEST_F(BackupPolicyTest, StandardWithOneFileOnePreviousBackup)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 1, false, "%Y-%m-%d_%H-%M-%S");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");
    auto backup = createTempFile("target.fcstd1");

    // Act
    apply(source.string(), target.string());

    // Assert
    EXPECT_TRUE(std::filesystem::exists(backup));
    EXPECT_FALSE(std::filesystem::exists(target.string() + "2"));
}

TEST_F(BackupPolicyTest, StandardWithTwoFilesOnePreviousBackup)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 2, false, "%Y-%m-%d_%H-%M-%S");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");
    auto backup = createTempFile("target.fcstd1");

    // Act
    apply(source.string(), target.string());

    // Assert
    EXPECT_TRUE(std::filesystem::exists(backup));
    EXPECT_TRUE(std::filesystem::exists(target.string() + "2"));
}

TEST_F(BackupPolicyTest, StandardWithTwoFilesOnePreviousBackupUnexpectedSuffix)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 2, false, "%Y-%m-%d_%H-%M-%S");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");
    auto backup = createTempFile("target.fcstd1");
    auto weird = createTempFile("target.fcstd2a");

    // Act
    apply(source.string(), target.string());

    // Assert
    EXPECT_TRUE(std::filesystem::exists(backup));
    EXPECT_TRUE(std::filesystem::exists(target.string() + "2"));
    EXPECT_TRUE(std::filesystem::exists(weird));
}

TEST_F(BackupPolicyTest, StandardWithTwoFilesOnePreviousBackupOutOfSequenceNumber)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 2, false, "%Y-%m-%d_%H-%M-%S");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");
    auto backup = createTempFile("target.fcstd1");
    auto weird = createTempFile("target.fcstd999");

    // Act
    apply(source.string(), target.string());

    // Assert
    EXPECT_TRUE(std::filesystem::exists(backup));
    bool check1 = std::filesystem::exists(target.string() + "2");
    bool check2 = std::filesystem::exists(weird);
    EXPECT_NE(check1, check2);  // Only one or the other can exist (we don't know which because it
                                // depends on file modification date)
}

TEST_F(BackupPolicyTest, StandardWithFCBakSet)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::Standard, 1, true, "%Y-%m-%d_%H-%M-%S");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");

    // Act
    apply(source.string(), target.string());

    // Assert
    EXPECT_TRUE(std::filesystem::exists(target.string() + "1"));  // No FCBak extension for Standard
}

TEST_F(BackupPolicyTest, TimestampSourceDoesNotExist)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, false, "%Y-%m-%d_%H-%M-%S");

    // Act & Assert
    EXPECT_THROW(apply("nonexistent.fcstd", "backup.fcstd"), Base::FileException);
}

TEST_F(BackupPolicyTest, TimestampNoSourceGiven)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, false, "%Y-%m-%d_%H-%M-%S");
    auto target = createTempFile("target.fcstd");

    // Act & Assert
    EXPECT_THROW(apply("nonexistent.fcstd", target.string()), Base::FileException);
}

TEST_F(BackupPolicyTest, TimestampNoTargetGiven)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, false, "%Y-%m-%d_%H-%M-%S");
    auto source = createTempFile("source.fcstd");

    // Act & Assert
    EXPECT_THROW(apply(source.string(), ""), Base::FileException);
}

TEST_F(BackupPolicyTest, TimestampWithZeroFilesDeletesExisting)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 0, false, "%Y-%m-%d_%H-%M-%S");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");

    // Act
    apply(source.string(), target.string());

    // Assert
    GTEST_SKIP();  // Can't test on a real filesystem, too much caching for reliable results
    EXPECT_FALSE(std::filesystem::exists(target));
}

TEST_F(BackupPolicyTest, TimestampWithOneFileAndNoneExistingNotFCBakCreatesNumberedFile)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, false, "%Y-%m-%d");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");

    // Act
    apply(source.string(), target.string());

    // Assert
    // Without the .FCBak extension, the date stuff is completely ignored, even if the policy is set
    // to "Timestamp"
    EXPECT_TRUE(std::filesystem::exists(target.string() + "1"));
}

TEST_F(BackupPolicyTest, TimestampSourceHasNoExtension)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, true, "%Y-%m-%d");
    auto source = createTempFile("source");
    auto target = createTempFile("target.fcstd");

    // Act
    apply(source.string(), target.string());

    // Assert
    auto expected = "target." + filenameFromDateFormatString("%Y-%m-%d") + ".FCBak";
    EXPECT_TRUE(std::filesystem::exists(getTempPath() / expected));
}

TEST_F(BackupPolicyTest, TimestampTargetHasNoExtension)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, true, "%Y-%m-%d");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target");

    // Act
    apply(source.string(), target.string());

    // Assert
    auto expected = "target." + filenameFromDateFormatString("%Y-%m-%d") + ".FCBak";
    EXPECT_TRUE(std::filesystem::exists(getTempPath() / expected));
}

TEST_F(BackupPolicyTest, TimestampWithOneFileAndNoneExistingFCBakCreatesDatedFile)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, true, "%Y-%m-%d");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");

    // Act
    apply(source.string(), target.string());

    // Assert
    auto expected = "target." + filenameFromDateFormatString("%Y-%m-%d") + ".FCBak";
    EXPECT_TRUE(std::filesystem::exists(getTempPath() / expected));
}

TEST_F(BackupPolicyTest, TimestampReplacesDotsWithDashes)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, true, "%Y.%m.%d");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");

    // Act
    apply(source.string(), target.string());

    // Assert
    auto expected = "target." + filenameFromDateFormatString("%Y-%m-%d") + ".FCBak";
    EXPECT_TRUE(std::filesystem::exists(getTempPath() / expected));
}

TEST_F(BackupPolicyTest, DISABLED_TimestampWithInvalidFormatStringThrows)
{
    // THIS TEST IS DISABLED BECAUSE THE CURRENT CODE DOES NOT CORRECTLY HANDLE INVALID FORMAT
    // OPERATIONS, AND GENERATES UNEXPECTED FILENAMES WHEN GIVEN ONE. FIXME.

    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, true, "%Q-%W-%E");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");

    // Act and Assert
    EXPECT_THROW(apply(source.string(), target.string()), Base::FileException);
}

TEST_F(BackupPolicyTest, DISABLED_TimestampWithAbsurdlyLongFormatStringThrows)
{
    // THIS TEST IS DISABLED BECAUSE THE CURRENT CODE DOES NOT CORRECTLY HANDLE OVER-LENGTH FORMAT
    // OPERATIONS, AND GENERATES AN INVALID FILENAME. FIXME.

    // Arrange
    setPolicyTerms(
        App::BackupPolicy::Policy::TimeStamp,
        1,
        true,
        "%A, %B %d, %Y at %H:%M:%S %Z (Day %j of the year, Week %U/%W) — This is a "
        "verbose date string for demonstration purposes."
    );
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");

    // Act and Assert
    EXPECT_THROW(apply(source.string(), target.string()), Base::FileException);
}

TEST_F(BackupPolicyTest, TimestampDetectsOldBackupFormat)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, true, "%Y-%m-%d");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");
    auto backup = createTempFile("target.fcstd12345");

    // Act
    apply(source.string(), target.string());

    // Assert
    bool check1 = std::filesystem::exists(
        getTempPath() / ("target." + filenameFromDateFormatString("%Y-%m-%d") + ".FCBak")
    );
    bool check2 = std::filesystem::exists(backup);
    EXPECT_NE(check1, check2);
}

TEST_F(BackupPolicyTest, TimestampDetectsOldBackupFormatIgnoresOther)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, true, "%Y-%m-%d");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");
    auto backup = createTempFile("target.fcstd12345");
    auto weird = createTempFile("target.fcstd12345abc");

    // Act
    apply(source.string(), target.string());

    // Assert
    bool check1 = std::filesystem::exists(
        getTempPath() / ("target." + filenameFromDateFormatString("%Y-%m-%d") + ".FCBak")
    );
    bool check2 = std::filesystem::exists(backup);
    EXPECT_NE(check1, check2);
    EXPECT_TRUE(std::filesystem::exists(weird));
}

TEST_F(BackupPolicyTest, TimestampDetectsAndRetainsOldBackupWhenAllowed)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 2, true, "%Y-%m-%d");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");
    auto backup = createTempFile("target.fcstd12345");

    // Act
    apply(source.string(), target.string());

    // Assert
    EXPECT_TRUE(
        std::filesystem::exists(
            getTempPath() / ("target." + filenameFromDateFormatString("%Y-%m-%d") + ".FCBak")
        )
    );
    EXPECT_TRUE(std::filesystem::exists(backup));
}

TEST_F(BackupPolicyTest, TimestampFormatStringEndsWithSpace)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, true, "%Y-%m-%d ");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");

    // Act
    apply(source.string(), target.string());

    // Assert (the space is stripped, and an index is added)
    EXPECT_TRUE(
        std::filesystem::exists(
            getTempPath() / ("target." + filenameFromDateFormatString("%Y-%m-%d") + "1.FCBak")
        )
    );
}

TEST_F(BackupPolicyTest, TimestampFormatStringEndsWithDash)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 1, true, "%Y-%m-%d-");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");

    // Act
    apply(source.string(), target.string());

    // Assert (the dash is left, and an index is added)
    EXPECT_TRUE(
        std::filesystem::exists(
            getTempPath() / ("target." + filenameFromDateFormatString("%Y-%m-%d") + "-1.FCBak")
        )
    );
}

TEST_F(BackupPolicyTest, TimestampFormatFileAlreadyExists)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 2, true, "%Y-%m-%d");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");
    auto backup = createTempFile("target." + filenameFromDateFormatString("%Y-%m-%d") + ".FCBak");

    // Act
    apply(source.string(), target.string());

    // Assert (An index is appended)
    EXPECT_TRUE(std::filesystem::exists(backup));
    EXPECT_TRUE(
        std::filesystem::exists(
            getTempPath() / ("target." + filenameFromDateFormatString("%Y-%m-%d") + "-1.FCBak")
        )
    );
}

TEST_F(BackupPolicyTest, TimestampFormatFileAlreadyExistsMultipleTimes)
{
    // Arrange
    setPolicyTerms(App::BackupPolicy::Policy::TimeStamp, 5, true, "%Y-%m-%d");
    auto source = createTempFile("source.fcstd");
    auto target = createTempFile("target.fcstd");
    auto backup = createTempFile("target." + filenameFromDateFormatString("%Y-%m-%d") + ".FCBak");
    auto backup1 = createTempFile("target." + filenameFromDateFormatString("%Y-%m-%d") + "-1.FCBak");
    auto backup2 = createTempFile("target." + filenameFromDateFormatString("%Y-%m-%d") + "-2.FCBak");
    auto backup3 = createTempFile("target." + filenameFromDateFormatString("%Y-%m-%d") + "-3.FCBak");

    // Act
    apply(source.string(), target.string());

    // Assert (An index is appended)
    EXPECT_TRUE(std::filesystem::exists(backup));
    EXPECT_TRUE(std::filesystem::exists(backup1));
    EXPECT_TRUE(std::filesystem::exists(backup2));
    EXPECT_TRUE(std::filesystem::exists(backup3));
    EXPECT_TRUE(
        std::filesystem::exists(
            getTempPath() / ("target." + filenameFromDateFormatString("%Y-%m-%d") + "-4.FCBak")
        )
    );
}
