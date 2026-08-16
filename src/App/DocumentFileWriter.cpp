// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 The FreeCAD project association AISBL             *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the          *
 *   GNU Library General Public License for more details.                  *
 ***************************************************************************/

#include "DocumentFileWriter.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
# include <QByteArrayView>
#endif

#include <FCConfig.h>

#include <Base/Exception.h>
#include <Base/FileInfo.h>
#include <Base/FileLock.h>
#include <Base/Uuid.h>

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API) && !defined(FreeCADApp_EXPORTS)
# error "DocumentFileWriter test exports may only be built by the FreeCADApp target"
#endif

#ifdef FC_OS_WIN32
# include <windows.h>
# include <Aclapi.h>
#else
# include <fcntl.h>
# if defined(__linux__)
#  include <sys/syscall.h>
# elif defined(__APPLE__)
#  include <stdio.h>
# endif
# include <sys/stat.h>
# include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace App::Internal
{
namespace
{

constexpr std::size_t ioBufferSize = 1024 * 1024;

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
struct RequestDecoratorEntry
{
    std::uint64_t id {0};
    DocumentFileWriterRequestDecorator decorator;
};

struct RequestDecoratorRegistry
{
    std::mutex mutex;
    std::vector<RequestDecoratorEntry> entries;
    std::uint64_t nextId {1};
};

RequestDecoratorRegistry& requestDecoratorRegistry()
{
    // Intentionally process-lifetime storage: a guard owned by a static test
    // fixture can safely unregister during shutdown without a destruction-
    // order race against the registry mutex.
    static auto* registry = new RequestDecoratorRegistry;
    return *registry;
}

void unregisterRequestDecorator(const std::uint64_t id) noexcept
{
    try {
        auto& registry = requestDecoratorRegistry();
        const std::scoped_lock lock(registry.mutex);
        std::erase_if(registry.entries, [id](const RequestDecoratorEntry& entry) {
            return entry.id == id;
        });
    }
    catch (...) {
        // Guard destruction must not throw. Failure is limited to leaked
        // developer-test state and cannot occur in production builds.
    }
}

struct RequestDecoratorRegistration
{
    ~RequestDecoratorRegistration()
    {
        unregisterRequestDecorator(id);
    }

    std::uint64_t id {0};
};

void decorateRequestForTesting(DocumentFileReplacementRequest& request)
{
    DocumentFileWriterRequestDecorator decorator;
    {
        auto& registry = requestDecoratorRegistry();
        const std::scoped_lock lock(registry.mutex);
        if (!registry.entries.empty()) {
            decorator = registry.entries.back().decorator;
        }
    }
    if (decorator) {
        decorator(request);
    }
}
#endif

class MetadataUnsupportedError: public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

enum class NativeSeekOrigin
{
    Begin,
    Current,
    End,
};

struct FileSnapshot
{
    bool exists {false};
    bool regular {false};
    bool link {false};
    std::string identity;
    std::uintmax_t size {0};
    std::uint64_t created {0};
    std::uint64_t modified {0};
    std::uint32_t attributes {0};
};

std::string pathToUtf8(const fs::path& path)
{
    return Base::FileInfo::pathToString(path);
}

fs::path pathFromUtf8(const std::string& path)
{
    return Base::FileInfo::stringToPath(path);
}

std::string systemMessage(const std::error_code& error)
{
    return error ? error.message() : std::string {};
}

#ifdef FC_OS_WIN32
std::string windowsIdentity(const BY_HANDLE_FILE_INFORMATION& info)
{
    std::ostringstream stream;
    stream << std::hex << info.dwVolumeSerialNumber << ':' << info.nFileIndexHigh << ':'
           << info.nFileIndexLow;
    return stream.str();
}

FileSnapshot snapshotFromHandle(const HANDLE handle)
{
    BY_HANDLE_FILE_INFORMATION info {};
    if (GetFileInformationByHandle(handle, &info) == 0) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "Unable to identify an open save file");
    }
    FileSnapshot result;
    result.exists = true;
    result.link = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    result.regular = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && !result.link;
    result.identity = windowsIdentity(info);
    result.size = (static_cast<std::uintmax_t>(info.nFileSizeHigh) << 32U)
        | static_cast<std::uintmax_t>(info.nFileSizeLow);
    result.created = (static_cast<std::uint64_t>(info.ftCreationTime.dwHighDateTime) << 32U)
        | static_cast<std::uint64_t>(info.ftCreationTime.dwLowDateTime);
    result.modified = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32U)
        | static_cast<std::uint64_t>(info.ftLastWriteTime.dwLowDateTime);
    result.attributes = info.dwFileAttributes;
    return result;
}

FileSnapshot inspectPath(const fs::path& path, const bool followLinks)
{
    FileSnapshot result;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return result;
        }
        throw std::system_error(static_cast<int>(error),
                                std::system_category(),
                                "Unable to inspect a save path");
    }

    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (!followLinks) {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        flags |= FILE_FLAG_BACKUP_SEMANTICS;
    }
    const HANDLE handle = CreateFileW(path.c_str(),
                                      FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      flags,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "Unable to open a save path for identity validation");
    }
    try {
        result = snapshotFromHandle(handle);
        if (!followLinks) {
            result.link = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            result.regular = (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && !result.link;
        }
    }
    catch (...) {
        CloseHandle(handle);
        throw;
    }
    CloseHandle(handle);
    return result;
}

// --- Windows relative rename ---------------------------------------------
//
// The pinned-parent design renames a sibling by naming a leaf relative to a
// retained directory handle, so no path is ever re-resolved and no CWD is
// consulted. SetFileInformationByHandle cannot express that: it ignores a
// non-NULL RootDirectory and fails every such call with
// ERROR_INVALID_PARAMETER. Measured on NTFS, Windows 10.0.26200, for both
// FileRenameInfo and FileRenameInfoEx, destination absent or present,
// replace and no-replace -- all 16 combinations.
//
// Passing RootDirectory = NULL with a bare leaf is not an alternative: the
// name then resolves against the process current directory, and the file is
// silently moved out of its parent. That was measured too.
//
// Only the NT information classes honour the directory handle, so the
// guarantee is expressed through NtSetInformationFile. Same measurement
// harness: relative rename through the pinned parent succeeds, and
// no-replace onto an existing destination reports STATUS_OBJECT_NAME_COLLISION.

constexpr LONG statusObjectNameCollision = static_cast<LONG>(0xC0000035);
constexpr LONG statusInvalidParameter = static_cast<LONG>(0xC000000D);
constexpr LONG statusNotImplemented = static_cast<LONG>(0xC0000002);
constexpr LONG statusNotSupported = static_cast<LONG>(0xC00000BB);
constexpr LONG statusInvalidInfoClass = static_cast<LONG>(0xC0000003);

constexpr ULONG fileRenameInformation = 10;
constexpr ULONG fileRenameInformationEx = 65;

constexpr ULONG renameFlagReplaceIfExists = 0x00000001;
constexpr ULONG renameFlagPosixSemantics = 0x00000002;
// Windows refuses to replace a destination carrying FILE_ATTRIBUTE_READONLY.
// This flag is the supported way to proceed: the filesystem itself disregards
// the attribute for this one operation, so the writer never has to clear it by
// pathname and open a window in which the file is genuinely writable.
constexpr ULONG renameFlagIgnoreReadonlyAttribute = 0x00000040;

struct NtIoStatusBlock
{
    union
    {
        LONG status;
        void* pointer;
    };
    ULONG_PTR information;
};

using NtSetInformationFileFn = LONG(NTAPI*)(HANDLE, NtIoStatusBlock*, void*, ULONG, ULONG);
using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(LONG);

NtSetInformationFileFn ntSetInformationFile()
{
    static NtSetInformationFileFn function = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll != nullptr ? reinterpret_cast<NtSetInformationFileFn>(
                   reinterpret_cast<void*>(GetProcAddress(ntdll, "NtSetInformationFile")))
                                : nullptr;
    }();
    return function;
}

std::error_code dosErrorFromStatus(const LONG status)
{
    static RtlNtStatusToDosErrorFn translate = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll != nullptr ? reinterpret_cast<RtlNtStatusToDosErrorFn>(
                   reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")))
                                : nullptr;
    }();
    const ULONG dosError =
        translate != nullptr ? translate(status) : static_cast<ULONG>(ERROR_GEN_FAILURE);
    return std::error_code(static_cast<int>(dosError), std::system_category());
}

enum class NtRenameOutcome
{
    Renamed,
    DestinationExists,
    Unsupported,
    Failed,
};

struct NtRenameResult
{
    NtRenameOutcome outcome {NtRenameOutcome::Failed};
    std::error_code error;
};

/*!
 * Rename \a source to \a leaf inside \a parentDirectory, naming the
 * destination only relative to that retained handle.
 *
 * The buffer is the official SDK FILE_RENAME_INFO layout, which is identical
 * to the NT FILE_RENAME_INFORMATION. It is zero initialized, allocated with
 * new[] so it carries at least 16-byte alignment for the embedded HANDLE, and
 * sized with explicit capacity for a terminating NUL that FileNameLength
 * itself excludes.
 */
NtRenameResult ntRenameThroughParent(const HANDLE source,
                                     const HANDLE parentDirectory,
                                     const std::wstring& leaf,
                                     const bool replaceIfExists,
                                     const bool extended,
                                     const bool ignoreReadonlyAttribute = false)
{
    auto* const setInformation = ntSetInformationFile();
    if (setInformation == nullptr) {
        return {NtRenameOutcome::Unsupported,
                std::error_code(ERROR_PROC_NOT_FOUND, std::system_category())};
    }
    if (leaf.empty() || leaf.find(L'\\') != std::wstring::npos
        || leaf.find(L'/') != std::wstring::npos) {
        return {NtRenameOutcome::Failed,
                std::error_code(ERROR_INVALID_NAME, std::system_category())};
    }

    const auto nameBytes = static_cast<ULONG>(leaf.size() * sizeof(wchar_t));
    const std::size_t bytes =
        offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t);
    auto storage = std::make_unique<std::byte[]>(bytes);
    std::memset(storage.get(), 0, bytes);
    auto* const info = reinterpret_cast<FILE_RENAME_INFO*>(storage.get());

    if (extended) {
        // The extended class reads the same leading word as flags.
        auto* const flags = reinterpret_cast<ULONG*>(storage.get());
        *flags = (replaceIfExists ? renameFlagReplaceIfExists : 0U) | renameFlagPosixSemantics
            | (ignoreReadonlyAttribute ? renameFlagIgnoreReadonlyAttribute : 0U);
    }
    else {
        info->ReplaceIfExists = replaceIfExists ? TRUE : FALSE;
    }
    info->RootDirectory = parentDirectory;
    info->FileNameLength = nameBytes;
    std::memcpy(info->FileName, leaf.data(), nameBytes);

    NtIoStatusBlock iosb {};
    const LONG status = setInformation(source,
                                       &iosb,
                                       info,
                                       static_cast<ULONG>(bytes),
                                       extended ? fileRenameInformationEx : fileRenameInformation);
    if (status >= 0) {
        return {NtRenameOutcome::Renamed, {}};
    }
    if (status == statusObjectNameCollision) {
        return {NtRenameOutcome::DestinationExists, dosErrorFromStatus(status)};
    }
    if (status == statusNotImplemented || status == statusNotSupported
        || status == statusInvalidInfoClass || (extended && status == statusInvalidParameter)) {
        return {NtRenameOutcome::Unsupported, dosErrorFromStatus(status)};
    }
    return {NtRenameOutcome::Failed, dosErrorFromStatus(status)};
}
#else
std::string posixIdentity(const struct stat& info)
{
    return std::to_string(static_cast<std::uintmax_t>(info.st_dev)) + ':'
        + std::to_string(static_cast<std::uintmax_t>(info.st_ino));
}

FileSnapshot snapshotFromStat(const struct stat& info)
{
    FileSnapshot result;
    result.exists = true;
    result.link = S_ISLNK(info.st_mode);
    result.regular = S_ISREG(info.st_mode);
    result.identity = posixIdentity(info);
    result.size = static_cast<std::uintmax_t>(info.st_size);
    result.attributes = static_cast<std::uint32_t>(info.st_mode);
# if defined(__APPLE__)
    result.modified = (static_cast<std::uint64_t>(info.st_mtimespec.tv_sec) << 32U)
        ^ static_cast<std::uint64_t>(info.st_mtimespec.tv_nsec);
# else
    result.modified = (static_cast<std::uint64_t>(info.st_mtim.tv_sec) << 32U)
        ^ static_cast<std::uint64_t>(info.st_mtim.tv_nsec);
# endif
    return result;
}

FileSnapshot snapshotFromHandle(const int descriptor)
{
    struct stat info {};
    if (::fstat(descriptor, &info) != 0) {
        throw std::system_error(errno,
                                std::generic_category(),
                                "Unable to identify an open save file");
    }
    return snapshotFromStat(info);
}

FileSnapshot inspectPath(const fs::path& path, const bool followLinks)
{
    struct stat info {};
    const int status = followLinks ? ::stat(path.c_str(), &info) : ::lstat(path.c_str(), &info);
    if (status != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return {};
        }
        throw std::system_error(errno, std::generic_category(), "Unable to inspect a save path");
    }
    return snapshotFromStat(info);
}
#endif

bool sameObservedFile(const FileSnapshot& left, const FileSnapshot& right)
{
    return left.exists == right.exists && left.regular == right.regular
        && left.link == right.link && left.identity == right.identity && left.size == right.size
        && left.created == right.created && left.modified == right.modified
        && left.attributes == right.attributes;
}

bool samePersistentFileState(const FileSnapshot& left, const FileSnapshot& right)
{
    return left.regular == right.regular && left.size == right.size
        && left.created == right.created && left.modified == right.modified
        && left.attributes == right.attributes;
}

bool platformPathsEqual(const fs::path& left, const fs::path& right)
{
#ifdef FC_OS_WIN32
    std::wstring lhs = left.native();
    std::wstring rhs = right.native();
    std::ranges::transform(lhs, lhs.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    std::ranges::transform(rhs, rhs.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    return lhs == rhs;
#else
    return left == right;
#endif
}

fs::path normalizeFinalEntry(const std::string& value, const bool createParentDirectories)
{
    if (value.empty()) {
        throw std::invalid_argument("A file path is required");
    }
    std::error_code error;
    const auto absolute = fs::absolute(pathFromUtf8(value), error);
    const auto filename = absolute.filename();
    if (error || filename.empty() || filename == "." || filename == "..") {
        throw std::invalid_argument("The destination path is invalid");
    }
    auto parent = absolute.parent_path();
    if (createParentDirectories) {
        fs::create_directories(parent, error);
        if (error) {
            throw std::system_error(error, "Unable to create the destination directory");
        }
    }
    if (!fs::is_directory(parent, error) || error) {
        throw std::invalid_argument("The destination parent is not a directory");
    }
    auto resolvedParent = fs::weakly_canonical(parent, error);
    if (error) {
        throw std::system_error(error, "Unable to resolve the destination directory");
    }
    // Never canonicalize the final component: replacement must operate on the
    // requested namespace entry and must not follow it to a link target.
    return (resolvedParent / filename).lexically_normal();
}

fs::path normalizeDiagnosticFinalEntry(const std::string& value)
{
    if (value.empty()) {
        throw std::invalid_argument("A file path is required");
    }
    std::error_code error;
    const auto absolute = fs::absolute(pathFromUtf8(value), error);
    const auto filename = absolute.filename();
    if (error || filename.empty() || filename == "." || filename == "..") {
        throw std::invalid_argument("The file path is invalid");
    }
    auto parent = fs::weakly_canonical(absolute.parent_path(), error);
    if (error) {
        parent = absolute.parent_path().lexically_normal();
    }
    return (parent / filename).lexically_normal();
}

fs::path normalizeDestination(const DocumentFileReplacementRequest& request)
{
    return normalizeFinalEntry(request.destination, request.createParentDirectories);
}

class PinnedDirectory
{
public:
#ifdef FC_OS_WIN32
    using Handle = HANDLE;
#else
    using Handle = int;
#endif

    explicit PinnedDirectory(fs::path path)
        : _path(std::move(path))
    {
#ifdef FC_OS_WIN32
        _handle = CreateFileW(_path.c_str(),
                              FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_READ_ATTRIBUTES
                                  | SYNCHRONIZE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS,
                              nullptr);
        if (_handle == INVALID_HANDLE_VALUE) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to pin the destination directory");
        }
#else
        int flags = O_RDONLY | O_CLOEXEC;
# ifdef O_DIRECTORY
        flags |= O_DIRECTORY;
# endif
        _handle = ::open(_path.c_str(), flags);
        if (_handle < 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    "Unable to pin the destination directory");
        }
#endif
        try {
            _identity = snapshotFromHandle(_handle).identity;
            if (!pathStillOwned()) {
                throw std::runtime_error(
                    "The destination directory changed while it was being pinned");
            }
        }
        catch (...) {
            close();
            throw;
        }
    }

    ~PinnedDirectory()
    {
        close();
    }

    PinnedDirectory(const PinnedDirectory&) = delete;
    PinnedDirectory(PinnedDirectory&&) = delete;
    PinnedDirectory& operator=(const PinnedDirectory&) = delete;
    PinnedDirectory& operator=(PinnedDirectory&&) = delete;

    [[nodiscard]] Handle handle() const noexcept
    {
        return _handle;
    }

    [[nodiscard]] const fs::path& path() const noexcept
    {
        return _path;
    }

    [[nodiscard]] bool pathStillOwned() const
    {
        const auto current = inspectPath(_path, true);
        return current.exists && current.identity == _identity;
    }

    void flush()
    {
#ifdef FC_OS_WIN32
        // Windows namespace durability is completed by flushing the retained
        // installed file handle after exact-handle rename. Directory handles
        // do not provide a portable FlushFileBuffers contract.
#else
        if (::fsync(_handle) != 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    "Unable to flush the destination directory");
        }
#endif
    }

private:
    void close() noexcept
    {
#ifdef FC_OS_WIN32
        if (_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(_handle);
            _handle = INVALID_HANDLE_VALUE;
        }
#else
        if (_handle >= 0) {
            ::close(_handle);
            _handle = -1;
        }
#endif
    }

    fs::path _path;
