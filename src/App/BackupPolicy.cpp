// SPDX-License-Identifier: LGPL-2.1-or-later

/****************************************************************************
 *   Copyright (c) 2020 Werner Mayer <wmayer[at]users.sourceforge.net>      *
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

#include <boost/algorithm/string/replace.hpp>
#include <boost/regex.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <Base/TimeInfo.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/Interpreter.h>
#include <Base/Tools.h>
#include <Base/Writer.h>

#include "BackupPolicy.h"
#include "DocumentFileWriter.h"

#include <FCConfig.h>

#ifdef FC_OS_WIN32
# include <windows.h>
#else
# include <cerrno>
# include <fcntl.h>
# include <sys/stat.h>
# include <unistd.h>
#endif

using namespace App;

namespace
{

namespace fs = std::filesystem;

struct AtomicInstallResult
{
    bool installed {false};
    bool sourceConsumed {false};
    bool durabilityVerified {false};
    bool destinationExists {false};
    std::string error;
};

enum class BackupInstallCheckpoint
{
    AfterLinkBeforeDirectoryFlush,
    AfterDirectoryFlushBeforeSourceUnlink,
    AfterSourceUnlinkBeforeDirectoryFlush,
};

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
std::mutex backupHookMutex;
App::Internal::BackupPolicyBeforeInstallHook backupBeforeInstallHook;
App::Internal::BackupPolicyCheckpointHook backupCheckpointHook;

void invokeBackupBeforeInstallHook(const std::string& candidate)
{
    App::Internal::BackupPolicyBeforeInstallHook hook;
    {
        const std::scoped_lock guard(backupHookMutex);
        hook = backupBeforeInstallHook;
    }
    if (hook) {
        hook(candidate);
    }
}

void invokeBackupCheckpointHook(const App::Internal::BackupPolicyTestCheckpoint checkpoint,
                                const std::string& source,
                                const std::string& destination)
{
    App::Internal::BackupPolicyCheckpointHook hook;
    {
        const std::scoped_lock guard(backupHookMutex);
        hook = backupCheckpointHook;
    }
    if (hook) {
        hook(checkpoint, source, destination);
    }
}

bool hasBackupCheckpointHook()
{
    const std::scoped_lock guard(backupHookMutex);
    return static_cast<bool>(backupCheckpointHook);
}
#endif

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API) && !defined(FC_OS_WIN32)
std::string currentExceptionMessage()
{
    try {
        throw;
    }
    catch (const std::exception& exception) {
        return exception.what();
    }
    catch (...) {
        return "unknown injected failure";
    }
}
#endif

#ifndef FC_OS_WIN32
class ScopedDescriptor
{
public:
    explicit ScopedDescriptor(const int value = -1)
        : value(value)
    {}

    ~ScopedDescriptor()
    {
        if (value >= 0) {
            ::close(value);
        }
    }

    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept
    {
        return value;
    }

private:
    int value {-1};
};

int openReadOnly(const fs::path& path)
{
    int flags = O_RDONLY;
# ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
# endif
# ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
# endif
    return ::open(path.c_str(), flags);
}

int openDirectory(const fs::path& path)
{
    int flags = O_RDONLY;
# ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
# endif
# ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
# endif
    return ::open(path.c_str(), flags);
}

bool sameIdentity(const struct stat& left, const struct stat& right) noexcept
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}
#endif

AtomicInstallResult atomicInstallNoReplace(const std::string& source,
                                           const std::string& destination,
                                           const bool invokeTestHook)
{
    const auto sourcePath = Base::FileInfo::stringToPath(source);
    const auto destinationPath = Base::FileInfo::stringToPath(destination);
#ifdef FC_OS_WIN32
# if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
    if (invokeTestHook) {
        invokeBackupBeforeInstallHook(destination);
    }
# else
    (void)invokeTestHook;
# endif
    if (MoveFileExW(sourcePath.c_str(), destinationPath.c_str(), MOVEFILE_WRITE_THROUGH) != 0) {
        return {.installed = true,
                .sourceConsumed = true,
                .durabilityVerified = true};
    }
    const DWORD error = GetLastError();
    return {.destinationExists = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS,
            .error = std::system_category().message(static_cast<int>(error))};
#else
    ScopedDescriptor sourceDescriptor(openReadOnly(sourcePath));
    if (sourceDescriptor.get() < 0) {
        const int error = errno;
        return {.error = std::generic_category().message(error)};
    }
    struct stat pinnedSource {};
    if (::fstat(sourceDescriptor.get(), &pinnedSource) != 0) {
        const int error = errno;
        return {.error = std::generic_category().message(error)};
    }
    if (!S_ISREG(pinnedSource.st_mode)) {
        return {.error = std::generic_category().message(EINVAL)};
    }
    if (::fsync(sourceDescriptor.get()) != 0) {
        const int error = errno;
        return {.error = "Unable to flush the displaced snapshot before backup install: "
                + std::generic_category().message(error)};
    }

    ScopedDescriptor destinationParent(openDirectory(destinationPath.parent_path()));
    if (destinationParent.get() < 0) {
        const int error = errno;
        return {.error = std::generic_category().message(error)};
    }
    ScopedDescriptor sourceParent(openDirectory(sourcePath.parent_path()));
    if (sourceParent.get() < 0) {
        const int error = errno;
        return {.error = std::generic_category().message(error)};
    }

    struct stat sourceAtBoundary {};
    if (::lstat(sourcePath.c_str(), &sourceAtBoundary) != 0
        || !sameIdentity(pinnedSource, sourceAtBoundary)) {
        return {.error = "The displaced snapshot path changed before backup install"};
    }
# if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
    if (invokeTestHook) {
        // This is intentionally the last operation before the no-replace OS
        // primitive so tests exercise EEXIST rather than a preflight branch.
        invokeBackupBeforeInstallHook(destination);
    }
# else
    (void)invokeTestHook;
# endif
    if (::link(sourcePath.c_str(), destinationPath.c_str()) != 0) {
        const int error = errno;
        return {.destinationExists = error == EEXIST,
                .error = std::generic_category().message(error)};
    }

    AtomicInstallResult result;
    result.installed = true;
    struct stat installedSnapshot {};
    const bool installedInspected = ::lstat(destinationPath.c_str(), &installedSnapshot) == 0;
    if (!installedInspected || !sameIdentity(pinnedSource, installedSnapshot)) {
        // Never adopt or remove a path whose identity is not the pinned
        // displaced snapshot; retain the source as recovery evidence.
        result.installed = false;
        result.error = "The installed backup does not identify the displaced snapshot";
        return result;
    }
    ScopedDescriptor installedDescriptor(openReadOnly(destinationPath));
    struct stat pinnedInstalled {};
    if (installedDescriptor.get() < 0
        || ::fstat(installedDescriptor.get(), &pinnedInstalled) != 0
        || !sameIdentity(pinnedSource, pinnedInstalled)) {
        result.installed = false;
        result.error = "The installed backup could not be pinned to the displaced snapshot";
        return result;
    }

    const auto checkpointFailure = [&](const BackupInstallCheckpoint checkpoint)
        -> std::string {
# if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        try {
            App::Internal::BackupPolicyTestCheckpoint exposedCheckpoint =
                App::Internal::BackupPolicyTestCheckpoint::AfterLinkBeforeDirectoryFlush;
            switch (checkpoint) {
                case BackupInstallCheckpoint::AfterLinkBeforeDirectoryFlush:
                    break;
                case BackupInstallCheckpoint::AfterDirectoryFlushBeforeSourceUnlink:
                    exposedCheckpoint = App::Internal::BackupPolicyTestCheckpoint::
                        AfterDirectoryFlushBeforeSourceUnlink;
                    break;
                case BackupInstallCheckpoint::AfterSourceUnlinkBeforeDirectoryFlush:
                    exposedCheckpoint = App::Internal::BackupPolicyTestCheckpoint::
                        AfterSourceUnlinkBeforeDirectoryFlush;
                    break;
            }
            invokeBackupCheckpointHook(exposedCheckpoint, source, destination);
        }
        catch (...) {
            return currentExceptionMessage();
        }
# else
        (void)checkpoint;
# endif
        return {};
    };

    if (auto error = checkpointFailure(BackupInstallCheckpoint::AfterLinkBeforeDirectoryFlush);
        !error.empty()) {
        result.error = std::move(error);
        return result;
    }
    if (::fsync(destinationParent.get()) != 0) {
        const int error = errno;
        result.error = "Unable to make the installed backup name durable: "
            + std::generic_category().message(error);
        return result;
    }
    if (auto error = checkpointFailure(
            BackupInstallCheckpoint::AfterDirectoryFlushBeforeSourceUnlink);
        !error.empty()) {
        result.error = std::move(error);
        return result;
    }

    struct stat sourceBeforeUnlink {};
    struct stat destinationBeforeUnlink {};
    if (::lstat(sourcePath.c_str(), &sourceBeforeUnlink) != 0
        || !sameIdentity(pinnedSource, sourceBeforeUnlink)
        || ::lstat(destinationPath.c_str(), &destinationBeforeUnlink) != 0
        || !sameIdentity(pinnedInstalled, destinationBeforeUnlink)) {
        result.installed = false;
        result.error = "The displaced snapshot or installed backup path changed before cleanup";
        return result;
    }
    if (::unlink(sourcePath.c_str()) != 0) {
        const int error = errno;
        result.durabilityVerified = true;
        result.error = std::generic_category().message(error);
        return result;
    }
    result.sourceConsumed = true;
    if (auto error = checkpointFailure(
            BackupInstallCheckpoint::AfterSourceUnlinkBeforeDirectoryFlush);
        !error.empty()) {
        result.error = std::move(error);
        return result;
    }
    if (::fsync(sourceParent.get()) != 0) {
        const int error = errno;
        result.error = "Unable to make displaced snapshot cleanup durable: "
            + std::generic_category().message(error);
        return result;
    }
    result.durabilityVerified = true;
    return result;
#endif
}

AtomicInstallResult installDisplacedNoReplace(const std::string& source,
                                              const std::string& destination,
                                              const std::shared_ptr<void>& lease)
{
    if (!lease) {
        // Compatibility-only path implementation for callers that predate
        // DocumentFileWriter's retained displaced-file lease.
        return atomicInstallNoReplace(source, destination, true);
    }
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
    const auto checkpoint = [&](const App::Internal::DisplacedFileLeaseCheckpoint point) {
        switch (point) {
            case App::Internal::DisplacedFileLeaseCheckpoint::BeforeNamespaceInstall:
                invokeBackupBeforeInstallHook(destination);
                break;
            case App::Internal::DisplacedFileLeaseCheckpoint::AfterLinkBeforeDirectoryFlush:
                invokeBackupCheckpointHook(
                    App::Internal::BackupPolicyTestCheckpoint::AfterLinkBeforeDirectoryFlush,
                    source,
                    destination);
                break;
            // The retained-lease path has no source-unlink checkpoints: its
            // portable link fallback deliberately never removes the displaced
            // pathname. Only the non-lease compatibility path still reports
            // the two unlink checkpoints.
        }
    };
    const auto installed = App::Internal::installDisplacedFileLeaseNoReplace(
        lease, source, destination, checkpoint, hasBackupCheckpointHook());
#else
    const auto installed =
        App::Internal::installDisplacedFileLeaseNoReplace(lease, source, destination);
#endif
    return {.installed = installed.installed,
            .sourceConsumed = installed.sourceConsumed,
            .durabilityVerified = installed.durabilityVerified,
            .destinationExists = installed.destinationExists,
            .error = installed.error};
}

AtomicInstallResult discardDisplaced(const std::string& source,
                                     const std::shared_ptr<void>& lease)
{
    if (!lease) {
        Base::FileInfo sourceInfo(source);
        const bool consumed = sourceInfo.deleteFile();
        return {.sourceConsumed = consumed,
                .durabilityVerified = consumed,
                .error = consumed ? std::string {} : "Unable to remove the displaced file"};
    }
    const auto discarded = App::Internal::discardDisplacedFileLease(lease, source);
    return {.sourceConsumed = discarded.sourceConsumed,
            .durabilityVerified = discarded.durabilityVerified,
            .error = discarded.error};
}

fs::path normalizePathForComparison(const std::string& value)
{
    std::error_code error;
    auto absolute = fs::absolute(Base::FileInfo::stringToPath(value), error);
    if (error) {
        return Base::FileInfo::stringToPath(value).lexically_normal();
    }
    const auto filename = absolute.filename();
    auto parent = fs::weakly_canonical(absolute.parent_path(), error);
    if (error) {
        return absolute.lexically_normal();
    }
    return (parent / filename).lexically_normal();
}

bool platformPathsEqual(fs::path left, fs::path right)
{
#ifdef FC_OS_WIN32
    auto leftNative = left.native();
    auto rightNative = right.native();
    std::ranges::transform(leftNative, leftNative.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    std::ranges::transform(rightNative, rightNative.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    return leftNative == rightNative;
#else
    return left == right;
#endif
}

bool pathsAlias(const std::string& source, const std::string& target)
{
    const auto sourcePath = Base::FileInfo::stringToPath(source);
    const auto targetPath = Base::FileInfo::stringToPath(target);
    if (platformPathsEqual(normalizePathForComparison(source),
                           normalizePathForComparison(target))) {
        return true;
    }
    std::error_code error;
    const bool equivalent = fs::equivalent(sourcePath, targetPath, error);
    return !error && equivalent;
}

void appendPostReplacementWarning(BackupPolicy::PostReplacementResult& result,
                                  std::string warning)
{
    Base::Console().warning("%s\n", warning.c_str());
    result.warnings.push_back(std::move(warning));
}

void pruneInstalledHistory(BackupPolicy::PostReplacementResult& result,
                           std::vector<Base::FileInfo> backups,
                           const std::string& installed,
                           const std::vector<std::string>& protectedCandidates,
                           const int retainedCount)
{
    int removals = std::max<int>(0, static_cast<int>(backups.size()) - retainedCount);
    if (removals == 0) {
        return;
    }
    std::sort(backups.begin(), backups.end(), [](const auto& left, const auto& right) {
        return left.lastModified() < right.lastModified();
    });
    for (const Base::FileInfo& backup : backups) {
        if (removals == 0) {
            break;
        }
        // Never prune the backup that was just installed, even when its
        // preserved source timestamp makes it appear to be the oldest file.
        if (backup.filePath() == installed
            || std::ranges::find(protectedCandidates, backup.filePath())
                != protectedCandidates.end()) {
            continue;
        }
        if (backup.deleteFile()) {
            --removals;
        }
        else {
            appendPostReplacementWarning(result,
                                         "Unable to remove old backup file: "
                                             + backup.fileName());
        }
    }
    if (removals > 0) {
        appendPostReplacementWarning(
            result,
            "Backup history exceeds its configured limit because late collision entries were "
            "left untouched");
    }
}

}  // namespace

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
void App::Internal::setBackupPolicyBeforeInstallHookForTesting(
    BackupPolicyBeforeInstallHook hook)
{
    const std::scoped_lock guard(backupHookMutex);
    backupBeforeInstallHook = std::move(hook);
}

void App::Internal::setBackupPolicyCheckpointHookForTesting(BackupPolicyCheckpointHook hook)
{
    const std::scoped_lock guard(backupHookMutex);
    backupCheckpointHook = std::move(hook);
}
#endif

void BackupPolicy::setPolicy(const Policy p)
{
    policy = p;
}
void BackupPolicy::setNumberOfFiles(const int count)
{
    numberOfFiles = count;
}
void BackupPolicy::useBackupExtension(const bool on)
{
    useFCBakExtension = on;
}
void BackupPolicy::setDateFormat(const std::string& fmt)
{
    saveBackupDateFormat = fmt;
}
void BackupPolicy::apply(const std::string& sourcename, const std::string& targetname)
{
    // Keep legacy validation/exception types for an empty path. Valid save
    // paths share the exact same in-process and OS advisory lock as the new
    // replacement primitive and post-replacement history rotation.
    std::unique_ptr<Internal::DocumentFileLock> destinationLock;
    if (!targetname.empty() && Base::FileInfo(sourcename).exists()) {
        try {
            destinationLock = std::make_unique<Internal::DocumentFileLock>(targetname, -1);
            if (!destinationLock->isLocked()) {
                throw std::runtime_error("destination lock unavailable");
            }
        }
        catch (const std::exception&) {
            throw Base::FileException("Cannot lock project file for backup management",
                                      Base::FileInfo(targetname));
        }
    }
    switch (policy) {
        case Standard:
            applyStandard(sourcename, targetname);
            break;
        case TimeStamp:
            applyTimeStamp(sourcename, targetname);
            break;
    }
}

BackupPolicy::PostReplacementResult
BackupPolicy::applyAfterReplacement(const std::string& sourcename,
                                    const std::string& targetname)
{
    PostReplacementResult result;
    try {
        if (targetname.empty()) {
            appendPostReplacementWarning(result, "The canonical backup name is empty");
            return result;
        }
        if (!sourcename.empty() && pathsAlias(sourcename, targetname)) {
            appendPostReplacementWarning(
                result,
                "The displaced snapshot aliases the canonical file; backup management was "
                "refused");
            return result;
        }
        Internal::DocumentFileLock destinationLock(targetname, -1);
        if (!destinationLock.isLocked()) {
            appendPostReplacementWarning(
                result,
                "Unable to lock the canonical file while managing backup history");
            return result;
        }
        switch (policy) {
            case Standard:
                return applyStandardAfterReplacement(sourcename, targetname, {});
            case TimeStamp:
                return applyTimeStampAfterReplacement(sourcename, targetname, {});
        }
        appendPostReplacementWarning(result, "Unknown backup policy");
    }
    catch (const std::exception& exception) {
        appendPostReplacementWarning(result,
                                     "Unable to manage the previous file after replacement: "
                                         + std::string(exception.what()));
    }
    catch (...) {
        appendPostReplacementWarning(
            result,
            "Unable to manage the previous file after replacement: unknown error");
    }
    return result;
}

BackupPolicy::PostReplacementResult
BackupPolicy::applyAfterReplacement(const std::string& sourcename,
                                    const std::string& targetname,
                                    const std::shared_ptr<void>& displacedFileLease)
{
    PostReplacementResult result;
    try {
        if (!displacedFileLease) {
            appendPostReplacementWarning(
                result,
                "Authoritative backup management requires the retained displaced-file lease");
            return result;
        }
        if (targetname.empty()) {
            appendPostReplacementWarning(result, "The canonical backup name is empty");
            return result;
        }
        if (sourcename.empty() || pathsAlias(sourcename, targetname)) {
            appendPostReplacementWarning(
                result,
                "The displaced snapshot aliases the canonical file; backup management was "
                "refused");
            return result;
        }
        Internal::DocumentFileLock destinationLock(targetname, -1);
        if (!destinationLock.isLocked()) {
            appendPostReplacementWarning(
                result,
                "Unable to lock the canonical file while managing backup history");
            return result;
        }
        switch (policy) {
            case Standard:
                return applyStandardAfterReplacement(
                    sourcename, targetname, displacedFileLease);
            case TimeStamp:
                return applyTimeStampAfterReplacement(
                    sourcename, targetname, displacedFileLease);
        }
        appendPostReplacementWarning(result, "Unknown backup policy");
    }
    catch (const std::exception& exception) {
        appendPostReplacementWarning(result,
                                     "Unable to manage the previous file after replacement: "
                                         + std::string(exception.what()));
    }
    catch (...) {
        appendPostReplacementWarning(
            result,
            "Unable to manage the previous file after replacement: unknown error");
    }
    return result;
}

BackupPolicy::PostReplacementResult
BackupPolicy::applyStandardAfterReplacement(const std::string& sourcename,
                                            const std::string& targetname,
                                            const std::shared_ptr<void>& lease)
{
    PostReplacementResult result;
    Base::FileInfo source(sourcename);
    if (sourcename.empty() || (!lease && (!source.exists() || !source.isFile()))) {
        appendPostReplacementWarning(result,
                                     "The displaced previous file is unavailable for backup");
        return result;
    }
    if (numberOfFiles <= 0) {
        const auto discarded = discardDisplaced(sourcename, lease);
        result.displacedFileConsumed = discarded.sourceConsumed;
        if (!result.displacedFileConsumed) {
            appendPostReplacementWarning(result,
                                         "Unable to remove the unneeded displaced previous file: "
                                             + discarded.error);
        }
        else if (!discarded.durabilityVerified) {
            appendPostReplacementWarning(
                result,
                "The displaced previous file was removed, but cleanup durability is unverified: "
                    + discarded.error);
        }
        return result;
    }

    Base::FileInfo target(targetname);
    const std::string canonicalName = target.fileName();
    Base::FileInfo directory(target.dirPath());
    int highestSuffix = 0;
    for (const Base::FileInfo& entry : directory.getDirectoryContent()) {
        const std::string file = entry.fileName();
        if (!entry.isFile() || file.substr(0, canonicalName.length()) != canonicalName) {
            continue;
        }
        const std::string suffix = file.substr(canonicalName.length());
        if (!suffix.empty() && suffix.find_first_not_of("0123456789") == std::string::npos) {
            highestSuffix = std::max(highestSuffix, std::atoi(suffix.c_str()));
        }
    }

    int suffix = highestSuffix + 1;
    std::string installed;
    bool installationDurable = false;
    std::vector<std::string> protectedCandidates;
    for (int attempt = 0; attempt < numberOfFiles + 128; ++attempt) {
        const std::string candidate = target.filePath() + std::to_string(suffix++);
        const bool existedBeforeAttempt = Base::FileInfo(candidate).exists();
        const auto install = installDisplacedNoReplace(sourcename, candidate, lease);
        if (install.installed) {
            result.backupCreated = true;
            result.displacedFileConsumed = install.sourceConsumed;
            installationDurable = install.durabilityVerified && install.sourceConsumed;
            installed = candidate;
            if (!install.durabilityVerified) {
                appendPostReplacementWarning(
                    result,
                    "The backup name was installed, but transfer durability is unverified; "
                    "the displaced snapshot was retained when possible: "
                        + install.error);
            }
            else if (!install.sourceConsumed) {
                appendPostReplacementWarning(
                    result,
                    "The backup was installed, but the displaced snapshot name remains: "
                        + install.error);
            }
            break;
        }
        if (install.destinationExists && !existedBeforeAttempt) {
            protectedCandidates.push_back(candidate);
        }
        if (!install.destinationExists) {
            result.displacedFileConsumed = install.sourceConsumed;
            appendPostReplacementWarning(
                result,
                "Unable to install the displaced previous file in backup history: "
                    + install.error);
            return result;
        }
    }
    if (!result.backupCreated) {
        appendPostReplacementWarning(
            result,
            "Unable to reserve a collision-free standard backup history entry");
        return result;
    }
    if (!installationDurable) {
        // Never prune known-good history until the newly installed snapshot
        // and its source-name transition are durably accounted for.
        return result;
    }

    try {
        std::vector<Base::FileInfo> backups;
        for (const Base::FileInfo& entry : directory.getDirectoryContent()) {
            const std::string file = entry.fileName();
            if (!entry.isFile() || file.substr(0, canonicalName.length()) != canonicalName) {
                continue;
            }
            const std::string suffixText = file.substr(canonicalName.length());
            if (!suffixText.empty()
                && suffixText.find_first_not_of("0123456789") == std::string::npos) {
                backups.push_back(entry);
            }
        }
        pruneInstalledHistory(result,
                              std::move(backups),
                              installed,
                              protectedCandidates,
                              numberOfFiles);
    }
    catch (const std::exception& exception) {
        appendPostReplacementWarning(
            result,
            "The new backup is installed, but old history could not be pruned: "
                + std::string(exception.what()));
    }
    return result;
}

BackupPolicy::PostReplacementResult
BackupPolicy::applyTimeStampAfterReplacement(const std::string& sourcename,
                                             const std::string& targetname,
                                             const std::shared_ptr<void>& lease)
{
    PostReplacementResult result;
    Base::FileInfo source(sourcename);
    if (sourcename.empty() || (!lease && (!source.exists() || !source.isFile()))) {
        appendPostReplacementWarning(result,
                                     "The displaced previous file is unavailable for backup");
        return result;
    }
    if (numberOfFiles <= 0) {
        const auto discarded = discardDisplaced(sourcename, lease);
        result.displacedFileConsumed = discarded.sourceConsumed;
        if (!result.displacedFileConsumed) {
            appendPostReplacementWarning(result,
                                         "Unable to remove the unneeded displaced previous file: "
                                             + discarded.error);
        }
        else if (!discarded.durabilityVerified) {
            appendPostReplacementWarning(
                result,
                "The displaced previous file was removed, but cleanup durability is unverified: "
                    + discarded.error);
        }
        return result;
    }

    Base::FileInfo target(targetname);
    const std::string extension = target.extension();
    std::string backupBase;
    std::string projectBase;
    if (!extension.empty()) {
        backupBase = target.filePath().substr(0, target.filePath().length() - extension.length());
        projectBase = target.fileName().substr(0, target.fileName().length() - extension.length());
    }
    else {
        backupBase = target.filePath() + ".";
        projectBase = target.fileName() + ".";
    }

    std::string installed;
    bool installationDurable = false;
    std::vector<std::string> protectedCandidates;
    if (!useFCBakExtension) {
        for (int suffix = 1; suffix < numberOfFiles + 128; ++suffix) {
            const std::string candidate = target.filePath() + std::to_string(suffix);
            const bool existedBeforeAttempt = Base::FileInfo(candidate).exists();
            const auto install = installDisplacedNoReplace(sourcename, candidate, lease);
            if (install.installed) {
                result.backupCreated = true;
                result.displacedFileConsumed = install.sourceConsumed;
                installationDurable = install.durabilityVerified && install.sourceConsumed;
                installed = candidate;
                if (!install.durabilityVerified) {
                    appendPostReplacementWarning(
                        result,
                        "The backup name was installed, but transfer durability is unverified; "
                        "the displaced snapshot was retained when possible: "
                            + install.error);
                }
                else if (!install.sourceConsumed) {
                    appendPostReplacementWarning(
                        result,
                        "The backup was installed, but the displaced snapshot name remains: "
                            + install.error);
                }
                break;
            }
            if (install.destinationExists && !existedBeforeAttempt) {
                protectedCandidates.push_back(candidate);
            }
            if (!install.destinationExists) {
                result.displacedFileConsumed = install.sourceConsumed;
                appendPostReplacementWarning(
                    result,
                    "Unable to install the displaced previous file in backup history: "
                        + install.error);
                return result;
            }
        }
    }
    else {
        std::string dateFormat = saveBackupDateFormat;
        boost::replace_all(dateFormat, ".", "-");
        Base::TimeInfo modified = source.lastModified();
        const std::time_t modifiedTime = modified.getTime_t();
        std::tm localTime {};
#if defined(_WIN32)
        localtime_s(&localTime, &modifiedTime);
#else
        localtime_r(&modifiedTime, &localTime);
#endif
        constexpr std::size_t bufferLength = 128;
        std::array<char, bufferLength> buffer {};
        if (std::strftime(buffer.data(), bufferLength, dateFormat.c_str(), &localTime) == 0) {
            appendPostReplacementWarning(result,
                                         "The backup date format was invalid; using a safe format");
            constexpr std::string_view fallback {"%Y-%m-%d_%H-%M-%S"};
            std::strftime(buffer.data(), bufferLength, fallback.data(), &localTime);
        }
        std::string timestamp = buffer.data();
        const auto invalidCharacter = [](const char character) {
#if defined(_WIN32)
            return std::string_view("<>:\"/\\|?*").find(character) != std::string_view::npos;
#else
            return character == '/';
#endif
        };
        if (std::ranges::any_of(timestamp, invalidCharacter)) {
            std::ranges::replace_if(timestamp, invalidCharacter, '-');
            appendPostReplacementWarning(
                result,
                "Invalid backup filename characters were replaced with '-'");
        }

        std::string candidateBase = backupBase + timestamp;
        if (!candidateBase.empty() && candidateBase.back() == ' ') {
            candidateBase.pop_back();
        }
        std::vector<std::string> candidates;
        if (!candidateBase.empty() && candidateBase.back() != '-') {
            candidates.push_back(candidateBase + ".FCBak");
            candidateBase += '-';
        }
        for (int suffix = 1; suffix < numberOfFiles + 128; ++suffix) {
            candidates.push_back(candidateBase + std::to_string(suffix) + ".FCBak");
        }
        for (const std::string& candidate : candidates) {
            const bool existedBeforeAttempt = Base::FileInfo(candidate).exists();
            const auto install = installDisplacedNoReplace(sourcename, candidate, lease);
            if (install.installed) {
                result.backupCreated = true;
                result.displacedFileConsumed = install.sourceConsumed;
                installationDurable = install.durabilityVerified && install.sourceConsumed;
                installed = candidate;
                if (!install.durabilityVerified) {
                    appendPostReplacementWarning(
                        result,
                        "The backup name was installed, but transfer durability is unverified; "
                        "the displaced snapshot was retained when possible: "
                            + install.error);
                }
                else if (!install.sourceConsumed) {
                    appendPostReplacementWarning(
                        result,
                        "The backup was installed, but the displaced snapshot name remains: "
                            + install.error);
                }
                break;
            }
            if (install.destinationExists && !existedBeforeAttempt) {
                protectedCandidates.push_back(candidate);
            }
            if (!install.destinationExists) {
                result.displacedFileConsumed = install.sourceConsumed;
                appendPostReplacementWarning(
                    result,
                    "Unable to install the displaced previous file in backup history: "
                        + install.error);
                return result;
            }
        }
    }

    if (!result.backupCreated) {
        appendPostReplacementWarning(
            result,
            "Unable to reserve a collision-free timestamp backup history entry");
        return result;
    }
    if (!installationDurable) {
        return result;
    }

    try {
        std::vector<Base::FileInfo> backups;
        Base::FileInfo directory(target.dirPath());
        for (const Base::FileInfo& entry : directory.getDirectoryContent()) {
            if (!entry.isFile()) {
                continue;
            }
            const std::string file = entry.fileName();
            std::string entryExtension = entry.extension();
            std::ranges::transform(entryExtension,
                                   entryExtension.begin(),
                                   [](const unsigned char ch) {
                                       return static_cast<char>(std::toupper(ch));
                                   });
            const bool oldNumeric = startsWith(file, target.fileName())
                && file.length() > target.fileName().length()
                && checkDigits(file.substr(target.fileName().length()));
            const bool timestamped = entryExtension == "FCBAK" && startsWith(file, projectBase)
                && checkValidComplement(file, projectBase, entry.extension());
            if (oldNumeric || timestamped) {
                backups.push_back(entry);
            }
        }
        pruneInstalledHistory(result,
                              std::move(backups),
                              installed,
                              protectedCandidates,
                              numberOfFiles);
    }
    catch (const std::exception& exception) {
        appendPostReplacementWarning(
            result,
            "The new backup is installed, but old history could not be pruned: "
                + std::string(exception.what()));
    }
    return result;
}

void BackupPolicy::applyStandard(const std::string& sourcename, const std::string& targetname) const
{
    // if saving the project data succeeded rename to the actual file name
    if (Base::FileInfo fi(targetname); fi.exists()) {
        if (numberOfFiles > 0) {
            int nSuff = 0;
            std::string fn = fi.fileName();
            Base::FileInfo di(fi.dirPath());
            std::vector<Base::FileInfo> backup;
            std::vector<Base::FileInfo> files = di.getDirectoryContent();
            for (const Base::FileInfo& it : files) {
                if (std::string file = it.fileName(); file.substr(0, fn.length()) == fn) {
                    // starts with the same file name
                    std::string suf(file.substr(fn.length()));
                    if (!suf.empty()) {
                        std::string::size_type nPos = suf.find_first_not_of("0123456789");
                        if (nPos == std::string::npos) {
                            // store all backup files
                            backup.push_back(it);
                            nSuff =
                                std::max<int>(nSuff, static_cast<int>(std::atol(suf.c_str())));
                        }
                    }
                }
            }

            if (!backup.empty() && static_cast<int>(backup.size()) >= numberOfFiles) {
                // delete the oldest backup file we found
                Base::FileInfo del = backup.front();
                for (const Base::FileInfo& it : backup) {
                    if (it.lastModified() < del.lastModified()) {
                        del = it;
                    }
                }

                del.deleteFile();
                fn = del.filePath();
            }
            else {
                // create a new backup file
                std::stringstream str;
                str << fi.filePath() << (nSuff + 1);
                fn = str.str();
            }

            if (!fi.renameFile(fn.c_str())) {
                Base::Console().warning("Cannot rename project file to backup file\n");
            }
        }
        else {
            fi.deleteFile();
        }
    }

    if (Base::FileInfo tmp(sourcename); !tmp.renameFile(targetname.c_str())) {
        throw Base::FileException("Cannot rename tmp save file to project file",
                                  Base::FileInfo(targetname));
    }
}

void BackupPolicy::applyTimeStamp(const std::string& sourcename, const std::string& targetname)
{
    Base::FileInfo fi(targetname);

    std::string fn = sourcename;
    std::string ext = fi.extension();
    std::string bn;   // full path with no extension but with "."
    std::string pbn;  // base name of the project + "."
    if (!ext.empty()) {
        bn = fi.filePath().substr(0, fi.filePath().length() - ext.length());
        pbn = fi.fileName().substr(0, fi.fileName().length() - ext.length());
    }
    else {
        bn = fi.filePath() + ".";
        pbn = fi.fileName() + ".";
    }

    bool backupManagementError = false;  // Note error and report at the end
    if (fi.exists()) {
        if (numberOfFiles > 0) {
            // replace . by - in format to avoid . between base name and extension
            boost::replace_all(saveBackupDateFormat, ".", "-");
            {
                // Remove all extra backups
                std::string filename = fi.fileName();
                Base::FileInfo di(fi.dirPath());
                std::vector<Base::FileInfo> backup;
                std::vector<Base::FileInfo> files = di.getDirectoryContent();
                for (const Base::FileInfo& it : files) {
                    if (it.isFile()) {
                        std::string file = it.fileName();
                        std::string fext = it.extension();
                        std::string fextUp = fext;
                        std::transform(fextUp.begin(),
                                       fextUp.end(),
                                       fextUp.begin(),
                                       static_cast<int (*)(int)>(toupper));
                        // re-enforcing identification of the backup file


                        // old case : the name starts with the full name of the project and
                        // follows with numbers
                        if ((startsWith(file, filename) && (file.length() > filename.length())
                             && checkDigits(file.substr(filename.length())))
                            ||
                            // .FCBak case : The bame starts with the base name of the project +
                            // "."
                            // + complement with no "." + ".FCBak"
                            ((fextUp == "FCBAK") && startsWith(file, pbn)
                             && (checkValidComplement(file, pbn, fext)))) {
                            backup.push_back(it);
                        }
                    }
                }

                if (!backup.empty() && static_cast<int>(backup.size()) >= numberOfFiles) {
                    std::sort(backup.begin(), backup.end(), fileComparisonByDate);
                    // delete the oldest backup file we found
                    // Base::FileInfo del = backup.front();
                    int nb = 0;
                    for (Base::FileInfo& it : backup) {
                        nb++;
                        if (nb >= numberOfFiles) {
                            try {
                                if (!it.deleteFile()) {
                                    backupManagementError = true;
                                    Base::Console().warning("Cannot remove backup file : %s\n",
                                                            it.fileName().c_str());
                                }
                            }
                            catch (...) {
                                backupManagementError = true;
                                Base::Console().warning("Cannot remove backup file : %s\n",
                                                        it.fileName().c_str());
                            }
                        }
                    }
                }
            }  // end remove backup

            // create a new backup file
            {
                int ext2 = 1;
                if (useFCBakExtension) {
                    std::stringstream str;
                    Base::TimeInfo ti = fi.lastModified();
                    time_t s = ti.getTime_t();
                    std::tm local_tm {};
#if defined(_WIN32)
                    localtime_s(&local_tm, &s);  // Windows
#else
                    localtime_r(&s, &local_tm);  // POSIX
#endif
                    constexpr size_t bufferLength = 128;
                    std::array<char, bufferLength> buffer {};
                    if (size_t bytes = std::strftime(buffer.data(),
                                                     bufferLength,
                                                     saveBackupDateFormat.c_str(),
                                                     &local_tm);
                        bytes == 0) {
                        // An error here is typically that we over-ran the maximum buffer length (
                        // which should be a *very* unusual condition).
                        Base::Console().error("Failed to create valid backup file name from format string:\n");
                        Base::Console().error(saveBackupDateFormat.c_str());
                        const auto knownGoodFormat {"%Y-%m-%d_%H-%M-%S"};
                        std::strftime(buffer.data(), bufferLength, knownGoodFormat, &local_tm);
                    }
                    std::string timestamp = buffer.data();

                    auto isInvalidChar = [](char ch) {
#if defined(_WIN32)
                        return std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos;
#else
                        return ch == '/';
#endif
                    };

                    if (std::ranges::any_of(timestamp, isInvalidChar)) {
                        std::ranges::replace_if(timestamp, isInvalidChar, '-');

                        static bool warned = false;
                        if (!warned) {
                            Base::Console().warning(
                                "Backup filename contained invalid characters. "
                                "Automatically replaced with '-'. "
                                "Consider changing the date format in Preferences/Document.\n");
                            warned = true;
                        }
                    }

                    str << bn << timestamp;

                    fn = str.str();
                    bool done = false;

                    if ((fn.empty()) || (fn[fn.length() - 1] == ' ')
                        || (fn[fn.length() - 1] == '-')) {
                        if (fn[fn.length() - 1] == ' ') {
                            fn = fn.substr(0, fn.length() - 1);
                        }
                    }
                    else {
                        if (!renameFileNoErase(fi, fn + ".FCBak")) {
                            fn = fn + "-";
                        }
                        else {
                            done = true;
                        }
                    }

                    if (!done) {
                        while (ext2 < numberOfFiles + 10) {
                            if (renameFileNoErase(fi, fn + std::to_string(ext2) + ".FCBak")) {
                                break;
                            }
                            ext2++;
                        }
                    }
                }
                else {
                    // changed but simpler and solves also the delay sometimes introduced by
                    // google drive
                    while (ext2 < numberOfFiles + 10) {
                        // linux just replace the file if exists, and then the existence is to
                        // be tested before rename
                        if (renameFileNoErase(fi, fi.filePath() + std::to_string(ext2))) {
                            break;
                        }
                        ext2++;
                    }
                }

                if (ext2 >= numberOfFiles + 10) {
                    Base::Console().error(
                        "File not saved: Cannot rename project file to backup file\n");
                    // throw Base::FileException("File not saved: Cannot rename project file to
                    // backup file", fi);
                }
            }
        }
        else {
            try {
                fi.deleteFile();
            }
            catch (...) {
                Base::Console().warning("Cannot remove backup file: %s\n",
                                        fi.fileName().c_str());
                backupManagementError = true;
            }
        }
    }

    Base::FileInfo tmp(sourcename);
    if (!tmp.renameFile(targetname.c_str())) {
        throw Base::FileException(
            "Save interrupted: Cannot rename temporary file to project file",
            tmp);
    }

    if (backupManagementError) {
        throw Base::FileException(
            "Warning: Save complete, but error while managing backup history.",
            fi);
    }
}

bool BackupPolicy::fileComparisonByDate(const Base::FileInfo& i, const Base::FileInfo& j)
{
    return (i.lastModified() > j.lastModified());
}

bool BackupPolicy::startsWith(const std::string& st1, const std::string& st2) const
{
    return st1.substr(0, st2.length()) == st2;
}

bool BackupPolicy::checkValidString(const std::string& cmpl, const boost::regex& e) const
{
    boost::smatch what;
    const bool res = boost::regex_search(cmpl, what, e);
    return res;
}

bool BackupPolicy::checkValidComplement(const std::string& file,
                          const std::string& pbn,
                          const std::string& ext) const
{
    const std::string cmpl =
        file.substr(pbn.length(), file.length() - pbn.length() - ext.length() - 1);
    const boost::regex e(R"(^[^.]*$)");
    return checkValidString(cmpl, e);
}

bool BackupPolicy::checkDigits(const std::string& cmpl) const
{
    const boost::regex e(R"(^[0-9]*$)");
    return checkValidString(cmpl, e);
}

bool BackupPolicy::renameFileNoErase(Base::FileInfo fi, const std::string& newName)
{
    // A separate exists() check is a no-clobber TOCTOU. The OS primitive owns
    // the collision decision at the namespace boundary.
    try {
        const auto result = atomicInstallNoReplace(fi.filePath(), newName, false);
        return result.installed;
    }
    catch (...) {
        // This historical helper reports inability to reserve a name as false;
        // legacy apply() retains its established Base::FileException boundary.
        return false;
    }
}