#ifdef FC_OS_WIN32
    Handle _handle {INVALID_HANDLE_VALUE};
#else
    Handle _handle {-1};
#endif
    std::string _identity;
};

class NativeFile
{
public:
#ifdef FC_OS_WIN32
    using Handle = HANDLE;
#else
    using Handle = int;
#endif

    enum class CleanupState
    {
        None,
        Owned,
        Transferred,
        Relinquished,
    };

    NativeFile(fs::path path,
               const Handle handle,
               const bool ownsPath,
               std::shared_ptr<PinnedDirectory> parent = {}) try
        : _path(std::move(path))
        , _pathUtf8(pathToUtf8(_path))
        , _handle(handle)
        , _parent(std::move(parent))
        , _cleanupState(ownsPath ? CleanupState::Owned : CleanupState::None)
    {
        _identity = snapshotFromHandle(_handle).identity;
    }
    catch (...) {
#ifdef FC_OS_WIN32
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
#else
        if (handle >= 0) {
            ::close(handle);
        }
#endif
        throw;
    }

    ~NativeFile()
    {
        cleanupOwnedFile();
        close();
    }

    NativeFile(const NativeFile&) = delete;
    NativeFile(NativeFile&&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    NativeFile& operator=(NativeFile&&) = delete;

    static std::shared_ptr<NativeFile>
    createSibling(const std::shared_ptr<PinnedDirectory>& parent,
                  const fs::path& destination,
                  const std::string_view suffix,
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                  const std::function<void(const std::string&)>& beforeAttempt = {}
#else
                  const void* = nullptr
#endif
    )
    {
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto name = destination.filename().native()
                + pathFromUtf8("." + Base::Uuid::createUuid() + std::string(suffix)).native();
            const auto candidate = destination.parent_path() / name;
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
            if (beforeAttempt) {
                beforeAttempt(pathToUtf8(candidate));
            }
#endif
#ifdef FC_OS_WIN32
            const HANDLE handle = CreateFileW(candidate.c_str(),
                                               GENERIC_READ | GENERIC_WRITE | DELETE
                                                   | FILE_READ_ATTRIBUTES
                                                   | FILE_WRITE_ATTRIBUTES | READ_CONTROL
                                                   | WRITE_DAC,
                                               FILE_SHARE_READ | FILE_SHARE_DELETE,
                                              nullptr,
                                              CREATE_NEW,
                                              FILE_ATTRIBUTE_NORMAL,
                                              nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                return std::shared_ptr<NativeFile>(
                    new NativeFile(candidate, handle, true, parent));
            }
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
                throw std::system_error(static_cast<int>(error),
                                        std::system_category(),
                                        "Unable to reserve a sibling save file");
            }
#else
            const auto leaf = candidate.filename();
            const int descriptor = ::openat(parent->handle(),
                                            leaf.c_str(),
                                            O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC,
                                            0666);
            if (descriptor >= 0) {
                return std::shared_ptr<NativeFile>(
                    new NativeFile(candidate, descriptor, true, parent));
            }
            if (errno != EEXIST) {
                throw std::system_error(errno,
                                        std::generic_category(),
                                        "Unable to reserve a sibling save file");
            }
#endif
        }
        throw std::runtime_error("Unable to reserve a unique sibling save file");
    }

    static std::shared_ptr<NativeFile>
    openRegularNoFollow(const fs::path& path,
                        const std::shared_ptr<PinnedDirectory>& parent,
                        const bool requireRenameAuthority,
                        const bool requireDurabilityAuthority = false)
    {
#ifdef FC_OS_WIN32
        DWORD access = GENERIC_READ | FILE_READ_ATTRIBUTES | READ_CONTROL;
        DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        if (requireRenameAuthority) {
            access |= GENERIC_WRITE | DELETE | FILE_WRITE_ATTRIBUTES;
            // Keep all normal shares so a non-cooperating writer is handled
            // by the guard hash/identity proof instead of by an advisory open
            // convention. DELETE is authoritative for the exact move and
            // GENERIC_WRITE keeps FlushFileBuffers truthful after that move.
        }
        else if (requireDurabilityAuthority) {
            // FlushFileBuffers requires GENERIC_WRITE on the handle and fails
            // ERROR_ACCESS_DENIED without it. fsync has no such requirement,
            // which is why a read-only handle was enough on POSIX and not
            // here. No DELETE is added: this handle proves durability, it
            // never moves or removes anything.
            access |= GENERIC_WRITE;
        }
        const auto open = [&](const DWORD desired) {
            return CreateFileW(path.c_str(),
                               desired,
                               sharing,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT
                                   | FILE_FLAG_SEQUENTIAL_SCAN,
                               nullptr);
        };
        // Replacing a read-only destination needs attribute authority over it.
        // Ask for it, but never let asking be the reason a save cannot start:
        // a DACL may withhold it, and only a destination that turns out to be
        // read-only actually needs it. Whether it was granted is recorded, and
        // the replacement path fails closed later if it is missing and needed.
        bool attributeAuthority = (access & FILE_WRITE_ATTRIBUTES) != 0;
        HANDLE handle = open(access | FILE_WRITE_ATTRIBUTES);
        if (handle != INVALID_HANDLE_VALUE) {
            attributeAuthority = true;
        }
        else if (!attributeAuthority) {
            handle = open(access);
        }
        if (handle == INVALID_HANDLE_VALUE) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to open the previous file for a stable snapshot");
        }
#else
        int flags = (requireRenameAuthority ? O_RDWR : O_RDONLY) | O_CLOEXEC;
# ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
# endif
        const int handle = ::open(path.c_str(), flags);
        if (handle < 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    "Unable to open the previous file for a stable snapshot");
        }
#endif
        auto result =
            std::shared_ptr<NativeFile>(new NativeFile(path, handle, false, parent));
#ifdef FC_OS_WIN32
        result->_attributeAuthority = attributeAuthority;
#endif
        const auto snapshot = result->snapshot();
        const auto pathSnapshot = inspectPath(path, false);
        if (!snapshot.regular || !pathSnapshot.regular
            || snapshot.identity != pathSnapshot.identity) {
            throw std::runtime_error("The previous file is not a stable regular file");
        }
        return result;
    }

    [[nodiscard]] const fs::path& path() const noexcept
    {
        return _path;
    }

    [[nodiscard]] const std::string& pathUtf8() const noexcept
    {
        return _pathUtf8;
    }

    [[nodiscard]] const std::string& identity() const noexcept
    {
        return _identity;
    }

    [[nodiscard]] Handle handle() const noexcept
    {
        return _handle;
    }

    [[nodiscard]] FileSnapshot snapshot() const
    {
        return snapshotFromHandle(_handle);
    }

    [[nodiscard]] bool pathStillOwned() const
    {
        const auto current = inspectPath(_path, false);
        return current.exists && current.regular && current.identity == _identity;
    }

    [[nodiscard]] bool pinnedPathStillOwned() const
    {
        if (!_parent) {
            return pathStillOwned();
        }
#ifdef FC_OS_WIN32
        return pathStillOwned();
#else
        struct stat current {};
        const auto leaf = _path.filename();
        return ::fstatat(_parent->handle(), leaf.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0
            && S_ISREG(current.st_mode) && posixIdentity(current) == _identity;
#endif
    }

    [[nodiscard]] bool parentPathStillOwned() const
    {
        return _parent && _parent->pathStillOwned();
    }

    [[nodiscard]] PinnedDirectory& parent()
    {
        if (!_parent) {
            throw std::runtime_error("The owned file has no pinned parent directory");
        }
        return *_parent;
    }

    void relinquishPathCleanup() noexcept
    {
        _cleanupState = CleanupState::Relinquished;
    }

    /**
     * Close this object's descriptor without touching the namespace.
     *
     * Some filesystems leave a renamed destination unresolvable by name for as
     * long as any descriptor to the installed inode stays open. This is
     * reproducible on the 9p bind mount used by Docker Desktop, where the name
     * never becomes resolvable while the descriptor is held. Verification there
     * is only possible after release, so callers must capture everything they
     * need from the handle first. The bytes are already installed under the
     * destination name, so nothing may be removed on the way out.
     */
    void releaseDescriptor() noexcept
    {
        _cleanupState = CleanupState::Relinquished;
        close();
    }

    /**
     * Re-open an installed destination through the pinned parent directory,
     * without following links.
     */
    static std::shared_ptr<NativeFile>
    openInstalledThroughPinnedParent(const std::shared_ptr<PinnedDirectory>& parent,
                                     const fs::path& destination)
    {
#ifdef FC_OS_WIN32
        // Windows has no openat. The pinned parent already prevents the
        // directory itself from being exchanged underneath the lookup. This
        // handle exists to re-prove and flush the installed file, so it is
        // opened with the write access FlushFileBuffers demands.
        return openRegularNoFollow(destination, parent, false, true);
#else
        const auto leaf = destination.filename();
        int flags = O_RDONLY | O_CLOEXEC;
# ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
# endif
        const int handle = ::openat(parent->handle(), leaf.c_str(), flags);
        if (handle < 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    "Unable to re-resolve the installed destination");
        }
        auto result =
            std::shared_ptr<NativeFile>(new NativeFile(destination, handle, false, parent));
        if (!result->snapshot().regular) {
            throw std::runtime_error("The installed destination is not a regular file");
        }
        return result;
#endif
    }

    void claimOwnedPathCleanup() noexcept
    {
        _cleanupState = CleanupState::Owned;
    }

    void bindMovedOwnedPath(fs::path path, std::string pathUtf8) noexcept
    {
        // Both values are fully allocated before the namespace primitive.
        // Swapping them afterward cannot strand a moved file during unwind.
        _path.swap(path);
        _pathUtf8.swap(pathUtf8);
        _cleanupState = CleanupState::Owned;
    }

    void bindRestoredCanonicalPath(fs::path path, std::string pathUtf8) noexcept
    {
        _path.swap(path);
        _pathUtf8.swap(pathUtf8);
        _cleanupState = CleanupState::None;
    }

    void markTransferred(const fs::path&) noexcept
    {
        // Set the cleanup disposition without allocating. This method is
        // called only after the namespace primitive has consumed the owned
        // source name; throwing here could otherwise make unwinding delete an
        // already-installed canonical or backup file.
        _cleanupState = CleanupState::Transferred;
    }

    void markSourceConsumed() noexcept
    {
        _cleanupState = CleanupState::None;
    }

    /**
     * Mark this file as an EphemeralPartial serialization temporary.
     *
     * See the save-artifact contract in
     * doc/save-artifact-contract.md. EphemeralPartial is the only artifact
     * class whose directory entry may be removed by cleanup: it is created
     * O_EXCL under an unpredictable UUID name, it is never published to the
     * user, and it never holds a version of the user's document. Canonical,
     * displaced, backup and verified-recovery artifacts must never be marked.
     */
    void markEphemeralPartial() noexcept
    {
        _ephemeralPartial = true;
    }

    /**
     * Promote an EphemeralPartial to VerifiedSerialization (contract §2.2, Q1).
     *
     * Once the serialized bytes have been hashed and stably verified they are
     * the only copy of work the user asked to save, so they leave the one class
     * that cleanup may remove. From here the file is either consumed by the
     * install primitive or retained and reported as recovery evidence.
     */
    void markVerifiedSerialization() noexcept
    {
        _ephemeralPartial = false;
    }

    /**
     * Promote a verified previous-file snapshot to a DisplacedCanonical
     * (contract §2.3).
     *
     * Before its content is proved the snapshot is an unpublished partial copy
     * that duplicates a canonical file which is still intact, so it is
     * cleanup-eligible. Once proved it holds a real version of the user's
     * document and cleanup may never remove it.
     */
    void promoteToDisplacedCanonical() noexcept
    {
        _ephemeralPartial = false;
    }

    /**
     * Remove a DisplacedCanonical at the user's explicit request (contract R5).
     *
     * This is the single carve-out that removes a version of the user's
     * document, and it is reachable only from a `numberOfFiles == 0` retention
     * decision. It is deliberately **not** part of discardExact(): generic
     * cleanup must never be able to reach it. The POSIX path proves the entry
     * still names this exact inode and retains it when that proof fails.
     */
    [[nodiscard]] bool discardDisplacedCanonicalExact() noexcept
    {
        if (_cleanupState != CleanupState::Owned) {
            return false;
        }
#ifdef FC_OS_WIN32
        if (!setDeleteDisposition()) {
            return false;
        }
        _cleanupState = CleanupState::None;
        close();
        return true;
#else
        return unlinkProvedOwnedEntry();
#endif
    }

    [[nodiscard]] bool discardExact() noexcept
    {
        if (_cleanupState != CleanupState::Owned) {
            return false;
        }
#ifdef FC_OS_WIN32
        if (!_ephemeralPartial) {
            // The same rule as the POSIX branch below, for a different reason.
            // Windows *can* remove this file exactly and without a path race,
            // through the retained handle. It may not: for every class other
            // than EphemeralPartial the target is a version of the user's
            // document, and generic cleanup holds no authority to remove one.
            // Capability is not permission. The single carve-out stays
            // discardDisplacedCanonicalExact(), reachable only from an explicit
            // numberOfFiles == 0 retention decision.
            return false;
        }
        if (!setDeleteDisposition()) {
            return false;
        }
        _cleanupState = CleanupState::None;
        close();
        return true;
#else
        if (!_ephemeralPartial) {
            // POSIX has no portable unlink-by-handle or inode-conditional
            // unlink, and for every other artifact class the unlink target is
            // a version of the user's document. Fail closed and leave the
            // proved owned name as explicit recovery evidence. Namespace-
            // consuming rename paths still clear the cleanup state and so do
            // not leak their successful serialization temporary.
            return false;
        }
        return unlinkProvedOwnedEntry();
#endif
    }

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API) && defined(FC_OS_WIN32)
    void forceLegacyDeleteDispositionForTesting() noexcept
    {
        _forceLegacyDeleteDispositionForTesting = true;
    }

    void makeCleanupHostileForTesting()
    {
        FILE_BASIC_INFO info {};
        if (GetFileInformationByHandleEx(_handle, FileBasicInfo, &info, sizeof(info)) == 0) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to inspect the test temporary attributes");
        }
        info.FileAttributes |= FILE_ATTRIBUTE_READONLY;
        if (SetFileInformationByHandle(_handle, FileBasicInfo, &info, sizeof(info)) == 0) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to set the test temporary read-only attribute");
        }
        ACL emptyDacl {};
        if (!InitializeAcl(&emptyDacl, sizeof(emptyDacl), ACL_REVISION)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to initialize the test temporary DACL");
        }
        const DWORD securityStatus = SetSecurityInfo(
            _handle,
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            &emptyDacl,
            nullptr);
        if (securityStatus != ERROR_SUCCESS) {
            throw std::system_error(static_cast<int>(securityStatus),
                                    std::system_category(),
                                    "Unable to restrict the test temporary DACL");
        }
    }
#endif

    void rewind()
    {
        (void)seek(0, NativeSeekOrigin::Begin);
    }

    std::int64_t seek(const std::int64_t offset, const NativeSeekOrigin origin)
    {
#ifdef FC_OS_WIN32
        LARGE_INTEGER requested {};
        requested.QuadPart = offset;
        LARGE_INTEGER resulting {};
        DWORD method = FILE_BEGIN;
        if (origin == NativeSeekOrigin::Current) {
            method = FILE_CURRENT;
        }
        else if (origin == NativeSeekOrigin::End) {
            method = FILE_END;
        }
        if (SetFilePointerEx(_handle, requested, &resulting, method) == 0) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to seek an open save file");
        }
        return resulting.QuadPart;
#else
        int whence = SEEK_SET;
        if (origin == NativeSeekOrigin::Current) {
            whence = SEEK_CUR;
        }
        else if (origin == NativeSeekOrigin::End) {
            whence = SEEK_END;
        }
        const auto resulting = ::lseek(_handle, static_cast<off_t>(offset), whence);
        if (resulting < 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    "Unable to seek an open save file");
        }
        return static_cast<std::int64_t>(resulting);
#endif
    }

    void truncate()
    {
        rewind();
#ifdef FC_OS_WIN32
        if (SetEndOfFile(_handle) == 0) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to truncate a backup snapshot");
        }
#else
        if (::ftruncate(_handle, 0) != 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    "Unable to truncate a backup snapshot");
        }
#endif
    }

    std::size_t read(char* buffer, const std::size_t capacity)
    {
#ifdef FC_OS_WIN32
        DWORD count = 0;
        if (ReadFile(_handle,
                     buffer,
                     static_cast<DWORD>(std::min<std::size_t>(capacity, MAXDWORD)),
                     &count,
                     nullptr)
            == 0) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to read an open save file");
        }
        return static_cast<std::size_t>(count);
#else
        for (;;) {
            const auto count = ::read(_handle, buffer, capacity);
            if (count >= 0) {
                return static_cast<std::size_t>(count);
            }
            if (errno != EINTR) {
                throw std::system_error(errno,
                                        std::generic_category(),
                                        "Unable to read an open save file");
            }
        }
#endif
    }

    void write(const char* buffer, std::size_t count)
    {
        while (count > 0) {
#ifdef FC_OS_WIN32
            DWORD written = 0;
            if (WriteFile(_handle,
                          buffer,
                          static_cast<DWORD>(std::min<std::size_t>(count, MAXDWORD)),
                          &written,
                          nullptr)
                    == 0
                || written == 0) {
                throw std::system_error(static_cast<int>(GetLastError()),
                                        std::system_category(),
                                        "Unable to write a backup snapshot");
            }
            const auto completed = static_cast<std::size_t>(written);
#else
            const auto status = ::write(_handle, buffer, count);
            if (status < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno,
                                        std::generic_category(),
                                        "Unable to write a backup snapshot");
            }
            if (status == 0) {
                throw std::runtime_error("Unable to make progress writing a backup snapshot");
            }
            const auto completed = static_cast<std::size_t>(status);
#endif
            buffer += completed;
            count -= completed;
        }
    }

    void flush()
    {
#ifdef FC_OS_WIN32
        if (FlushFileBuffers(_handle) == 0) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to flush an open save file");
        }
#else
        if (::fsync(_handle) != 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    "Unable to flush an open save file");
        }
#endif
    }

    void copyModificationTimeFrom(const NativeFile& source)
    {
#ifdef FC_OS_WIN32
        BY_HANDLE_FILE_INFORMATION info {};
        if (GetFileInformationByHandle(source._handle, &info) == 0
            || SetFileTime(_handle, nullptr, nullptr, &info.ftLastWriteTime) == 0) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to preserve backup snapshot modification time");
        }
#else
        struct stat info {};
        if (::fstat(source._handle, &info) != 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    "Unable to inspect backup snapshot modification time");
        }
# if defined(__APPLE__)
        const timespec times[2] {info.st_atimespec, info.st_mtimespec};
# else
        const timespec times[2] {info.st_atim, info.st_mtim};
# endif
        if (::futimens(_handle, times) != 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    "Unable to preserve backup snapshot modification time");
        }
#endif
    }

    void preserveMetadataFrom(const NativeFile& source, const bool snapshot)
    {
#ifdef FC_OS_WIN32
        preserveWindowsMetadataFrom(source, snapshot);
#else
        struct stat sourceInfo {};
        if (::fstat(source._handle, &sourceInfo) != 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    "Unable to inspect existing file permissions");
        }
        const mode_t requiredMode = sourceInfo.st_mode & 07777;
        if (::fchmod(_handle, requiredMode) != 0) {
            throw std::system_error(errno,
                                    std::generic_category(),
                                    "Unable to preserve existing file permissions");
        }
        struct stat resultingInfo {};
        if (::fstat(_handle, &resultingInfo) != 0
            || (resultingInfo.st_mode & 07777) != requiredMode) {
            throw std::runtime_error("Existing file permissions could not be verified");
        }
        if (snapshot) {
            copyModificationTimeFrom(source);
        }
#endif
    }

#ifdef FC_OS_WIN32
    /*!
     * Whether this file carries FILE_ATTRIBUTE_READONLY, read through the
     * retained handle rather than by pathname.
     *
     * Windows refuses to replace a destination marked this way, so a save has
     * to know before it reaches the namespace boundary.
     */
    [[nodiscard]] bool isReadOnly() const
    {
        FILE_BASIC_INFO info {};
        if (GetFileInformationByHandleEx(_handle, FileBasicInfo, &info, sizeof(info)) == 0) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to inspect the destination read-only attribute");
        }
        return (info.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    }

    /*!
     * Prove this handle really can write attributes, by writing the ones it
     * already has straight back.
     *
     * All-zero timestamps mean "leave unchanged", and the attribute word is the
     * one just read, so nothing about the file changes -- least of all the
     * read-only bit, which stays exactly as the user set it. What the call
     * establishes is authority: a granted access mask is what CreateFileW
     * reported, and this is the filesystem agreeing.
     */
    [[nodiscard]] bool hasProvenAttributeAuthority() const noexcept
    {
        if (!_attributeAuthority) {
            return false;
        }
        FILE_BASIC_INFO info {};
        if (GetFileInformationByHandleEx(_handle, FileBasicInfo, &info, sizeof(info)) == 0) {
            return false;
        }
        FILE_BASIC_INFO unchanged {};
        unchanged.FileAttributes = info.FileAttributes;
        return SetFileInformationByHandle(_handle, FileBasicInfo, &unchanged, sizeof(unchanged))
            != 0;
    }
#endif

    void prepareForExternalMove()
    {
#ifdef FC_OS_WIN32
        // The originally reserved handle already carries DELETE and write
        // access, denies write sharing, and permits delete sharing. Keep that
        // exact handle through BackupPolicy instead of reopening a pathname.
        const auto retained = snapshotFromHandle(_handle);
        const auto pathSnapshot = inspectPath(_path, false);
        if (!retained.regular || retained.identity != _identity || !pathSnapshot.regular
            || pathSnapshot.identity != _identity) {
            throw std::runtime_error(
                "The displaced snapshot changed while its lease was prepared");
        }
#endif
    }

private:
#ifdef FC_OS_WIN32
    [[nodiscard]] bool setDeleteDisposition() noexcept
    {
        if (_handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        struct LocalDispositionInfoEx
        {
            DWORD flags;
        };
        constexpr auto fileDispositionInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
        constexpr DWORD deleteFlag = 0x00000001;
        constexpr DWORD posixSemantics = 0x00000002;
        constexpr DWORD ignoreReadOnly = 0x00000010;
        bool forceLegacy = false;
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        forceLegacy = _forceLegacyDeleteDispositionForTesting;
#endif
        if (!forceLegacy) {
            LocalDispositionInfoEx extended {deleteFlag | posixSemantics | ignoreReadOnly};
            if (SetFileInformationByHandle(_handle,
                                           fileDispositionInfoEx,
                                           &extended,
                                           sizeof(extended))
                != 0) {
                return true;
            }
            const DWORD extendedError = GetLastError();
            if (extendedError != ERROR_INVALID_PARAMETER
                && extendedError != ERROR_NOT_SUPPORTED
                && extendedError != ERROR_INVALID_FUNCTION
                && extendedError != ERROR_ACCESS_DENIED) {
                return false;
            }
        }

        FILE_BASIC_INFO original {};
        if (GetFileInformationByHandleEx(_handle, FileBasicInfo, &original, sizeof(original))
            == 0) {
            return false;
        }
        bool changedReadOnly = false;
        if ((original.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
            FILE_BASIC_INFO writable = original;
            writable.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
            if (writable.FileAttributes == 0) {
                writable.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            }
            if (SetFileInformationByHandle(_handle, FileBasicInfo, &writable, sizeof(writable))
                == 0) {
                return false;
            }
            changedReadOnly = true;
        }
        FILE_DISPOSITION_INFO disposition {TRUE};
        if (SetFileInformationByHandle(
                _handle, FileDispositionInfo, &disposition, sizeof(disposition))
            != 0) {
            return true;
        }
        if (changedReadOnly) {
            (void)SetFileInformationByHandle(_handle, FileBasicInfo, &original, sizeof(original));
        }
        return false;
    }
#endif

    void cleanupOwnedFile() noexcept
    {
        if (_cleanupState != CleanupState::Owned || _identity.empty()) {
            return;
        }
        // Only an EphemeralPartial may be removed by destructor cleanup. Every
        // other class keeps its name; see discardExact() and the contract. A
        // VerifiedSerialization is the only copy of the work being saved and a
        // DisplacedCanonical is the user's previous version, so destroying this
        // object must never destroy either.
        if (!_ephemeralPartial) {
            return;
        }
#ifdef FC_OS_WIN32
        if (setDeleteDisposition()) {
            _cleanupState = CleanupState::None;
        }
#else
        (void)unlinkProvedOwnedEntry();
#endif
    }

#ifndef FC_OS_WIN32
    /**
     * Remove this file's directory entry after proving it still names the exact
     * retained inode; retain it untouched when the proof fails.
     *
     * Callers own the *authority* to remove; this helper owns only the proof.
     * It is reachable from exactly two places, both of which state their
     * justification: EphemeralPartial cleanup, and the explicit
     * `numberOfFiles == 0` DisplacedCanonical discard.
     *
     * Threat model for the EphemeralPartial caller, deliberately narrower than
     * the rest of this writer: the entry is an unpredictable UUID name this
     * process created with O_EXCL and never published, so winning the
     * proof-to-unlink race requires write access to a directory in which the
     * attacker could already damage the document, and losing the race costs one
     * attacker-planted file rather than user data. The DisplacedCanonical
     * caller does remove user data, and is justified instead by the user having
     * explicitly configured zero retained backups.
     */
    [[nodiscard]] bool unlinkProvedOwnedEntry() noexcept
    {
        if (!_parent || _path.empty() || _identity.empty()) {
            return false;
        }
        const auto leaf = _path.filename();
        struct stat current {};
        if (::fstatat(_parent->handle(), leaf.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISREG(current.st_mode) || posixIdentity(current) != _identity) {
            return false;
        }
        if (::unlinkat(_parent->handle(), leaf.c_str(), 0) != 0) {
            return false;
        }
        _cleanupState = CleanupState::None;
        return true;
    }
#endif

#ifdef FC_OS_WIN32
    struct LocalSecurityDescriptorDeleter
    {
        void operator()(void* value) const noexcept
        {
            if (value) {
                LocalFree(value);
            }
        }
    };

    struct SecurityDescriptorView
    {
        std::unique_ptr<void, LocalSecurityDescriptorDeleter> storage;
        PACL dacl {nullptr};
        bool protectedDacl {false};
    };

    static SecurityDescriptorView readDacl(const HANDLE handle)
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PACL dacl = nullptr;
        const DWORD status = GetSecurityInfo(handle,
                                             SE_FILE_OBJECT,
                                             DACL_SECURITY_INFORMATION,
                                             nullptr,
                                             nullptr,
                                             &dacl,
                                             nullptr,
                                             &descriptor);
        if (status != ERROR_SUCCESS) {
            throw std::system_error(static_cast<int>(status),
                                    std::system_category(),
                                    "Unable to read destination DACL");
        }
        SecurityDescriptorView result;
        result.storage.reset(descriptor);
        result.dacl = dacl;
        SECURITY_DESCRIPTOR_CONTROL control {};
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(descriptor, &control, &revision)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to read destination DACL protection");
        }
        result.protectedDacl = (control & SE_DACL_PROTECTED) != 0;
        return result;
    }

    static bool equivalentAcl(const PACL left, const PACL right)
    {
        if (!left || !right) {
            return left == right;
        }
        return left->AclSize == right->AclSize
            && std::memcmp(left, right, left->AclSize) == 0;
    }

    static bool hasNonDefaultStreams(const HANDLE handle)
    {
        std::vector<std::byte> buffer(4096);
        for (;;) {
            if (GetFileInformationByHandleEx(handle,
                                             FileStreamInfo,
                                             buffer.data(),
                                             static_cast<DWORD>(buffer.size()))
                != 0) {
                break;
            }
            const DWORD error = GetLastError();
            if (error == ERROR_MORE_DATA) {
                buffer.resize(buffer.size() * 2);
                continue;
            }
            if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED
                || error == ERROR_INVALID_FUNCTION) {
                return false;
            }
            throw std::system_error(static_cast<int>(error),
                                    std::system_category(),
                                    "Unable to inspect destination named streams");
        }

        auto* entry = reinterpret_cast<FILE_STREAM_INFO*>(buffer.data());
        for (;;) {
            const std::wstring name(entry->StreamName,
                                    entry->StreamNameLength / sizeof(wchar_t));
            if (name != L"::$DATA") {
                return true;
            }
            if (entry->NextEntryOffset == 0) {
                return false;
            }
            entry = reinterpret_cast<FILE_STREAM_INFO*>(
                reinterpret_cast<std::byte*>(entry) + entry->NextEntryOffset);
        }
    }

    void preserveWindowsMetadataFrom(const NativeFile& source, const bool snapshot)
    {
        FILE_BASIC_INFO sourceInfo {};
        FILE_BASIC_INFO temporaryInfo {};
        if (GetFileInformationByHandleEx(source._handle,
                                         FileBasicInfo,
                                         &sourceInfo,
                                         sizeof(sourceInfo))
                == 0
            || GetFileInformationByHandleEx(_handle,
                                            FileBasicInfo,
                                            &temporaryInfo,
                                            sizeof(temporaryInfo))
                == 0) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to inspect destination basic metadata");
        }

        constexpr DWORD supportedBasicAttributes = FILE_ATTRIBUTE_READONLY
            | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE
            | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE
            | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
        constexpr DWORD structuralAttributes = FILE_ATTRIBUTE_DIRECTORY
            | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT;
        // Every non-structural bit not in supportedBasicAttributes represents
        // filesystem-managed state (compression, EFS, sparse allocation,
        // integrity/no-scrub, EA/recall, pinning, or a future attribute). It is
        // safe only when the newly created sibling already has identical
        // state. This complement intentionally makes new Windows attributes
        // fail closed until an explicit preservation policy is added.
        constexpr DWORD unsupportedAttributes =
            ~(supportedBasicAttributes | structuralAttributes | FILE_ATTRIBUTE_NORMAL);
        if ((sourceInfo.FileAttributes & unsupportedAttributes)
            != (temporaryInfo.FileAttributes & unsupportedAttributes)) {
            throw MetadataUnsupportedError(
                "Filesystem-managed destination attributes cannot be reproduced safely");
        }
        if (hasNonDefaultStreams(source._handle) || hasNonDefaultStreams(_handle)) {
            throw MetadataUnsupportedError(
                "Alternate named streams cannot be reproduced safely during replacement");
        }

        const auto sourceSecurity = readDacl(source._handle);
        SECURITY_INFORMATION securityInformation = DACL_SECURITY_INFORMATION;
        securityInformation |= sourceSecurity.protectedDacl
            ? PROTECTED_DACL_SECURITY_INFORMATION
            : UNPROTECTED_DACL_SECURITY_INFORMATION;
        const DWORD securityStatus = SetSecurityInfo(_handle,
                                                     SE_FILE_OBJECT,
                                                     securityInformation,
                                                     nullptr,
                                                     nullptr,
                                                     sourceSecurity.dacl,
                                                     nullptr);
        if (securityStatus != ERROR_SUCCESS) {
            throw std::system_error(static_cast<int>(securityStatus),
                                    std::system_category(),
                                    "Unable to preserve destination DACL");
        }

        FILE_BASIC_INFO desired = temporaryInfo;
        desired.CreationTime = sourceInfo.CreationTime;
        if (snapshot) {
            desired.LastAccessTime = sourceInfo.LastAccessTime;
            desired.LastWriteTime = sourceInfo.LastWriteTime;
        }
        desired.FileAttributes =
            (temporaryInfo.FileAttributes & unsupportedAttributes)
            | (sourceInfo.FileAttributes & supportedBasicAttributes);
        if (desired.FileAttributes == 0) {
            desired.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        }
        if (SetFileInformationByHandle(_handle, FileBasicInfo, &desired, sizeof(desired)) == 0) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Unable to preserve destination basic metadata");
        }

        FILE_BASIC_INFO verified {};
        if (GetFileInformationByHandleEx(_handle, FileBasicInfo, &verified, sizeof(verified)) == 0
            || verified.CreationTime.QuadPart != sourceInfo.CreationTime.QuadPart
            || (verified.FileAttributes & supportedBasicAttributes)
                != (sourceInfo.FileAttributes & supportedBasicAttributes)
            || (verified.FileAttributes & unsupportedAttributes)
                != (sourceInfo.FileAttributes & unsupportedAttributes)) {
            throw std::runtime_error("Destination basic metadata could not be verified");
        }
        const auto verifiedSecurity = readDacl(_handle);
        if (verifiedSecurity.protectedDacl != sourceSecurity.protectedDacl
            || !equivalentAcl(verifiedSecurity.dacl, sourceSecurity.dacl)) {
            throw std::runtime_error("Destination DACL could not be verified");
        }
    }
#endif

    void close() noexcept
    {
#ifdef FC_OS_WIN32
        if (_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(_handle);
            _handle = INVALID_HANDLE_VALUE;
        }
#else
        if (_handle >= 0) {
            ::close(_handle);
            _handle = -1;
        }
#endif
    }

    fs::path _path;
    std::string _pathUtf8;
#ifdef FC_OS_WIN32
    Handle _handle {INVALID_HANDLE_VALUE};
#else
    Handle _handle {-1};
#endif
    std::shared_ptr<PinnedDirectory> _parent;
    CleanupState _cleanupState {CleanupState::None};
    // Set only for the serialization temporary; gates the one POSIX cleanup
    // path that is permitted to remove a directory entry.
    bool _ephemeralPartial {false};
    std::string _identity;
#ifdef FC_OS_WIN32
    // Whether this handle was granted FILE_WRITE_ATTRIBUTES. Only a read-only
    // destination needs it, so it is recorded rather than required at open.
    bool _attributeAuthority {false};
#endif
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API) && defined(FC_OS_WIN32)
    bool _forceLegacyDeleteDispositionForTesting {false};
#endif
};

class RetainedHandleStreamBuffer: public std::streambuf
{
public:
    explicit RetainedHandleStreamBuffer(
        NativeFile& file
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        ,
        const DocumentFileWriterTestFault fault
#endif
        )
        : _file(file)
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        , _fault(fault)
#endif
    {}

    [[nodiscard]] bool seal(std::string& error)
    {
        if (!_sealed) {
            if (sync() != 0) {
                _failed = true;
            }
            _sealed = true;
        }
        if (_failed) {
            error = errorMessage();
            return false;
        }
        return true;
    }

protected:
    std::streamsize xsputn(const char* data, const std::streamsize count) noexcept override
    {
        if (_sealed || _failed || count < 0) {
            return 0;
        }
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        if (_fault == DocumentFileWriterTestFault::SerializationWrite) {
            latch("Injected serialization write failure");
            return 0;
        }
#endif
        try {
            _file.write(data, static_cast<std::size_t>(count));
            return count;
        }
        catch (...) {
            latchCurrent();
            return 0;
        }
    }

    int_type overflow(const int_type value = traits_type::eof()) noexcept override
    {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        const char character = traits_type::to_char_type(value);
        return xsputn(&character, 1) == 1 ? value : traits_type::eof();
    }

    pos_type seekoff(const off_type offset,
                     const std::ios_base::seekdir direction,
                     const std::ios_base::openmode mode) noexcept override
    {
        if (_sealed || _failed || (mode & std::ios_base::out) == 0) {
            return pos_type(off_type(-1));
        }
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        if (_fault == DocumentFileWriterTestFault::SerializationSeek) {
            latch("Injected serialization seek failure");
            return pos_type(off_type(-1));
        }
#endif
        NativeSeekOrigin origin = NativeSeekOrigin::Begin;
        if (direction == std::ios_base::cur) {
            origin = NativeSeekOrigin::Current;
        }
        else if (direction == std::ios_base::end) {
            origin = NativeSeekOrigin::End;
        }
        else if (direction != std::ios_base::beg) {
            return pos_type(off_type(-1));
        }
        try {
            const auto resulting = _file.seek(static_cast<std::int64_t>(offset), origin);
            return pos_type(static_cast<off_type>(resulting));
        }
        catch (...) {
            latchCurrent();
            return pos_type(off_type(-1));
        }
    }

    pos_type seekpos(const pos_type position,
                     const std::ios_base::openmode mode) noexcept override
    {
        return seekoff(static_cast<off_type>(position), std::ios_base::beg, mode);
    }

    int sync() noexcept override
    {
        if (_failed) {
            return -1;
        }
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        if (_fault == DocumentFileWriterTestFault::SerializationSync) {
            latch("Injected serialization sync failure");
            return -1;
        }
#endif
        try {
            _file.flush();
            return 0;
        }
        catch (...) {
            latchCurrent();
            return -1;
        }
    }

private:
    void latch(const char* message) noexcept
    {
        if (_failed) {
            return;
        }
        _failed = true;
        try {
            _error = std::make_exception_ptr(std::runtime_error(message));
        }
        catch (...) {
        }
    }

    void latchCurrent() noexcept
    {
        if (_failed) {
            return;
        }
        _failed = true;
        _error = std::current_exception();
    }

    [[nodiscard]] std::string errorMessage() const
    {
        if (!_error) {
            return "The retained serialization stream failed";
        }
        try {
            std::rethrow_exception(_error);
        }
        catch (const std::exception& exception) {
            return exception.what();
        }
        catch (...) {
            return "The retained serialization stream failed with an unknown error";
        }
    }

    NativeFile& _file;
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
    DocumentFileWriterTestFault _fault {DocumentFileWriterTestFault::None};
#endif
    std::exception_ptr _error;
    bool _failed {false};
    bool _sealed {false};
};

void addHashData(QCryptographicHash& hash, const char* data, const std::size_t count)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 3, 0)
    hash.addData(data, static_cast<int>(count));
#else
    hash.addData(QByteArrayView(data, static_cast<qsizetype>(count)));
#endif
}

struct StableHash
{
    std::string value;
    FileSnapshot snapshot;
};

std::pair<std::string, std::pair<FileSnapshot, FileSnapshot>> hashOnce(NativeFile& file)
{
    const auto before = file.snapshot();
    file.rewind();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // Heap, not stack: ioBufferSize is 1 MiB and the default thread stack on
    // Windows is 1 MiB, so a local array here overflows the stack before the
    // first read. Linux's 8 MiB default hid this on every previous run.
    std::vector<char> buffer(ioBufferSize);
    for (;;) {
        const auto count = file.read(buffer.data(), buffer.size());
        if (count == 0) {
            break;
        }
        addHashData(hash, buffer.data(), count);
    }
    const auto after = file.snapshot();
    return {hash.result().toHex().toStdString(), {before, after}};
}

StableHash stableHash(NativeFile& file)
{
    const auto first = hashOnce(file);
    const auto second = hashOnce(file);
    if (!sameObservedFile(first.second.first, first.second.second)
        || !sameObservedFile(first.second.second, second.second.first)
        || !sameObservedFile(second.second.first, second.second.second)
        || first.first != second.first) {
        throw std::runtime_error("File content changed while it was being validated");
    }
    return {second.first, second.second.second};
}

std::string copyAndHash(NativeFile& source, NativeFile& destination)
{
    source.rewind();
    destination.truncate();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // Heap, not stack -- see hashOnce(): a 1 MiB local array overflows the
    // 1 MiB default Windows thread stack.
    std::vector<char> buffer(ioBufferSize);
    for (;;) {
        const auto count = source.read(buffer.data(), buffer.size());
        if (count == 0) {
            break;
        }
        destination.write(buffer.data(), count);
        addHashData(hash, buffer.data(), count);
    }
    return hash.result().toHex().toStdString();
}

std::string normalizeSha256(std::string value)
{
    if (value.size() != 64
        || !std::ranges::all_of(value, [](const unsigned char character) {
               return std::isxdigit(character) != 0;
           })) {
        throw std::invalid_argument(
            "Expected destination SHA-256 must contain 64 hexadecimal characters");
    }
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string lockPathFor(const fs::path& path)
{
    const auto lockName =
        path.filename().native() + pathFromUtf8(".FreeCAD-save.lock").native();
    return pathToUtf8(path.parent_path() / lockName);
}

std::string processLockKey(const fs::path& path)
{
#ifdef FC_OS_WIN32
    auto key = path.native();
    std::ranges::transform(key, key.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    return pathToUtf8(fs::path(key));
#else
    return pathToUtf8(path);
#endif
}

std::shared_ptr<std::timed_mutex> processMutexFor(const std::string& key)
{
    static std::mutex registryMutex;
    static std::unordered_map<std::string, std::weak_ptr<std::timed_mutex>> registry;
    const std::scoped_lock guard(registryMutex);
    if (const auto existing = registry[key].lock()) {
        return existing;
    }
    auto created = std::make_shared<std::timed_mutex>();
    registry[key] = created;
    return created;
}

#ifndef FC_OS_WIN32
enum class NativeNoReplaceResult
{
    Installed,
    Unsupported,
    Failed,
};

NativeNoReplaceResult renameNoReplace(const int parent,
                                      const fs::path& sourceLeaf,
                                      const fs::path& destinationLeaf,
                                      std::error_code& error)
{
# if defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned int renameNoReplaceFlag = 1U;
    if (::syscall(SYS_renameat2,
                  parent,
                  sourceLeaf.c_str(),
                  parent,
                  destinationLeaf.c_str(),
                  renameNoReplaceFlag)
        == 0) {
        return NativeNoReplaceResult::Installed;
    }
    const int status = errno;
    if (status != ENOSYS && status != EINVAL && status != EOPNOTSUPP) {
        error = std::error_code(status, std::generic_category());
        return NativeNoReplaceResult::Failed;
    }
# elif defined(__APPLE__) && defined(RENAME_EXCL)
    if (::renameatx_np(parent,
                       sourceLeaf.c_str(),
                       parent,
                       destinationLeaf.c_str(),
                       RENAME_EXCL)
        == 0) {
        return NativeNoReplaceResult::Installed;
    }
    const int status = errno;
    if (status != ENOTSUP && status != EINVAL) {
        error = std::error_code(status, std::generic_category());
        return NativeNoReplaceResult::Failed;
    }
# else
    (void)parent;
    (void)sourceLeaf;
    (void)destinationLeaf;
    (void)error;
# endif
    return NativeNoReplaceResult::Unsupported;
}
#endif

enum class StrictNoReplaceResult
{
    Installed,
    DestinationExists,
    SourceChanged,
    Unsupported,
    Failed,
};

StrictNoReplaceResult moveOwnedFileNoReplaceStrict(NativeFile& source,
                                                   PinnedDirectory& parent,
                                                   const fs::path& destination,
                                                   std::error_code& error
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                                                   , const std::function<void(
                                                       const std::string&,
                                                       const std::string&)>&
                                                       afterSourceValidation = {}
#endif
)
{
    error.clear();
#ifdef FC_OS_WIN32
# if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
    (void)afterSourceValidation;
# endif
    // Relative rename through the pinned parent with no replace authority.
    // This is the strict exact-handle contract; do not fall back to any
    // replace-capable primitive here.
    const std::wstring leaf = destination.filename().native();
    const auto renamed =
        ntRenameThroughParent(source.handle(), parent.handle(), leaf, false, false);
    if (renamed.outcome == NtRenameOutcome::Renamed) {
        return StrictNoReplaceResult::Installed;
    }
    error = renamed.error;
    if (renamed.outcome == NtRenameOutcome::DestinationExists) {
        return StrictNoReplaceResult::DestinationExists;
    }
    if (renamed.outcome == NtRenameOutcome::Unsupported) {
        return StrictNoReplaceResult::Unsupported;
    }
    return StrictNoReplaceResult::Failed;
#else
    const auto sourceLeaf = source.path().filename();
    struct stat sourceAtBoundary {};
    if (::fstatat(parent.handle(),
                  sourceLeaf.c_str(),
                  &sourceAtBoundary,
                  AT_SYMLINK_NOFOLLOW)
            != 0
        || !S_ISREG(sourceAtBoundary.st_mode)
        || posixIdentity(sourceAtBoundary) != source.identity()) {
        error = std::error_code(ESTALE, std::generic_category());
        return StrictNoReplaceResult::Failed;
    }
# if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
    if (afterSourceValidation) {
        afterSourceValidation(source.pathUtf8(), pathToUtf8(destination));
    }
# endif
    const auto renamed = renameNoReplace(parent.handle(),
                                         sourceLeaf,
                                         destination.filename(),
                                         error);
    if (renamed == NativeNoReplaceResult::Installed) {
        // POSIX strict no-replace renames are pathname-based. A source leaf
        // can be exchanged after the pre-syscall fstatat proof. Treat the
        // primitive as installed only when its destination now names the
        // exact retained source handle; a substituted entry is preserved at
        // the new name and reported as a closed, non-successful CAS.
        struct stat installedAtBoundary {};
        if (::fstatat(parent.handle(),
                      destination.filename().c_str(),
                      &installedAtBoundary,
                      AT_SYMLINK_NOFOLLOW)
                == 0
            && S_ISREG(installedAtBoundary.st_mode)
            && posixIdentity(installedAtBoundary) == source.identity()) {
            return StrictNoReplaceResult::Installed;
        }
        error = std::error_code(ESTALE, std::generic_category());
        return StrictNoReplaceResult::SourceChanged;
    }
    if (renamed == NativeNoReplaceResult::Unsupported) {
        return StrictNoReplaceResult::Unsupported;
    }
    if (error.value() == EEXIST) {
        return StrictNoReplaceResult::DestinationExists;
    }
    return StrictNoReplaceResult::Failed;
#endif
}

struct AbsentSiblingPath
{
    fs::path path;
    std::string utf8;
};

AbsentSiblingPath reserveAbsentSiblingPath(
    const std::shared_ptr<PinnedDirectory>& parent,
    const fs::path& destination,
    const std::string_view suffix)
{
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto name = destination.filename().native()
            + pathFromUtf8("." + Base::Uuid::createUuid() + std::string(suffix)).native();
        AbsentSiblingPath result {destination.parent_path() / name, {}};
        result.utf8 = pathToUtf8(result.path);
        // This is deliberately only an absent-name candidate, not a create /
        // unlink reservation. The strict no-replace move is the authority at
        // the mutation boundary and safely reports a collision. Avoiding a
        // reservation also avoids an unsafe POSIX pathname cleanup.
        if (parent->pathStillOwned() && !inspectPath(result.path, false).exists) {
            return result;
        }
    }
    throw std::runtime_error(
        "Unable to reserve an absent compare-and-swap recovery guard");
}

bool replaceOwnedFile(NativeFile& temporary,
                      PinnedDirectory& parent,
                      const fs::path& destination,
                      const DocumentFileReplacementMode mode,
                      std::error_code& error,
                      bool& sourceConsumed,
                      const bool ignoreDestinationReadOnly)
{
    sourceConsumed = false;
#ifdef FC_OS_WIN32
    const std::wstring leaf = destination.filename().native();
    const bool replaceIfExists = mode != DocumentFileReplacementMode::NoReplace;

    // Prefer the extended class: it carries POSIX semantics, so a replaced
    // predecessor is unlinked from the namespace immediately rather than
    // lingering until every handle on it closes. It is also the only class
    // that honours the read-only override; the classic class silently ignores
    // the flag, so a read-only destination must never fall through to it.
    if (replaceIfExists) {
        const auto extended = ntRenameThroughParent(temporary.handle(),
                                                    parent.handle(),
                                                    leaf,
                                                    true,
                                                    true,
                                                    ignoreDestinationReadOnly);
        if (extended.outcome == NtRenameOutcome::Renamed) {
            sourceConsumed = true;
            return true;
        }
        if (extended.outcome != NtRenameOutcome::Unsupported) {
            error = extended.error;
            return false;
        }
        if (ignoreDestinationReadOnly) {
            error = extended.error;
            return false;
        }
    }

    const auto classic =
        ntRenameThroughParent(temporary.handle(), parent.handle(), leaf, replaceIfExists, false);
    if (classic.outcome == NtRenameOutcome::Renamed) {
        sourceConsumed = true;
        return true;
    }
    error = classic.error;
    return false;
#else
    const auto temporaryLeaf = temporary.path().filename();
    const auto destinationLeaf = destination.filename();
    struct stat sourceAtBoundary {};
    if (::fstatat(parent.handle(),
                  temporaryLeaf.c_str(),
                  &sourceAtBoundary,
                  AT_SYMLINK_NOFOLLOW)
            != 0
        || !S_ISREG(sourceAtBoundary.st_mode)
        || posixIdentity(sourceAtBoundary) != temporary.identity()) {
        error = std::error_code(ESTALE, std::generic_category());
        return false;
    }
    if (mode == DocumentFileReplacementMode::NoReplace) {
        const auto renamed =
            renameNoReplace(parent.handle(), temporaryLeaf, destinationLeaf, error);
        if (renamed == NativeNoReplaceResult::Installed) {
            sourceConsumed = true;
            return true;
        }
        if (renamed == NativeNoReplaceResult::Failed) {
            return false;
        }
        if (::linkat(parent.handle(),
                     temporaryLeaf.c_str(),
                     parent.handle(),
                     destinationLeaf.c_str(),
                     0)
            != 0) {
            error = std::error_code(errno, std::generic_category());
            return false;
        }
        // linkat resolves its source by pathname, so a leaf substituted after
        // the pre-boundary proof could have installed a foreign inode under
        // the destination. Only report an installation once the destination
        // proves to name the exact retained handle.
        struct stat installedAtBoundary {};
        if (::fstatat(parent.handle(),
                      destinationLeaf.c_str(),
                      &installedAtBoundary,
                      AT_SYMLINK_NOFOLLOW)
                != 0
            || !S_ISREG(installedAtBoundary.st_mode)
            || posixIdentity(installedAtBoundary) != temporary.identity()) {
            error = std::error_code(ESTALE, std::generic_category());
            return false;
        }
        // The portable link fallback cannot consume the source name. POSIX has
        // no inode-conditional unlink, and every fstatat(path) -> unlinkat(path)
        // sequence has a substitution window in which the unlink would delete a
        // foreign entry; another userspace identity check cannot close it. See
        // discardExact(): leave the proved owned name in place and report it
        // unconsumed so the caller surfaces it as recovery evidence.
        return true;
    }
    if (::renameat(parent.handle(),
                   temporaryLeaf.c_str(),
                   parent.handle(),
                   destinationLeaf.c_str())
        == 0) {
        sourceConsumed = true;
        return true;
    }
    error = std::error_code(errno, std::generic_category());
    return false;
#endif
}

bool isDestinationExistsError(const std::error_code& error)
{
#ifdef FC_OS_WIN32
    return error.value() == ERROR_FILE_EXISTS || error.value() == ERROR_ALREADY_EXISTS;
#else
    return error.value() == EEXIST;
#endif
}

}  // namespace

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
DocumentFileWriterRequestDecoratorGuard setDocumentFileWriterRequestDecoratorForTesting(
    DocumentFileWriterRequestDecorator decorator)
{
    if (!decorator) {
        return {};
    }
    auto registration = std::make_shared<RequestDecoratorRegistration>();
    auto& registry = requestDecoratorRegistry();
    {
        const std::scoped_lock lock(registry.mutex);
        registration->id = registry.nextId++;
        registry.entries.push_back({registration->id, std::move(decorator)});
    }
    return registration;
}
#endif

DisplacedFileLeaseOperationResult installDisplacedFileLeaseNoReplace(
    const std::shared_ptr<void>& lease,
    const std::string& expectedSource,
    const std::string& destination
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
    ,
    const DisplacedFileLeaseCheckpointHook& checkpointHook,
    const bool forcePortableLinkFallback
#endif
)
{
    DisplacedFileLeaseOperationResult result;
    const auto fail = [&](std::string message) {
        result.error = std::move(message);
        return result;
    };
    try {
        if (!lease || expectedSource.empty() || destination.empty()) {
            return fail("A retained displaced-file lease and both paths are required");
        }
        const auto owned = std::static_pointer_cast<NativeFile>(lease);
        const auto source = normalizeDiagnosticFinalEntry(expectedSource);
        const auto target = normalizeFinalEntry(destination, false);
        if (!platformPathsEqual(source, owned->path())) {
            return fail("The displaced-file lease does not match the asserted source path");
        }
        if (platformPathsEqual(source, target)) {
            return fail("The displaced snapshot and backup destination are the same path");
        }
        if (!platformPathsEqual(target.parent_path(), owned->parent().path())) {
            return fail("The backup destination is outside the displaced snapshot's pinned directory");
        }
        if (!owned->parentPathStillOwned()) {
            return fail("The displaced snapshot directory changed before backup installation");
        }

        const auto targetBefore = inspectPath(target, false);
        if (targetBefore.exists && targetBefore.identity == owned->identity()) {
            return fail("The backup destination aliases the displaced snapshot");
        }
        if (targetBefore.exists) {
            result.destinationExists = true;
            return fail("The backup destination already exists");
        }

        owned->flush();
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        const auto invokeCheckpoint = [&](const DisplacedFileLeaseCheckpoint checkpoint) {
            if (!checkpointHook) {
                return true;
            }
            try {
                checkpointHook(checkpoint);
                return true;
            }
            catch (const std::exception& exception) {
                result.error = exception.what();
            }
            catch (...) {
                result.error = "Unknown injected displaced-file checkpoint failure";
            }
            return false;
        };
        if (!invokeCheckpoint(DisplacedFileLeaseCheckpoint::BeforeNamespaceInstall)) {
            return result;
        }
#endif
        if (!owned->parentPathStillOwned()) {
            return fail("The displaced snapshot directory changed at the backup boundary");
        }

#ifdef FC_OS_WIN32
        // The source HANDLE, not its diagnostic pathname, is authoritative.
        // A concurrent source-name swap therefore cannot redirect the move.
        std::error_code replacementError;
        bool sourceConsumed = false;
        if (!replaceOwnedFile(*owned,
                              owned->parent(),
                              target,
                              DocumentFileReplacementMode::NoReplace,
                              replacementError,
                              sourceConsumed,
                              // A backup install never replaces anything, so no
                              // destination read-only attribute is in play.
                              false)) {
            result.destinationExists = isDestinationExistsError(replacementError);
            return fail(systemMessage(replacementError));
        }
        if (!sourceConsumed) {
            return fail("The retained displaced-file source was not consumed by handle rename");
        }
        owned->markTransferred(target);
        result.installed = true;
        result.sourceConsumed = sourceConsumed;
        try {
            owned->flush();
        }
        catch (const std::exception& exception) {
            result.error = exception.what();
            return result;
        }
        const auto installed = inspectPath(target, false);
        if (!installed.exists || !installed.regular
            || installed.identity != owned->identity()) {
            result.installed = false;
            return fail("The installed backup no longer names the retained displaced file");
        }
        result.durabilityVerified = true;
        // The lease handle now names installed backup history, which is a
        // user-visible file: it must read like any other file on disk. Its
        // DELETE access has done its work -- the exact move is complete and
        // verified -- and keeping it open would leave every ordinary reader on
        // Windows facing ERROR_SHARING_VIOLATION. POSIX needs no equivalent:
        // an open descriptor there denies nobody.
        owned->releaseDescriptor();
        return result;
#else
        if (!owned->pinnedPathStillOwned()) {
            return fail("The displaced snapshot path changed at the backup boundary");
        }
        const auto sourceLeaf = owned->path().filename();
        const auto destinationLeaf = target.filename();
# if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        const bool usePortableFallback = forcePortableLinkFallback;
# else
        constexpr bool usePortableFallback = false;
# endif
        if (!usePortableFallback) {
            std::error_code renameError;
            const auto renamed = renameNoReplace(owned->parent().handle(),
                                                 sourceLeaf,
                                                 destinationLeaf,
                                                 renameError);
            if (renamed == NativeNoReplaceResult::Installed) {
                owned->markTransferred(target);
                result.installed = true;
                result.sourceConsumed = true;
                try {
                    owned->parent().flush();
                    result.durabilityVerified = true;
                }
                catch (const std::exception& exception) {
                    result.error = exception.what();
                }
                return result;
            }
            if (renamed == NativeNoReplaceResult::Failed) {
                result.destinationExists = isDestinationExistsError(renameError);
                return fail(systemMessage(renameError));
            }
        }

        if (::linkat(owned->parent().handle(),
                     sourceLeaf.c_str(),
                     owned->parent().handle(),
                     destinationLeaf.c_str(),
                     0)
            != 0) {
            const std::error_code linkError(errno, std::generic_category());
            result.destinationExists = isDestinationExistsError(linkError);
            return fail(systemMessage(linkError));
        }
        // linkat resolves its source by pathname, so a leaf substituted after
        // the pre-boundary proof could have linked a foreign inode under the
        // backup name. Prove the new entry names the retained displaced file
        // before reporting an installation at all.
        struct stat installedAtBoundary {};
        if (::fstatat(owned->parent().handle(),
                      destinationLeaf.c_str(),
                      &installedAtBoundary,
                      AT_SYMLINK_NOFOLLOW)
                != 0
            || !S_ISREG(installedAtBoundary.st_mode)
            || posixIdentity(installedAtBoundary) != owned->identity()) {
            return fail(
                "The linked backup destination does not name the retained displaced file");
        }
        result.installed = true;

# if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        if (!invokeCheckpoint(DisplacedFileLeaseCheckpoint::AfterLinkBeforeDirectoryFlush)) {
            return result;
        }
# endif
        try {
            owned->parent().flush();
        }
        catch (const std::exception& exception) {
            result.error = exception.what();
            return result;
        }
        if (!owned->parentPathStillOwned()) {
            result.installed = false;
            result.error = "The backup directory changed at the durability boundary";
            return result;
        }
        struct stat destinationAfterDurabilityFlush {};
        if (::fstatat(owned->parent().handle(),
                      destinationLeaf.c_str(),
                      &destinationAfterDurabilityFlush,
                      AT_SYMLINK_NOFOLLOW)
                != 0
            || !S_ISREG(destinationAfterDurabilityFlush.st_mode)
            || posixIdentity(destinationAfterDurabilityFlush) != owned->identity()) {
            result.installed = false;
            result.durabilityVerified = false;
            result.error =
                "The durable backup destination no longer names the retained displaced file";
            return result;
        }
        result.durabilityVerified = true;
        // The portable link fallback cannot consume the displaced source name.
        // POSIX has no inode-conditional unlink, and every fstatat(path) ->
        // unlinkat(path) sequence has a substitution window in which the unlink
        // would delete a foreign entry; another userspace identity check cannot
        // close it. Keep the proved owned name and report it unconsumed so the
        // caller surfaces it as recoverable maintenance evidence and never
        // prunes known-good history on this path.
        result.sourceConsumed = false;
        result.error =
            "this filesystem provides no strict no-replace rename, and POSIX cannot remove "
            "the displaced name conditionally on its retained identity";
        return result;
#endif
    }
    catch (const std::exception& exception) {
        return fail(exception.what());
    }
    catch (...) {
        return fail("Unknown displaced-file lease installation failure");
    }
}

DisplacedFileLeaseOperationResult discardDisplacedFileLease(
    const std::shared_ptr<void>& lease,
    const std::string& expectedSource
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
    ,
    const std::function<void(const std::string&)>& beforePosixFailClosedDecision
#endif
)
{
    DisplacedFileLeaseOperationResult result;
    const auto fail = [&](std::string message) {
        result.error = std::move(message);
        return result;
    };
    try {
        if (!lease || expectedSource.empty()) {
            return fail("A retained displaced-file lease and source path are required");
        }
        const auto owned = std::static_pointer_cast<NativeFile>(lease);
        const auto source = normalizeDiagnosticFinalEntry(expectedSource);
        if (!platformPathsEqual(source, owned->path())) {
            return fail("The displaced-file lease does not match the asserted source path");
        }
        const std::string ownedIdentity = owned->identity();
        owned->flush();
#ifndef FC_OS_WIN32
        if (!owned->parentPathStillOwned() || !owned->pinnedPathStillOwned()) {
            return fail("The displaced snapshot path changed before exact discard");
        }
# if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        if (beforePosixFailClosedDecision) {
            beforePosixFailClosedDecision(owned->pathUtf8());
        }
# endif
#endif
        // Contract R5: an explicit numberOfFiles == 0 retention decision is the
        // only authority that may remove a DisplacedCanonical, and it uses its
        // own entry point so generic cleanup can never reach this.
        if (!owned->discardDisplacedCanonicalExact()) {
#ifdef FC_OS_WIN32
            return fail("Unable to discard the exact displaced snapshot");
#else
            return fail(
                "The exact displaced snapshot was retained: its pathname no longer proves to "
                "name the retained snapshot, so the discard failed closed");
#endif
        }
        result.sourceConsumed = true;
#ifdef FC_OS_WIN32
        // SetFileInformationByHandle + close consumes this retained lease, but
        // legacy disposition may remain delete-pending while another shared
        // handle is open. Even when the no-follow namespace entry is already
        // absent, Windows exposes no directory-fsync primitive with which to
        // prove that absence durable. Never promote either case to durable.
        result.durabilityVerified = false;
        try {
            const auto afterClose = inspectPath(source, false);
            if (!afterClose.exists) {
                result.error =
                    "The exact displaced snapshot was removed, but Windows directory "
                    "namespace durability cannot be proven";
            }
            else if (afterClose.identity == ownedIdentity) {
                result.error =
                    "The exact displaced snapshot delete was accepted but remains pending; "
                    "Windows directory namespace durability cannot be proven";
            }
            else {
                result.error =
                    "The exact displaced snapshot was consumed and its asserted path now names "
                    "a foreign entry; Windows directory namespace durability cannot be proven";
            }
        }
        catch (const std::exception& exception) {
            result.error =
                "The exact displaced snapshot delete was accepted, but post-close namespace "
                "absence and durability cannot be proven: "
                + std::string(exception.what());
        }
        return result;
#else
        try {
            owned->parent().flush();
            result.durabilityVerified = true;
        }
        catch (const std::exception& exception) {
            result.error = exception.what();
        }
        return result;
#endif
    }
    catch (const std::exception& exception) {
        return fail(exception.what());
    }
    catch (...) {
        return fail("Unknown displaced-file lease discard failure");
    }
}

struct DocumentFileLock::Impl
{
    Impl(const std::string& destination, const int timeoutMs)
        : normalized(normalizeFinalEntry(destination, false))
        , processMutex(processMutexFor(processLockKey(normalized)))
        , processLock(*processMutex, std::defer_lock)
        , filesystemLock(lockPathFor(normalized))
    {
        bool processLocked = false;
        if (timeoutMs < 0) {
            processLock.lock();
            processLocked = true;
        }
        else if (timeoutMs == 0) {
            processLocked = processLock.try_lock();
        }
        else {
            processLocked = processLock.try_lock_for(std::chrono::milliseconds(timeoutMs));
        }
        if (!processLocked) {
            return;
        }
        if (!filesystemLock.tryLock(timeoutMs)) {
            processLock.unlock();
            return;
        }
        locked = true;
    }

    fs::path normalized;
    std::shared_ptr<std::timed_mutex> processMutex;
    std::unique_lock<std::timed_mutex> processLock;
    Base::FileLock filesystemLock;
    bool locked {false};
};

DocumentFileLock::DocumentFileLock(const std::string& destination, const int timeoutMs)
    : _impl(std::make_unique<Impl>(destination, timeoutMs))
{}

DocumentFileLock::~DocumentFileLock() = default;

bool DocumentFileLock::isLocked() const noexcept
{
    return _impl && _impl->locked;
}

struct DocumentFileWriter::Impl
{
    explicit Impl(DocumentFileReplacementRequest value)
        : request(std::move(value))
        , destination(normalizeDestination(request))
        , forbidden(request.forbiddenAliasPath.empty()
                        ? std::nullopt
                        : std::make_optional(
                              normalizeFinalEntry(request.forbiddenAliasPath, false)))
        , parent(std::make_shared<PinnedDirectory>(destination.parent_path()))
        , temporary(NativeFile::createSibling(parent, destination, ".tmp"))
        , serializationBuffer(
              *temporary
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
              ,
              request.testFault
#endif
              )
        , serialization(&serializationBuffer)
        , destinationUtf8(pathToUtf8(destination))
        , temporaryUtf8(temporary->pathUtf8())
    {
        // The serialization temporary is the only EphemeralPartial in a save.
        // Nothing else created by this writer may be marked.
        temporary->markEphemeralPartial();
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API) && defined(FC_OS_WIN32)
        if (request.testFault
            == DocumentFileWriterTestFault::CleanupReadOnlyAndRestrictedDacl) {
            temporary->makeCleanupHostileForTesting();
        }
#endif
    }

    DocumentFileReplacementRequest request;
    fs::path destination;
    std::optional<fs::path> forbidden;
    std::shared_ptr<PinnedDirectory> parent;
    std::shared_ptr<NativeFile> temporary;
    RetainedHandleStreamBuffer serializationBuffer;
    std::ostream serialization;
    std::string destinationUtf8;
    std::string temporaryUtf8;
    std::shared_ptr<NativeFile> destinationSource;
    std::shared_ptr<NativeFile> displaced;
    std::optional<StableHash> displacedSourceHash;
    bool commitAttempted {false};
};

bool DocumentFileReplacementResult::retainDisplacedFileForRecovery() noexcept
{
    try {
        if (!displacedFileLease || displacedFile.empty()) {
            return false;
        }
        const auto owned = std::static_pointer_cast<NativeFile>(displacedFileLease);
        if (!owned->pathStillOwned() || owned->pathUtf8() != displacedFile) {
            return false;
        }
        owned->relinquishPathCleanup();
        // Publishing the recovery evidence has to end the lease outright, not
        // merely drop this pointer to it. On Windows the retained handle holds
        // DELETE, and while it is open no ordinary reader can open the file;
        // the caller who was just told the evidence is theirs would find it
        // unreadable. Whether some other reference to the lease survives is not
        // something this transition may depend on.
        owned->releaseDescriptor();
        displacedFileLease.reset();
        return true;
    }
    catch (...) {
        return false;
    }
}

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
DocumentFileWriter::DocumentFileWriter(DocumentFileReplacementRequest request)
{
    decorateRequestForTesting(request);
    _impl = std::make_unique<Impl>(std::move(request));
}
#else
DocumentFileWriter::DocumentFileWriter(DocumentFileReplacementRequest request)
    : _impl(std::make_unique<Impl>(std::move(request)))
{}
#endif

DocumentFileWriter::~DocumentFileWriter() = default;
DocumentFileWriter::DocumentFileWriter(DocumentFileWriter&&) noexcept = default;
DocumentFileWriter& DocumentFileWriter::operator=(DocumentFileWriter&&) noexcept = default;

const std::string& DocumentFileWriter::destinationPath() const noexcept
{
    return _impl->destinationUtf8;
}

std::ostream& DocumentFileWriter::serializationStream() noexcept
{
    return _impl->serialization;
}

const std::string& DocumentFileWriter::temporaryPath() const noexcept
{
    return _impl->temporaryUtf8;
}

DocumentFileReplacementResult DocumentFileWriter::commit()
{
    DocumentFileReplacementResult result;
    result.destination = _impl->destinationUtf8;
    const auto fail = [&](std::string code, std::string message) {
        // Contract §2.2: the serialized bytes were verified before the
        // replacement boundary, so they are a VerifiedSerialization and are
        // never removed by cleanup. If no install consumed them they are the
        // only copy of the work being saved, so report them as recovery
        // evidence. Specific partial-CAS paths may already have published the
        // same evidence, so avoid duplicating their diagnostic.
        try {
            if (!result.fileWritten && _impl->temporary
                && _impl->temporary->pinnedPathStillOwned()) {
                _impl->temporary->relinquishPathCleanup();
                const auto& temporary = _impl->temporary->pathUtf8();
                const bool alreadyReported = std::any_of(
                    result.warnings.begin(),
                    result.warnings.end(),
                    [&](const std::string& warning) {
                        return warning.find(temporary) != std::string::npos;
                    });
                if (!alreadyReported) {
                    result.warnings.push_back(
                        "The save did not complete, so the verified serialized document is "
                        "retained as recovery evidence at '"
                        + temporary
                        + "'. It contains the bytes that were being saved and is safe to "
                          "delete once you no longer need them.");
                }
            }
        }
        catch (...) {
            // The namespace was never touched by this best-effort diagnostic;
            // destructor cleanup remains fail-closed even if reporting fails.
        }
        result.errorCode = std::move(code);
        result.message = std::move(message);
        // A compare-and-swap opens the destination with DELETE so it can move
        // the exact file it validated. If the handle still names the canonical
        // path here, that move never happened: the save failed and the user's
        // document is untouched at its own name. Keeping DELETE on it past that
        // point blocks every ordinary reader of a file this writer decided not
        // to change. Never released when it is the reported recovery lease --
        // that artifact is still internal and still needs its authority.
        if (_impl->destinationSource
            && static_cast<const void*>(_impl->destinationSource.get())
                != result.displacedFileLease.get()
            && _impl->destinationSource->pathUtf8() == _impl->destinationUtf8) {
            _impl->destinationSource->releaseDescriptor();
        }
        if (_impl->request.mode
                == DocumentFileReplacementMode::CompareAndSwapSha256
            && result.displacedFileLease) {
            // A CAS that crossed either namespace boundary must leave the
            // exact previous version recoverable even if the caller drops the
            // structured failure without an explicit retention handoff.
            std::static_pointer_cast<NativeFile>(result.displacedFileLease)
                ->relinquishPathCleanup();
        }
        return DocumentFileReplacementResult(std::move(result));
    };

    try {
        if (_impl->commitAttempted) {
            return fail("REPLACEMENT_ALREADY_ATTEMPTED",
                        "This serialized file has already had a replacement attempt");
        }
        _impl->commitAttempted = true;

        std::string serializationError;
        if (!_impl->serializationBuffer.seal(serializationError)) {
            return fail("SERIALIZATION_IO_FAILED",
                        "Serialization through the retained file handle failed: "
                            + serializationError);
        }
        if (!_impl->parent->pathStillOwned()) {
            return fail("DESTINATION_DIRECTORY_CHANGED",
                        "The destination directory changed before replacement");
        }
        if (!_impl->temporary->pathStillOwned()) {
            return fail("TEMPORARY_FILE_CHANGED",
                        "The serialized temporary path no longer names the reserved file");
        }

        DocumentFileLock destinationLock(_impl->destinationUtf8,
                                         _impl->request.lockTimeoutMs);
        if (!destinationLock.isLocked()) {
            return fail("DESTINATION_BUSY", "Another FreeCAD writer owns the destination lock");
        }

        const auto destinationBefore = inspectPath(_impl->destination, false);
        if (_impl->request.mode == DocumentFileReplacementMode::NoReplace
            && destinationBefore.exists) {
            return fail("DESTINATION_EXISTS", "The destination already exists");
        }

        if (destinationBefore.link) {
            if (_impl->forbidden) {
                const auto followed = inspectPath(_impl->destination, true);
                const auto forbidden = inspectPath(*_impl->forbidden, true);
                if (followed.exists && forbidden.exists
                    && followed.identity == forbidden.identity) {
                    return fail("DESTINATION_ALIASES_FORBIDDEN_FILE",
                                "The destination aliases the protected canonical file");
                }
            }
            return fail("DESTINATION_LINK_FORBIDDEN",
                        "Symbolic-link and reparse-point destinations are not allowed");
        }
        if (destinationBefore.exists && !destinationBefore.regular) {
            return fail("DESTINATION_NOT_REGULAR", "The destination is not a regular file");
        }

        if (_impl->forbidden) {
            if (platformPathsEqual(_impl->destination, *_impl->forbidden)) {
                return fail("DESTINATION_ALIASES_FORBIDDEN_FILE",
                            "The destination is the protected canonical file");
            }
            const auto forbidden = inspectPath(*_impl->forbidden, true);
            if (destinationBefore.exists && forbidden.exists
                && destinationBefore.identity == forbidden.identity) {
                return fail("DESTINATION_ALIASES_FORBIDDEN_FILE",
                            "The destination aliases the protected canonical file");
            }
        }

        if (destinationBefore.exists) {
            const bool compareAndSwap = _impl->request.mode
                == DocumentFileReplacementMode::CompareAndSwapSha256;
            try {
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                if (compareAndSwap
                    && _impl->request.testFault
                        == DocumentFileWriterTestFault::CompareAndSwapAuthorityUnavailable) {
                    throw std::system_error(
# ifdef FC_OS_WIN32
                        std::error_code(ERROR_ACCESS_DENIED, std::system_category()),
# else
                        std::error_code(EACCES, std::generic_category()),
# endif
                        "Injected retained destination authority failure");
                }
#endif
                _impl->destinationSource = NativeFile::openRegularNoFollow(
                    _impl->destination, _impl->parent, compareAndSwap);
            }
            catch (const std::exception& exception) {
                if (compareAndSwap) {
                    return fail(
                        "COMPARE_AND_SWAP_AUTHORITY_UNAVAILABLE",
                        "Unable to retain the destination with exact-rename and durability "
                        "authority before compare-and-swap: "
                            + std::string(exception.what()));
                }
                throw;
            }
            const auto opened = _impl->destinationSource->snapshot();
            if (!sameObservedFile(destinationBefore, opened)) {
                return fail("DESTINATION_CHANGED",
                            "The destination changed while it was opened for validation");
            }
            try {
                _impl->temporary->preserveMetadataFrom(*_impl->destinationSource, false);
                _impl->temporary->flush();
            }
            catch (const MetadataUnsupportedError& exception) {
                return fail("METADATA_UNSUPPORTED", exception.what());
            }
            catch (const std::exception& exception) {
                return fail("METADATA_PRESERVATION_FAILED", exception.what());
            }
        }
        // Capture the serialized baseline only after applying verified
        // destination metadata, so later mode/attribute changes are part of
        // the final-boundary identity check.
        const auto serializedHash = stableHash(*_impl->temporary);
        // Contract §2.2 / Q1: these bytes are now verified and are the only
        // copy of the work being saved. Promote out of EphemeralPartial so no
        // cleanup path can remove them; from here they are either consumed by
        // the install primitive or retained and reported as recovery evidence.
        _impl->temporary->markVerifiedSerialization();

        std::optional<std::string> expectedDestinationSha256;
        std::optional<StableHash> destinationHashBefore;
        if (_impl->request.mode == DocumentFileReplacementMode::CompareAndSwapSha256) {
            if (!destinationBefore.exists) {
                return fail("DESTINATION_MISSING", "The expected destination no longer exists");
            }
            expectedDestinationSha256 =
                normalizeSha256(_impl->request.expectedDestinationSha256);
            destinationHashBefore = stableHash(*_impl->destinationSource);
            if (destinationHashBefore->value != *expectedDestinationSha256
                || !sameObservedFile(destinationBefore, destinationHashBefore->snapshot)
                || !_impl->destinationSource->pathStillOwned()) {
                return fail("DESTINATION_CHANGED",
                            "The destination SHA-256 does not match the expected value");
            }
        }

        if (_impl->request.preserveDisplacedFile && destinationBefore.exists
            && _impl->request.mode
                != DocumentFileReplacementMode::CompareAndSwapSha256) {
            try {
                _impl->displaced = NativeFile::createSibling(
                    _impl->parent,
                    _impl->destination,
                    ".displaced"
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                    ,
                    _impl->request.beforeDisplacedReservationAttempt
#endif
                );
                // Until its content is proved, the snapshot is an unpublished
                // partial copy and the previous file is still intact at the
                // canonical name, so it is cleanup-eligible. It is promoted to
                // a DisplacedCanonical only once verified.
                _impl->displaced->markEphemeralPartial();
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API) && defined(FC_OS_WIN32)
                if (_impl->request.testFault
                    == DocumentFileWriterTestFault::LegacyDisplacedDiscardDeletePending) {
                    _impl->displaced->forceLegacyDeleteDispositionForTesting();
                }
#endif
                const auto sourceBefore = stableHash(*_impl->destinationSource);
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                if (_impl->request.afterDisplacedSourceHashBeforeCopy) {
                    _impl->request.afterDisplacedSourceHashBeforeCopy();
                }
#endif
                const auto copiedHash =
                    copyAndHash(*_impl->destinationSource, *_impl->displaced);
                const auto sourceAfter = stableHash(*_impl->destinationSource);
                _impl->displaced->preserveMetadataFrom(*_impl->destinationSource, true);
                _impl->displaced->flush();
                const auto snapshotHash = stableHash(*_impl->displaced);
                if (copiedHash != sourceBefore.value || sourceAfter.value != sourceBefore.value
                    || snapshotHash.value != sourceBefore.value
                    || !samePersistentFileState(sourceAfter.snapshot, snapshotHash.snapshot)
                    || !sameObservedFile(sourceBefore.snapshot, sourceAfter.snapshot)
                    || !_impl->destinationSource->pathStillOwned()
                    || !_impl->displaced->pathStillOwned()) {
                    throw std::runtime_error(
                        "The previous file changed while its exact backup snapshot was created");
                }
                // Verified: this is now a DisplacedCanonical holding a real
                // version of the user's document, and cleanup may never remove
                // it again.
                _impl->displaced->promoteToDisplacedCanonical();
                _impl->displacedSourceHash = sourceAfter;
            }
            catch (const std::exception& exception) {
                result.warnings.push_back(
                    "Unable to preserve an exact previous-file snapshot: "
                    + std::string(exception.what()));
                if (_impl->displaced && !_impl->displaced->discardExact()) {
                    result.warnings.push_back(
                        "The abandoned previous-file snapshot could not be removed and remains "
                        "at '"
                        + _impl->displaced->pathUtf8() + "'.");
                }
                _impl->displaced.reset();
                _impl->displacedSourceHash.reset();
            }
        }

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        if (_impl->request.beforeFinalBoundaryValidation) {
            _impl->request.beforeFinalBoundaryValidation();
        }
#endif

        const auto destinationAtBoundary = inspectPath(_impl->destination, false);
        if (_impl->request.mode == DocumentFileReplacementMode::NoReplace
            && destinationAtBoundary.exists) {
            return fail("DESTINATION_EXISTS", "The destination was created before replacement");
        }

        // Followed-alias detection must precede the generic link error so a
        // Save Copy race is classified as canonical-alias protection.
        if (destinationAtBoundary.link && _impl->forbidden) {
            const auto followed = inspectPath(_impl->destination, true);
            const auto forbidden = inspectPath(*_impl->forbidden, true);
            if (followed.exists && forbidden.exists
                && followed.identity == forbidden.identity) {
                return fail("DESTINATION_ALIASES_FORBIDDEN_FILE",
                            "The destination aliases the protected canonical file");
            }
        }
        if (destinationAtBoundary.link) {
            return fail("DESTINATION_LINK_FORBIDDEN",
                        "The destination became a symbolic link or reparse point");
        }
        if (destinationAtBoundary.exists && !destinationAtBoundary.regular) {
            return fail("DESTINATION_NOT_REGULAR", "The destination is not a regular file");
        }
        if (_impl->forbidden && destinationAtBoundary.exists) {
            const auto forbidden = inspectPath(*_impl->forbidden, true);
            if (forbidden.exists && destinationAtBoundary.identity == forbidden.identity) {
                return fail("DESTINATION_ALIASES_FORBIDDEN_FILE",
                            "The destination aliases the protected canonical file");
            }
        }

        if (expectedDestinationSha256) {
            if (!sameObservedFile(destinationBefore, destinationAtBoundary)
                || !_impl->destinationSource->pathStillOwned()) {
                return fail("DESTINATION_CHANGED",
                            "The destination changed before the replacement boundary");
            }
            const auto boundaryHash = stableHash(*_impl->destinationSource);
            const auto destinationAfterHash = inspectPath(_impl->destination, false);
            if (!sameObservedFile(destinationAtBoundary, destinationAfterHash)
                || !sameObservedFile(destinationHashBefore->snapshot, boundaryHash.snapshot)
                || boundaryHash.value != *expectedDestinationSha256) {
                return fail("DESTINATION_CHANGED",
                            "The destination content changed before the replacement boundary");
            }
        }

        if (_impl->displaced && _impl->displacedSourceHash) {
            try {
                const auto boundarySourceHash = stableHash(*_impl->destinationSource);
                const auto boundarySnapshotHash = stableHash(*_impl->displaced);
                if (!_impl->destinationSource->pathStillOwned()
                    || !sameObservedFile(_impl->displacedSourceHash->snapshot,
                                         boundarySourceHash.snapshot)
                    || _impl->displacedSourceHash->value != boundarySourceHash.value
                    || !_impl->displaced->pathStillOwned()
                    || boundarySnapshotHash.value != boundarySourceHash.value
                    || !samePersistentFileState(boundarySourceHash.snapshot,
                                                boundarySnapshotHash.snapshot)) {
                    throw std::runtime_error(
                        "The previous file changed before the replacement boundary");
                }
                _impl->displaced->flush();
                _impl->displaced->prepareForExternalMove();
            }
            catch (const std::exception& exception) {
                result.warnings.push_back(
                    "The previous-file snapshot was discarded: "
                    + std::string(exception.what()));
                _impl->displaced.reset();
                _impl->displacedSourceHash.reset();
            }
        }

        // The owned handle pins the exact inode/file ID. Re-hash it and flush
        // it immediately before the namespace operation, then prove the path
        // still names that owned object. This closes path-swap and in-place
        // mutation seams between serialization and replacement.
        if (!_impl->temporary->pathStillOwned()) {
            return fail("TEMPORARY_FILE_CHANGED",
                        "The serialized temporary path changed before replacement");
        }
        const auto boundarySerializedHash = stableHash(*_impl->temporary);
        if (boundarySerializedHash.value != serializedHash.value
            || !sameObservedFile(boundarySerializedHash.snapshot, serializedHash.snapshot)) {
            return fail("TEMPORARY_FILE_CHANGED",
                        "The serialized temporary content changed before replacement");
        }
        _impl->temporary->flush();
        const auto flushedTemporary = _impl->temporary->snapshot();
        if (!sameObservedFile(boundarySerializedHash.snapshot, flushedTemporary)
            || !_impl->temporary->pathStillOwned()) {
            return fail("TEMPORARY_FILE_CHANGED",
                        "The serialized temporary file changed at the replacement boundary");
        }

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        if (_impl->request.afterFinalBoundaryValidationBeforeReplace) {
            _impl->request.afterFinalBoundaryValidationBeforeReplace();
        }
#endif

        if (_impl->displaced && _impl->displacedSourceHash) {
            try {
                const auto finalSourceHash = stableHash(*_impl->destinationSource);
                const auto finalSnapshotHash = stableHash(*_impl->displaced);
                if (!_impl->destinationSource->pathStillOwned()
                    || !sameObservedFile(_impl->displacedSourceHash->snapshot,
                                         finalSourceHash.snapshot)
                    || _impl->displacedSourceHash->value != finalSourceHash.value
                    || !_impl->displaced->pinnedPathStillOwned()
                    || finalSnapshotHash.value != finalSourceHash.value
                    || !samePersistentFileState(finalSourceHash.snapshot,
                                                finalSnapshotHash.snapshot)) {
                    throw std::runtime_error(
                        "The previous file or retained snapshot changed at the final boundary");
                }
                _impl->displaced->flush();
            }
            catch (const std::exception& exception) {
                result.warnings.push_back(
                    "The previous-file snapshot was discarded: "
                    + std::string(exception.what()));
                _impl->displaced.reset();
                _impl->displacedSourceHash.reset();
            }
        }

        // Preallocate the recovery-path result before the final serialization
        // proof and namespace primitive. Post-install handoff is then a
        // non-throwing string/shared_ptr move.
        std::string displacedPathForResult;
        if (_impl->displaced) {
            displacedPathForResult = _impl->displaced->pathUtf8();
        }

        // The test hook intentionally runs before the last content and
        // namespace proof. No callback or allocation-capable policy code may
        // run between the proof below and the OS replacement primitive.
        const auto finalSerializedHash = stableHash(*_impl->temporary);
        if (finalSerializedHash.value != serializedHash.value
            || !sameObservedFile(finalSerializedHash.snapshot, serializedHash.snapshot)) {
            return fail("TEMPORARY_FILE_CHANGED",
                        "The serialized temporary content changed at the replacement boundary");
        }
        _impl->temporary->flush();
        const auto finalFlushedTemporary = _impl->temporary->snapshot();
        if (!sameObservedFile(finalSerializedHash.snapshot, finalFlushedTemporary)) {
            return fail("TEMPORARY_FILE_CHANGED",
                        "The serialized temporary file changed at the replacement boundary");
        }

        const auto destinationAtFinalBoundary = inspectPath(_impl->destination, false);
        if (_impl->request.mode == DocumentFileReplacementMode::NoReplace
            && destinationAtFinalBoundary.exists) {
            return fail("DESTINATION_EXISTS", "The destination was created before replacement");
        }
        if (destinationAtFinalBoundary.link && _impl->forbidden) {
            const auto followed = inspectPath(_impl->destination, true);
            const auto forbidden = inspectPath(*_impl->forbidden, true);
            if (followed.exists && forbidden.exists
                && followed.identity == forbidden.identity) {
                return fail("DESTINATION_ALIASES_FORBIDDEN_FILE",
                            "The destination aliases the protected canonical file");
            }
        }
        if (destinationAtFinalBoundary.link) {
            return fail("DESTINATION_LINK_FORBIDDEN",
                        "The destination became a symbolic link or reparse point");
        }
        if (destinationAtFinalBoundary.exists && !destinationAtFinalBoundary.regular) {
            return fail("DESTINATION_NOT_REGULAR", "The destination is not a regular file");
        }
        if (_impl->forbidden && destinationAtFinalBoundary.exists) {
            const auto forbidden = inspectPath(*_impl->forbidden, true);
            if (forbidden.exists
                && destinationAtFinalBoundary.identity == forbidden.identity) {
                return fail("DESTINATION_ALIASES_FORBIDDEN_FILE",
                            "The destination aliases the protected canonical file");
            }
        }
        if (expectedDestinationSha256) {
            if (!sameObservedFile(destinationAtBoundary, destinationAtFinalBoundary)
                || !_impl->destinationSource->pathStillOwned()) {
                return fail("DESTINATION_CHANGED",
                            "The destination changed at the replacement boundary");
            }
            const auto finalDestinationHash = stableHash(*_impl->destinationSource);
            const auto destinationAfterFinalHash = inspectPath(_impl->destination, false);
            if (finalDestinationHash.value != *expectedDestinationSha256
                || !sameObservedFile(destinationAtFinalBoundary, destinationAfterFinalHash)
                || !sameObservedFile(destinationHashBefore->snapshot,
                                     finalDestinationHash.snapshot)) {
                return fail("DESTINATION_CHANGED",
                            "The destination content changed at the replacement boundary");
            }
        }

        if (!_impl->parent->pathStillOwned()) {
            return fail("DESTINATION_DIRECTORY_CHANGED",
                        "The destination directory changed at the replacement boundary");
        }
#ifndef FC_OS_WIN32
        // POSIX has no portable fd-source rename. Prove the final pathname
        // still names the owned inode immediately before renameat/linkat.
        if (!_impl->temporary->pathStillOwned()) {
            return fail("TEMPORARY_FILE_CHANGED",
                        "The serialized temporary path changed at the replacement boundary");
        }
#endif

        std::optional<AbsentSiblingPath> compareAndSwapGuard;
        fs::path guardPathForBinding;
        std::string guardUtf8ForBinding;
        fs::path canonicalPathForRestore;
        std::string canonicalUtf8ForRestore;
        std::string guardPathForResult;
        std::string guardRecoveryWarning;
        std::string temporaryRecoveryWarning;
        std::string canonicalRecoveryWarning;
        if (expectedDestinationSha256) {
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
            if (_impl->request.testFault
                == DocumentFileWriterTestFault::StrictNoReplaceUnavailable) {
                return fail(
                    "STRICT_NO_REPLACE_UNSUPPORTED",
                    "The filesystem does not provide the strict no-replace primitive "
                    "required for recoverable compare-and-swap");
            }
#endif
            compareAndSwapGuard = reserveAbsentSiblingPath(
                _impl->parent, _impl->destination, ".cas-recovery");
            guardPathForBinding = compareAndSwapGuard->path;
            guardUtf8ForBinding = compareAndSwapGuard->utf8;
            canonicalPathForRestore = _impl->destination;
            canonicalUtf8ForRestore = _impl->destinationUtf8;
            guardPathForResult = compareAndSwapGuard->utf8;
            guardRecoveryWarning =
                "The exact previous destination is retained as compare-and-swap "
                "recovery evidence at '"
                + compareAndSwapGuard->utf8 + "'.";
            temporaryRecoveryWarning =
                "The serialized replacement is retained as compare-and-swap recovery "
                "evidence at '"
                + _impl->temporary->pathUtf8() + "'.";
            canonicalRecoveryWarning =
                "A conflicting canonical destination was preserved without overwrite at '"
                + _impl->destinationUtf8 + "'.";
            result.warnings.reserve(result.warnings.size() + 4);
        }

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        if (_impl->request.beforeReplacementPrimitive) {
            _impl->request.beforeReplacementPrimitive();
        }
#endif

        std::error_code replacementError;
        bool replacementSourceConsumed = false;
        bool installed = false;
        // Set when the replacement had to override a read-only destination, so
        // the post-replacement verification can insist the attribute survived.
        bool destinationWasReadOnly = false;
        if (expectedDestinationSha256) {
            bool guardMoved = false;
            bool guardRestoreHookInvoked = false;
            const std::size_t warningsBeforeCompareAndSwap = result.warnings.size();

            const auto createVerifiedRecoveryCopy = [&](NativeFile& source,
                                                        const StableHash& expected,
                                                        const std::string_view suffix) {
                // A POSIX no-replace rename can only name its source by
                // pathname. If that leaf was substituted at the syscall
                // boundary, retain a stable, owned name for the exact bytes
                // still pinned by our original handle. Never claim or remove
                // the substituted entry moved by the namespace primitive.
                const auto sourceBefore = stableHash(source);
                if (sourceBefore.value != expected.value
                    || !sameObservedFile(sourceBefore.snapshot, expected.snapshot)) {
                    throw std::runtime_error(
                        "The retained source changed before recovery materialization");
                }
                auto recovery =
                    NativeFile::createSibling(_impl->parent, _impl->destination, suffix);
                const auto copiedHash = copyAndHash(source, *recovery);
                const auto sourceAfter = stableHash(source);
                recovery->preserveMetadataFrom(source, true);
                recovery->flush();
                const auto recoveryHash = stableHash(*recovery);
                if (copiedHash != expected.value || sourceAfter.value != expected.value
                    || recoveryHash.value != expected.value
                    || !sameObservedFile(sourceBefore.snapshot, sourceAfter.snapshot)
                    || !samePersistentFileState(sourceAfter.snapshot,
                                                recoveryHash.snapshot)
                    || !recovery->pinnedPathStillOwned()
                    || !_impl->parent->pathStillOwned()) {
                    throw std::runtime_error(
                        "The retained source changed while its recovery copy was created");
                }
                _impl->parent->flush();
                return recovery;
            };

            const auto publishPredecessorRecovery = [&] (
                std::shared_ptr<NativeFile> recovery) {
                std::string predecessorPath = recovery->pathUtf8();
                std::string predecessorWarning =
                    "The exact previous destination is retained as compare-and-swap "
                    "recovery evidence at '"
                    + predecessorPath + "'.";
                if (result.warnings.size() > warningsBeforeCompareAndSwap) {
                    result.warnings[warningsBeforeCompareAndSwap]
                        .swap(predecessorWarning);
                }
                else {
                    result.warnings.push_back(std::move(predecessorWarning));
                }
                result.displacedFile.swap(predecessorPath);
                recovery->relinquishPathCleanup();
                result.displacedFileLease = std::move(recovery);
            };

            const auto preserveRecoveryEvidence = [&]() {
                // A partial CAS must never let ordinary destructor cleanup
                // erase either owned version. The existing displaced lease is
                // the authority for the guard; the temporary path is reported
                // explicitly because the public result ABI has one lease slot.
                _impl->temporary->relinquishPathCleanup();
                // Relinquish even while the result retains the handle. This
                // makes both paths durable recovery evidence if result
                // construction, copying, or caller-side handling later fails.
                _impl->destinationSource->relinquishPathCleanup();
                if (!result.displacedFileLease) {
                    result.displacedFile.swap(guardPathForResult);
                    result.displacedFileLease = _impl->destinationSource;
                }
                if (!guardRecoveryWarning.empty()) {
                    result.warnings.push_back(std::move(guardRecoveryWarning));
                }
                if (!temporaryRecoveryWarning.empty()) {
                    result.warnings.push_back(std::move(temporaryRecoveryWarning));
                }
                try {
                    if (inspectPath(_impl->destination, false).exists) {
                        result.warnings.push_back(std::move(canonicalRecoveryWarning));
                    }
                }
                catch (...) {
                    // Recovery ownership is already safe. Diagnostics must not
                    // weaken that state if the namespace can no longer be read.
                }
            };

            const auto recoverGuard = [&](std::string code, std::string message) {
                bool restored = false;
                std::string restoreDiagnostic;
                bool guardCanBeRestored = false;
                try {
                    guardCanBeRestored = guardMoved && _impl->parent->pathStillOwned()
                        && _impl->destinationSource->pinnedPathStillOwned();
                }
                catch (const std::exception& exception) {
                    restoreDiagnostic = exception.what();
                }
                catch (...) {
                    restoreDiagnostic = "the recovery guard identity could not be inspected";
                }
                if (guardCanBeRestored) {
                    bool hookCompleted = true;
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                    if (!guardRestoreHookInvoked
                        && _impl->request.beforeCompareAndSwapGuardRestore) {
                        guardRestoreHookInvoked = true;
                        try {
                            _impl->request.beforeCompareAndSwapGuardRestore(
                                _impl->destinationSource->pathUtf8());
                        }
                        catch (const std::exception& exception) {
                            hookCompleted = false;
                            restoreDiagnostic = exception.what();
                        }
                        catch (...) {
                            hookCompleted = false;
                            restoreDiagnostic = "unknown restore-hook failure";
                        }
                    }
#endif
                    if (hookCompleted) {
                        std::error_code restoreError;
                        StrictNoReplaceResult restore = StrictNoReplaceResult::Failed;
                        try {
                            restore = moveOwnedFileNoReplaceStrict(
                                *_impl->destinationSource,
                                *_impl->parent,
                                _impl->destination,
                                restoreError);
                        }
                        catch (const std::exception& exception) {
                            restoreDiagnostic = exception.what();
                        }
                        catch (...) {
                            restoreDiagnostic =
                                "the strict recovery primitive failed unexpectedly";
                        }
                        if (restore == StrictNoReplaceResult::Installed) {
                            _impl->destinationSource->bindRestoredCanonicalPath(
                                std::move(canonicalPathForRestore),
                                std::move(canonicalUtf8ForRestore));
                            guardMoved = false;
                            bool inspectRestoredName = true;
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                            if (_impl->request
                                    .afterCompareAndSwapGuardRestoreBeforeInspection) {
                                try {
                                    _impl->request
                                        .afterCompareAndSwapGuardRestoreBeforeInspection(
                                            _impl->destinationSource->pathUtf8());
                                }
                                catch (const std::exception& exception) {
                                    inspectRestoredName = false;
                                    restoreDiagnostic =
                                        "post-restore inspection hook failed: "
                                        + std::string(exception.what());
                                }
                                catch (...) {
                                    inspectRestoredName = false;
                                    restoreDiagnostic =
                                        "post-restore inspection hook failed unexpectedly";
                                }
                            }
#endif
                            const auto materializeAfterRestoreInspectionFailure = [&] {
                                _impl->destinationSource->relinquishPathCleanup();
                                try {
                                    publishPredecessorRecovery(
                                        createVerifiedRecoveryCopy(
                                            *_impl->destinationSource,
                                            *destinationHashBefore,
                                            ".cas-restored-predecessor-recovery"));
                                    restoreDiagnostic +=
                                        "; the exact predecessor was copied to stable "
                                        "recovery evidence";
                                }
                                catch (const std::exception& exception) {
                                    restoreDiagnostic +=
                                        "; a stable recovery copy could not be created: "
                                        + std::string(exception.what());
                                }
                                catch (...) {
                                    restoreDiagnostic +=
                                        "; a stable recovery copy could not be created";
                                }
                            };
                            try {
                                const auto restoredEntry = inspectRestoredName
                                    ? inspectPath(_impl->destination, false)
                                    : FileSnapshot {};
                                if (!inspectRestoredName || !restoredEntry.exists
                                    || !restoredEntry.regular
                                    || restoredEntry.identity
                                        != _impl->destinationSource->identity()) {
                                    if (restoreDiagnostic.empty()) {
                                        restoreDiagnostic =
                                            "the restored name no longer identifies the "
                                            "captured destination";
                                    }
                                    materializeAfterRestoreInspectionFailure();
                                }
                                else {
                                    restored = true;
                                    try {
                                        _impl->destinationSource->flush();
                                        _impl->parent->flush();
                                    }
                                    catch (const std::exception& exception) {
                                        result.warnings.push_back(
                                            "The expected destination was restored without "
                                            "overwrite, but its directory durability could not "
                                            "be verified: "
                                            + std::string(exception.what()));
                                    }
                                }
                            }
                            catch (const std::exception& exception) {
                                restoreDiagnostic =
                                    "the restored name could not be inspected: "
                                    + std::string(exception.what());
                                materializeAfterRestoreInspectionFailure();
                            }
                            catch (...) {
                                restoreDiagnostic =
                                    "the restored name could not be inspected";
                                materializeAfterRestoreInspectionFailure();
                            }
                        }
                        else if (restore == StrictNoReplaceResult::DestinationExists) {
                            restoreDiagnostic =
                                "the canonical destination is occupied by a concurrent file";
                        }
                        else if (restore == StrictNoReplaceResult::SourceChanged) {
                            try {
                                publishPredecessorRecovery(
                                    createVerifiedRecoveryCopy(
                                        *_impl->destinationSource,
                                        *destinationHashBefore,
                                        ".cas-predecessor-recovery"));
                                restoreDiagnostic =
                                    "the recovery-guard source changed inside the strict "
                                    "no-replace primitive; its exact predecessor was copied "
                                    "to stable recovery evidence";
                            }
                            catch (const std::exception& exception) {
                                restoreDiagnostic =
                                    "the recovery-guard source changed inside the strict "
                                    "no-replace primitive, and its stable recovery copy "
                                    "could not be created: "
                                    + std::string(exception.what());
                            }
                        }
                        else if (restore == StrictNoReplaceResult::Unsupported) {
                            restoreDiagnostic =
                                "strict no-replace became unavailable during recovery";
                        }
                        else if (restoreDiagnostic.empty()) {
                            restoreDiagnostic = systemMessage(restoreError);
                        }
                    }
                }

                if (!restored) {
                    try {
                        _impl->destinationSource->flush();
                        _impl->parent->flush();
                    }
                    catch (const std::exception& exception) {
                        if (restoreDiagnostic.empty()) {
                            restoreDiagnostic = exception.what();
                        }
                    }
                    preserveRecoveryEvidence();
                    if (!restoreDiagnostic.empty()) {
                        message += "; automatic restoration was not completed: "
                            + restoreDiagnostic;
                    }
                }
                else {
                    result.displacedFile.clear();
                    result.displacedFileLease.reset();
                    if (result.warnings.size() > warningsBeforeCompareAndSwap) {
                        // The canonical predecessor was restored, so retain
                        // the serialized-temporary recovery diagnostic and
                        // any later durability warning. Only the first CAS
                        // warning describes the no-longer-displaced guard.
                        result.warnings.erase(
                            result.warnings.begin()
                            + static_cast<std::ptrdiff_t>(warningsBeforeCompareAndSwap));
                    }
                }
                return fail(std::move(code), std::move(message));
            };

            // Close the original validation-to-replace gap after the public
            // test/race seam. A pathname swap here is detected before any
            // canonical namespace mutation.
            const auto destinationImmediatelyBeforeGuard =
                inspectPath(_impl->destination, false);
            if (!sameObservedFile(destinationAtFinalBoundary,
                                  destinationImmediatelyBeforeGuard)
                || !_impl->destinationSource->pathStillOwned()) {
                return fail("DESTINATION_CHANGED",
                            "The destination changed before compare-and-swap guard creation");
            }
            const auto destinationHashImmediatelyBeforeGuard =
                stableHash(*_impl->destinationSource);
            const auto destinationAfterImmediateHash =
                inspectPath(_impl->destination, false);
            if (destinationHashImmediatelyBeforeGuard.value
                    != *expectedDestinationSha256
                || !sameObservedFile(destinationHashBefore->snapshot,
                                     destinationHashImmediatelyBeforeGuard.snapshot)
                || !sameObservedFile(destinationImmediatelyBeforeGuard,
                                     destinationAfterImmediateHash)) {
                return fail("DESTINATION_CHANGED",
                            "The destination content changed before compare-and-swap guard "
                            "creation");
            }
            try {
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                if (_impl->request.testFault
                    == DocumentFileWriterTestFault::CompareAndSwapDurabilityUnavailable) {
                    throw std::system_error(
# ifdef FC_OS_WIN32
                        std::error_code(ERROR_WRITE_FAULT, std::system_category()),
# else
                        std::error_code(EIO, std::generic_category()),
# endif
                        "Injected predecessor durability failure");
                }
#endif
                _impl->destinationSource->flush();
            }
            catch (const std::exception& exception) {
                return fail(
                    "COMPARE_AND_SWAP_DURABILITY_UNAVAILABLE",
                    "The expected destination could not be made durable before "
                    "compare-and-swap guard creation: "
                        + std::string(exception.what()));
            }

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
            if (_impl->request.beforeCompareAndSwapGuardMove) {
                _impl->request.beforeCompareAndSwapGuardMove(
                    compareAndSwapGuard->utf8);
            }
#endif

            const auto guardMove = moveOwnedFileNoReplaceStrict(
                *_impl->destinationSource,
                *_impl->parent,
                compareAndSwapGuard->path,
                replacementError
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                , _impl->request.afterStrictNoReplaceSourceValidationBeforeRename
#endif
            );
            if (guardMove == StrictNoReplaceResult::DestinationExists) {
                return fail("COMPARE_AND_SWAP_GUARD_COLLISION",
                            "A foreign file occupied the reserved recovery-guard name at '"
                                + compareAndSwapGuard->utf8 + "'");
            }
            if (guardMove == StrictNoReplaceResult::Unsupported) {
                return fail("STRICT_NO_REPLACE_UNSUPPORTED",
                            "Strict no-replace became unavailable before guard creation");
            }
            if (guardMove == StrictNoReplaceResult::SourceChanged) {
                // The namespace primitive moved a substituted source entry,
                // not the retained expected predecessor. Materialize an
                // independently named, hash-verified copy from the retained
                // predecessor handle before returning. Never claim or remove
                // the foreign file now at the guard name.
                _impl->temporary->relinquishPathCleanup();
                try {
                    auto predecessorRecovery = createVerifiedRecoveryCopy(
                        *_impl->destinationSource,
                        *destinationHashBefore,
                        ".cas-predecessor-recovery");
                    std::string predecessorPath = predecessorRecovery->pathUtf8();
                    std::string predecessorWarning =
                        "The exact previous destination is retained as compare-and-swap "
                        "recovery evidence at '"
                        + predecessorPath + "'.";
                    result.warnings.push_back(std::move(predecessorWarning));
                    result.displacedFile.swap(predecessorPath);
                    result.displacedFileLease = std::move(predecessorRecovery);
                    std::static_pointer_cast<NativeFile>(result.displacedFileLease)
                        ->relinquishPathCleanup();
                }
                catch (const std::exception& exception) {
                    // Keep the authoritative handle alive in the result even
                    // if the filesystem cannot allocate a stable recovery
                    // name. This is an explicit degraded recovery condition,
                    // never a successful CAS.
                    result.displacedFileLease = _impl->destinationSource;
                    result.warnings.push_back(
                        "The exact previous destination remains pinned by its retained "
                        "handle, but a stable recovery copy could not be created: "
                        + std::string(exception.what()));
                }
                result.warnings.push_back(std::move(temporaryRecoveryWarning));
                result.warnings.push_back(
                    "A source entry substituted during the strict guard move was "
                    "preserved without overwrite at '"
                    + compareAndSwapGuard->utf8 + "'.");
                try {
                    _impl->parent->flush();
                }
                catch (const std::exception& exception) {
                    result.warnings.push_back(
                        "The substituted guard entry was preserved, but its directory "
                        "update could not be verified: "
                        + std::string(exception.what()));
                }
                return fail(
                    "STRICT_NO_REPLACE_SOURCE_CHANGED",
                    "The validated destination source changed inside the strict "
                    "no-replace guard primitive; the substituted entry and serialized "
                    "temporary were preserved");
            }
            if (guardMove != StrictNoReplaceResult::Installed) {
                return fail("COMPARE_AND_SWAP_GUARD_FAILED",
                            "Unable to move the exact destination to its recovery guard: "
                                + systemMessage(replacementError));
            }
            _impl->destinationSource->bindMovedOwnedPath(
                std::move(guardPathForBinding), std::move(guardUtf8ForBinding));
            guardMoved = true;
            // From this point until either a verified restore or install, an
            // unexpected exception must preserve both complete versions.
            _impl->destinationSource->relinquishPathCleanup();
            _impl->temporary->relinquishPathCleanup();
            result.displacedFile.swap(guardPathForResult);
            result.displacedFileLease = _impl->destinationSource;
            result.warnings.push_back(std::move(guardRecoveryWarning));
            result.warnings.push_back(std::move(temporaryRecoveryWarning));

            try {
                _impl->destinationSource->flush();
                _impl->parent->flush();
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                if (_impl->request.afterCompareAndSwapGuardMove) {
                    _impl->request.afterCompareAndSwapGuardMove(
                        _impl->destinationSource->pathUtf8());
                }
#endif
            }
            catch (const std::exception& exception) {
                return recoverGuard("COMPARE_AND_SWAP_GUARD_VALIDATION_FAILED",
                                    "The moved destination guard could not be made durable: "
                                        + std::string(exception.what()));
            }
            catch (...) {
                return recoverGuard("COMPARE_AND_SWAP_GUARD_VALIDATION_FAILED",
                                    "The moved destination guard failed with an unknown error");
            }

            const auto guardMatchesExpected = [&]() {
                if (!_impl->parent->pathStillOwned()
                    || !_impl->destinationSource->pinnedPathStillOwned()
                    || inspectPath(_impl->destination, false).exists) {
                    return false;
                }
                const auto guardEntry = inspectPath(compareAndSwapGuard->path, false);
                const auto guardHash = stableHash(*_impl->destinationSource);
                const auto guardAfterHash =
                    inspectPath(compareAndSwapGuard->path, false);
                return guardEntry.exists && guardEntry.regular
                    && guardEntry.identity == _impl->destinationSource->identity()
                    && guardHash.value == *expectedDestinationSha256
                    && sameObservedFile(destinationHashBefore->snapshot,
                                        guardHash.snapshot)
                    && sameObservedFile(guardEntry, guardAfterHash);
            };

            bool guardValidated = false;
            try {
                guardValidated = guardMatchesExpected();
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                if (_impl->request.testFault
                    == DocumentFileWriterTestFault::CompareAndSwapGuardValidation) {
                    guardValidated = false;
                }
#endif
            }
            catch (...) {
                guardValidated = false;
            }
            if (!guardValidated) {
                return recoverGuard("DESTINATION_CHANGED",
                                    "The moved destination no longer matches the expected "
                                    "compare-and-swap content");
            }

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
            if (_impl->request.afterCompareAndSwapGuardValidationBeforeInstall) {
                try {
                    _impl->request.afterCompareAndSwapGuardValidationBeforeInstall(
                        _impl->destinationSource->pathUtf8());
                }
                catch (const std::exception& exception) {
                    return recoverGuard("COMPARE_AND_SWAP_INSTALL_ABORTED",
                                        "The compare-and-swap install hook failed: "
                                            + std::string(exception.what()));
                }
                catch (...) {
                    return recoverGuard(
                        "COMPARE_AND_SWAP_INSTALL_ABORTED",
                        "The compare-and-swap install hook failed with an unknown error");
                }
            }
#endif

            try {
                guardValidated = guardMatchesExpected();
            }
            catch (...) {
                guardValidated = false;
            }
            if (!guardValidated) {
                return recoverGuard("DESTINATION_CHANGED",
                                    "The destination or recovery guard changed before the "
                                    "strict no-replace install");
            }

            StrictNoReplaceResult strictInstall = StrictNoReplaceResult::Failed;
            try {
                strictInstall = moveOwnedFileNoReplaceStrict(
                    *_impl->temporary,
                    *_impl->parent,
                    _impl->destination,
                    replacementError
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                    , _impl->request.afterStrictNoReplaceSourceValidationBeforeRename
#endif
                );
            }
            catch (const std::exception& exception) {
                return recoverGuard(
                    "ATOMIC_REPLACEMENT_FAILED",
                    "Unable to prepare the strict compare-and-swap install: "
                        + std::string(exception.what()));
            }
            catch (...) {
                return recoverGuard(
                    "ATOMIC_REPLACEMENT_FAILED",
                    "Unable to prepare the strict compare-and-swap install");
            }
            if (strictInstall != StrictNoReplaceResult::Installed) {
                if (strictInstall == StrictNoReplaceResult::DestinationExists) {
                    return recoverGuard(
                        "DESTINATION_CHANGED",
                        "A concurrent file occupied the destination before compare-and-swap "
                        "installation");
                }
                if (strictInstall == StrictNoReplaceResult::Unsupported) {
                    return recoverGuard(
                        "STRICT_NO_REPLACE_UNSUPPORTED",
                        "Strict no-replace became unavailable during compare-and-swap "
                        "installation");
                }
                if (strictInstall == StrictNoReplaceResult::SourceChanged) {
                    try {
                        auto serializedRecovery = createVerifiedRecoveryCopy(
                            *_impl->temporary,
                            serializedHash,
                            ".cas-serialized-recovery");
                        std::string serializedWarning =
                            "The exact serialized replacement is retained as "
                            "compare-and-swap recovery evidence at '"
                            + serializedRecovery->pathUtf8() + "'.";
                        if (result.warnings.size() > warningsBeforeCompareAndSwap + 1) {
                            result.warnings[warningsBeforeCompareAndSwap + 1]
                                .swap(serializedWarning);
                        }
                        else {
                            result.warnings.push_back(std::move(serializedWarning));
                        }
                        serializedRecovery->relinquishPathCleanup();
                    }
                    catch (const std::exception& exception) {
                        std::string serializedWarning =
                            "A stable recovery copy for the exact serialized replacement "
                            "could not be created after its source leaf changed; the former "
                            "temporary pathname is not trusted: "
                            + std::string(exception.what());
                        if (result.warnings.size() > warningsBeforeCompareAndSwap + 1) {
                            result.warnings[warningsBeforeCompareAndSwap + 1]
                                .swap(serializedWarning);
                        }
                        else {
                            result.warnings.push_back(std::move(serializedWarning));
                        }
                    }
                    return recoverGuard(
                        "STRICT_NO_REPLACE_SOURCE_CHANGED",
                        "The serialized temporary source changed inside the strict "
                        "no-replace install primitive; the substituted canonical entry "
                        "was preserved without overwrite");
                }
                return recoverGuard(
                    "ATOMIC_REPLACEMENT_FAILED",
                    "Unable to install the serialized compare-and-swap file: "
                        + systemMessage(replacementError));
            }
            installed = true;
            replacementSourceConsumed = true;
            _impl->temporary->markTransferred(_impl->destination);
            // The no-replace primitive has irreversibly installed a file at
            // the canonical name. Record that truth before any cleanup or
            // ownership diagnostic that can throw.
            result.replacementCompleted = true;
            result.fileWritten = true;
            if (result.warnings.size() > warningsBeforeCompareAndSwap + 1) {
                // The serialized version is now the canonical file, so the
                // temporary-path recovery diagnostic stopped being true at
                // the same irreversible boundary. Keep the guard diagnostic
                // until post-install cleanup has actually been inspected.
                result.warnings.pop_back();
            }
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
            if (_impl->request.testFault
                == DocumentFileWriterTestFault::AfterReplacementBeforeVerification) {
                return fail("TEST_INJECTED_REPLACEMENT_VERIFICATION_FAILURE",
                            "Injected failure after replacement and before verification");
            }
#endif
            result.warnings.resize(warningsBeforeCompareAndSwap);
            bool guardOwnershipProved = false;
            std::string guardInspectionDiagnostic;
            try {
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
                if (_impl->request
                        .afterCompareAndSwapInstallBeforeGuardInspection) {
                    _impl->request
                        .afterCompareAndSwapInstallBeforeGuardInspection(
                            _impl->destinationSource->pathUtf8());
                }
                if (_impl->request.testFault
                    == DocumentFileWriterTestFault::CompareAndSwapPostInstallGuardInspection) {
                    throw std::runtime_error(
                        "Injected post-install guard ownership inspection failure");
                }
#endif
                guardOwnershipProved =
                    _impl->destinationSource->pinnedPathStillOwned();
                if (!guardOwnershipProved) {
                    guardInspectionDiagnostic =
                        "the obsolete guard name no longer identifies the retained "
                        "predecessor";
                }
            }
            catch (const std::exception& exception) {
                guardInspectionDiagnostic = exception.what();
            }
            catch (...) {
                guardInspectionDiagnostic =
                    "guard ownership inspection failed unexpectedly";
            }
            if (guardOwnershipProved) {
                _impl->destinationSource->claimOwnedPathCleanup();
            }
            else {
                // The serialized canonical install is already authoritative.
                // Never touch a possibly foreign guard pathname. Materialize
                // the exact predecessor from the still-retained handle under
                // a new, hash-verified sibling name and publish that lease.
                _impl->destinationSource->relinquishPathCleanup();
                const std::string untrustedGuard =
                    _impl->destinationSource->pathUtf8();
                try {
                    publishPredecessorRecovery(
                        createVerifiedRecoveryCopy(
                            *_impl->destinationSource,
                            *destinationHashBefore,
                            ".cas-post-install-predecessor-recovery"));
                    result.warnings.push_back(
                        "The installed file is valid, but compare-and-swap guard "
                        "ownership could not be proved at '"
                        + untrustedGuard + "' (" + guardInspectionDiagnostic
                        + "). That pathname was left untouched and the exact predecessor "
                          "was copied to the reported recovery path.");
                }
                catch (const std::exception& exception) {
                    result.warnings.push_back(
                        "The installed file is valid, but compare-and-swap guard "
                        "ownership could not be proved at '"
                        + untrustedGuard + "' (" + guardInspectionDiagnostic
                        + "). The retained predecessor handle remains authoritative, but "
                          "a stable recovery copy could not be created: "
                        + std::string(exception.what()));
                }
                catch (...) {
                    result.warnings.push_back(
                        "The installed file is valid, but compare-and-swap guard "
                        "ownership could not be proved at '"
                        + untrustedGuard
                        + "'. The retained predecessor handle remains authoritative, but "
                          "a stable recovery copy could not be created.");
                }
            }
        }
        else {
            bool ignoreDestinationReadOnly = false;
#ifdef FC_OS_WIN32
            // A read-only destination cannot be replaced unless the rename is
            // told to disregard the attribute. Decide that here, from the
            // retained handle, and refuse the save rather than reaching a
            // namespace boundary that would fail with a bare "Access is
            // denied" after the document had already been serialized.
            if (_impl->request.mode != DocumentFileReplacementMode::NoReplace
                && _impl->destinationSource && _impl->destinationSource->isReadOnly()) {
                if (!_impl->destinationSource->hasProvenAttributeAuthority()) {
                    return fail("DESTINATION_READ_ONLY",
                                "The destination is marked read-only and this process does not "
                                "hold the attribute authority required to replace it: '"
                                    + _impl->destinationUtf8 + "'");
                }
                ignoreDestinationReadOnly = true;
                destinationWasReadOnly = true;
            }
#endif
            installed = replaceOwnedFile(*_impl->temporary,
                                         *_impl->parent,
                                         _impl->destination,
                                         _impl->request.mode,
                                         replacementError,
                                         replacementSourceConsumed,
                                         ignoreDestinationReadOnly);
            if (!installed) {
#ifdef FC_OS_WIN32
                if (ignoreDestinationReadOnly) {
                    // The override is the only supported way through, and the
                    // writer will not clear the attribute by pathname to get
                    // there. Name the condition so a caller can offer Save As.
                    return fail("DESTINATION_READ_ONLY",
                                "The destination is marked read-only and this filesystem does "
                                "not support replacing it while preserving that attribute: '"
                                    + _impl->destinationUtf8
                                    + "': " + systemMessage(replacementError));
                }
#endif
                if (_impl->request.mode == DocumentFileReplacementMode::NoReplace
                    && isDestinationExistsError(replacementError)) {
                    return fail("DESTINATION_EXISTS",
                                "The destination was created before replacement");
                }
                return fail("ATOMIC_REPLACEMENT_FAILED",
                            "Unable to atomically replace the destination: "
                                + systemMessage(replacementError));
            }
        }
        result.replacementCompleted = true;
        result.fileWritten = true;
        if (replacementSourceConsumed && !expectedDestinationSha256) {
            _impl->temporary->markTransferred(_impl->destination);
        }
        if (_impl->displaced) {
            result.displacedFile.swap(displacedPathForResult);
            result.displacedFileLease = std::move(_impl->displaced);
            _impl->displacedSourceHash.reset();
        }
        if (replacementError) {
            result.warnings.push_back(
                "The installed file is valid, but its temporary name could not be removed: "
                + systemMessage(replacementError));
        }
        else if (!replacementSourceConsumed) {
            // The portable no-replace link fallback installs the destination
            // without consuming its source name, because POSIX cannot remove
            // that name conditionally on the retained identity. Report the
            // retained serialization as recoverable evidence rather than
            // deleting a name that may have been substituted.
            result.warnings.push_back(
                "The installed file is valid, but its serialized temporary name was retained at '"
                + _impl->temporary->pathUtf8()
                + "' because this filesystem provides no strict no-replace rename and POSIX "
                  "cannot remove the name conditionally on its retained identity. Remove it "
                  "manually after verifying the installed file.");
        }

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        if (_impl->request.testFault
            == DocumentFileWriterTestFault::AfterReplacementBeforeVerification) {
            return fail("TEST_INJECTED_REPLACEMENT_VERIFICATION_FAILURE",
                        "Injected failure after replacement and before verification");
        }
#endif

        auto destinationAfter = inspectPath(_impl->destination, false);
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        if (_impl->request.simulatePathInvisibleUntilDescriptorRelease) {
            // Model a filesystem that cannot resolve the installed name while
            // this process still holds a descriptor on the inode.
            destinationAfter = FileSnapshot {};
        }
#endif
        if (destinationAfter.exists) {
            if (!destinationAfter.regular
                || destinationAfter.identity != _impl->temporary->identity()) {
                return fail(
                    "REPLACEMENT_VERIFICATION_FAILED",
                    "The installed destination does not match the serialized temporary file");
            }
            const auto installedHash = stableHash(*_impl->temporary);
            if (installedHash.value != serializedHash.value
                || !sameObservedFile(installedHash.snapshot, destinationAfter)) {
                return fail("REPLACEMENT_VERIFICATION_FAILED",
                            "The installed destination content could not be verified");
            }
#ifdef FC_OS_WIN32
            // Overriding the attribute is permission to replace the file, not
            // permission to unmark it. R13 says basic attributes survive a
            // replacement, and read-only is the one the override could plausibly
            // have dropped, so prove it is still there through the handle that
            // now names the installed file.
            if (destinationWasReadOnly && !_impl->temporary->isReadOnly()) {
                return fail("REPLACEMENT_VERIFICATION_FAILED",
                            "The replaced destination was read-only, but the installed file is "
                            "not");
            }
#endif
            result.replacementVerified = true;
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
            if (_impl->request.testFault == DocumentFileWriterTestFault::BeforeDurabilityFlush) {
                return fail("TEST_INJECTED_DURABILITY_FAILURE",
                            "Injected failure before replacement durability flush");
            }
#endif
            try {
                // The same retained handle now names the installed destination;
                // never reopen a potentially swapped path for durability.
                _impl->temporary->flush();
                _impl->parent->flush();
            }
            catch (const std::exception& exception) {
                return fail("DURABILITY_UNVERIFIED", exception.what());
            }
            result.durabilityVerified = true;
            // Every proof this handle can give has now been given: identity,
            // content against the serialized hash, and durability. It still
            // names the installed canonical file and still carries DELETE, and
            // on Windows that blocks every ordinary reader in every process --
            // including FreeCAD reopening the document the user just saved.
            // Holding it past this point buys nothing and costs the canonical
            // file's user-visible readability, so release it here rather than
            // whenever the writer happens to be destroyed.
            _impl->temporary->releaseDescriptor();
        }
        else {
            // The destination name did not resolve at all. A rename that
            // reports success is never accepted as proof on its own, so prove
            // everything provable through the retained handle first, then
            // release it and re-resolve the name. Releasing before the
            // re-resolve is required: on the reproduced 9p behaviour the name
            // stays unresolvable for as long as the descriptor is held, so
            // waiting with it open can never succeed.
            // Re-proving the content through the retained handle is attempted
            // first, but it is not always possible: on the same filesystems
            // that hide the renamed name, fstat on the retained descriptor also
            // fails with ENOENT, so the handle can no longer be identified or
            // hashed. That costs nothing, because serializedHash was itself
            // computed and stably re-verified through this very handle before
            // the replacement, and the post-release re-hash below proves the
            // installed bytes against it in full. A rename reporting success is
            // still never accepted on its own.
            std::string retainedProofDiagnostic;
            try {
                const auto retained = stableHash(*_impl->temporary);
                if (retained.value != serializedHash.value) {
                    return fail("REPLACEMENT_VERIFICATION_FAILED",
                                "The installed destination content could not be verified");
                }
            }
            catch (const std::exception& exception) {
                retainedProofDiagnostic = exception.what();
            }
            try {
                _impl->temporary->flush();
            }
            catch (const std::exception& exception) {
                // Durability is established below on the re-resolved handle and
                // the pinned parent; a handle that can no longer be identified
                // cannot be flushed either.
                if (retainedProofDiagnostic.empty()) {
                    return fail("DURABILITY_UNVERIFIED", exception.what());
                }
            }
            const std::string expectedIdentity = _impl->temporary->identity();
            const auto expectedSize = serializedHash.snapshot.size;

            _impl->temporary->releaseDescriptor();

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
            if (_impl->request.afterInstalledDescriptorRelease) {
                _impl->request.afterInstalledDescriptorRelease(_impl->destinationUtf8);
            }
#endif
            std::shared_ptr<NativeFile> installed;
            std::string reResolveDiagnostic;
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
            for (;;) {
                try {
                    installed = NativeFile::openInstalledThroughPinnedParent(
                        _impl->parent, _impl->destination);
                }
                catch (const std::exception& exception) {
                    reResolveDiagnostic = exception.what();
                    installed.reset();
                }
                if (installed || std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!installed) {
                return fail("REPLACEMENT_VERIFICATION_FAILED",
                            "The installed destination could not be re-resolved after releasing "
                            "the serialized handle: "
                                + reResolveDiagnostic);
            }

            const auto namespaceBefore = inspectPath(_impl->destination, false);
            StableHash installedHash;
            try {
                installedHash = stableHash(*installed);
            }
            catch (const std::exception& exception) {
                return fail("REPLACEMENT_VERIFICATION_FAILED",
                            "The re-resolved destination could not be hashed: "
                                + std::string(exception.what()));
            }
            const auto namespaceAfter = inspectPath(_impl->destination, false);
            const bool namespaceStable = namespaceBefore.exists && namespaceBefore.regular
                && namespaceAfter.exists && namespaceAfter.regular
                && sameObservedFile(namespaceBefore, namespaceAfter)
                && sameObservedFile(installedHash.snapshot, namespaceAfter);
            if (!installedHash.snapshot.regular || installedHash.value != serializedHash.value
                || installedHash.snapshot.size != expectedSize || !namespaceStable) {
                // Foreign or mismatching content. Fail closed: the entry now at
                // that name is never overwritten or removed, and every recovery
                // artifact the caller already holds is left in place.
                return fail("REPLACEMENT_VERIFICATION_FAILED",
                            "The re-resolved destination does not match the serialized document; "
                            "the entry now at that name was left untouched");
            }
            if (installedHash.snapshot.identity != expectedIdentity) {
                result.warnings.push_back(
                    "The installed file was verified by content, size and stable namespace "
                    "observation after its name became resolvable again, because this "
                    "filesystem did not report a stable file identity across the replacement.");
            }
            result.replacementVerified = true;
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
            if (_impl->request.testFault == DocumentFileWriterTestFault::BeforeDurabilityFlush) {
                return fail("TEST_INJECTED_DURABILITY_FAILURE",
                            "Injected failure before replacement durability flush");
            }
#endif
            try {
                installed->flush();
                _impl->parent->flush();
            }
            catch (const std::exception& exception) {
                return fail("DURABILITY_UNVERIFIED", exception.what());
            }
            result.durabilityVerified = true;
        }
        if (expectedDestinationSha256) {
            if (!_impl->request.preserveDisplacedFile
                && result.displacedFileLease) {
                const auto guard = std::static_pointer_cast<NativeFile>(
                    result.displacedFileLease);
                if (guard->discardExact()) {
                    result.displacedFileLease.reset();
                    result.displacedFile.clear();
                    try {
                        _impl->parent->flush();
                    }
                    catch (const std::exception& exception) {
                        result.warnings.push_back(
                            "The obsolete compare-and-swap recovery guard was removed, but "
                            "its directory update could not be verified: "
                            + std::string(exception.what()));
                    }
                }
                else {
                    result.warnings.push_back(
                        "The obsolete compare-and-swap recovery guard at '"
                        + guard->pathUtf8()
                        + "' could not be removed; its retained lease remains available to "
                          "the caller.");
                }
            }
            result.message =
                "The serialized file was installed by recoverable compare-and-swap and verified";
        }
        else {
            result.message = "The serialized file was atomically installed and verified";
        }
        return result;
    }
    catch (const std::invalid_argument& exception) {
        return fail("INVALID_REPLACEMENT_REQUEST", exception.what());
    }
    catch (const std::exception& exception) {
        return fail("REPLACEMENT_PREFLIGHT_FAILED", exception.what());
    }
    catch (const Base::Exception& exception) {
        // Base::Exception derives from BaseClass, not std::exception, so it
        // would otherwise fall through to the catch-all below and lose its
        // diagnostic entirely. Every Base::FileException raised by the code
        // this function calls arrives here.
        return fail("REPLACEMENT_PREFLIGHT_FAILED", exception.what());
    }
    catch (...) {
        return fail("REPLACEMENT_PREFLIGHT_FAILED",
                    "File replacement failed with an unknown exception");
    }
}

}  // namespace App::Internal
