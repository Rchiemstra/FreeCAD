// Standalone Win32 rename probe. No FreeCAD dependencies.
//
// Question: why does SetFileInformationByHandle(..., FileRenameInfo, ...) fail
// with ERROR_INVALID_PARAMETER for a same-directory sibling rename performed
// through a pinned parent directory handle?
//
// Variants:
//   A  RootDirectory = parent handle, FileName = leaf
//   B  RootDirectory = NULL,          FileName = leaf   (CWD deliberately elsewhere)
//   C  RootDirectory = NULL,          FileName = absolute destination
//   D  cross-directory control (RootDirectory = other-dir handle, FileName = leaf)
//
// Each variant runs for FileRenameInfo (3) and FileRenameInfoEx (22), with the
// destination absent and present, and with replace and no-replace.

#include <windows.h>

#include <cstdio>
#include <cstddef>
#include <string>
#include <vector>

namespace {

constexpr DWORD kRenameInfoEx = 22;           // FILE_INFO_BY_HANDLE_CLASS
constexpr DWORD kFlagReplaceIfExists = 0x001; // FILE_RENAME_FLAG_REPLACE_IF_EXISTS
constexpr DWORD kFlagPosixSemantics  = 0x002; // FILE_RENAME_FLAG_POSIX_SEMANTICS

struct RenameInfoEx
{
    DWORD flags;
    HANDLE rootDirectory;
    DWORD fileNameLength;
    wchar_t fileName[1];
};

// --- NT layer -------------------------------------------------------------
// SetFileInformationByHandle is a Win32 wrapper that does not honour a
// non-NULL RootDirectory. The relative-rename capability is an NT-layer
// feature, so variants E/F go straight to NtSetInformationFile to find out
// whether the pinned-parent guarantee is reachable at all on Windows.

struct IO_STATUS_BLOCK_T
{
    union {
        LONG Status;
        void* Pointer;
    };
    ULONG_PTR Information;
};

struct FILE_RENAME_INFORMATION_T
{
    union {
        BOOLEAN ReplaceIfExists;
        ULONG Flags;
    };
    HANDLE RootDirectory;
    ULONG FileNameLength;
    WCHAR FileName[1];
};

constexpr ULONG kFileRenameInformation = 10;
constexpr ULONG kFileRenameInformationEx = 65;

using NtSetInformationFileFn = LONG(NTAPI*)(HANDLE, IO_STATUS_BLOCK_T*, void*, ULONG, ULONG);

NtSetInformationFileFn ntSetInformationFile()
{
    static NtSetInformationFileFn fn = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtSetInformationFileFn>(
                   reinterpret_cast<void*>(GetProcAddress(ntdll, "NtSetInformationFile")))
                     : nullptr;
    }();
    return fn;
}

std::wstring gRoot;

std::string narrow(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::string identityOf(const std::wstring& path)
{
    const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return "<absent>";
    }
    BY_HANDLE_FILE_INFORMATION info {};
    std::string result = "<unknown>";
    if (GetFileInformationByHandle(handle, &info)) {
        char buffer[128];
        std::snprintf(buffer, sizeof buffer, "%08lx:%08lx%08lx",
                      info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow);
        result = buffer;
    }
    CloseHandle(handle);
    return result;
}

bool writeFile(const std::wstring& path, const char* content)
{
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    WriteFile(handle, content, static_cast<DWORD>(strlen(content)), &written, nullptr);
    CloseHandle(handle);
    return true;
}

// Source handle opened exactly as DocumentFileWriter opens its temporary.
HANDLE openSource(const std::wstring& path)
{
    return CreateFileW(path.c_str(),
                       GENERIC_READ | GENERIC_WRITE | DELETE,
                       FILE_SHARE_READ | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Parent handle opened exactly as PinnedDirectory opens it.
HANDLE openParent(const std::wstring& path)
{
    return CreateFileW(path.c_str(),
                       FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_READ_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

struct Case
{
    const char* variant;
    bool useEx;
    bool destinationPresent;
    bool replace;
};

void runCase(const Case& item, int index)
{
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%s\\c%03d", gRoot.c_str(), index);
    CreateDirectoryW(dir, nullptr);

    wchar_t otherDir[MAX_PATH];
    swprintf_s(otherDir, L"%s\\c%03d_other", gRoot.c_str(), index);
    CreateDirectoryW(otherDir, nullptr);

    const std::wstring source = std::wstring(dir) + L"\\source.tmp";
    const bool crossDirectory = (item.variant[0] == 'D');
    const std::wstring destination =
        (crossDirectory ? std::wstring(otherDir) : std::wstring(dir)) + L"\\target.FCStd";

    writeFile(source, "payload");
    if (item.destinationPresent) {
        writeFile(destination, "existing");
    }
    const std::string sourceIdentity = identityOf(source);

    const HANDLE handle = openSource(source);
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf("%-3s ex=%d dst=%-7s repl=%d | OPEN SOURCE FAILED err=%lu\n",
                    item.variant, static_cast<int>(item.useEx),
                    item.destinationPresent ? "present" : "absent",
                    static_cast<int>(item.replace), GetLastError());
        return;
    }

    HANDLE parent = INVALID_HANDLE_VALUE;
    HANDLE root = nullptr;
    std::wstring name;

    if (item.variant[0] == 'A' || item.variant[0] == 'E' || item.variant[0] == 'F') {
        parent = openParent(dir);
        root = parent;
        name = L"target.FCStd";
    }
    else if (item.variant[0] == 'B') {
        root = nullptr;
        name = L"target.FCStd";
    }
    else if (item.variant[0] == 'C') {
        root = nullptr;
        name = destination;
    }
    else {  // D: cross-directory through the other directory's handle
        parent = openParent(otherDir);
        root = parent;
        name = L"target.FCStd";
    }

    const DWORD nameBytes = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    BOOL ok = FALSE;
    DWORD error = 0;
    size_t bytes = 0;
    size_t nameOffset = 0;

    LONG ntStatus = 0;
    const bool useNt = (item.variant[0] == 'E' || item.variant[0] == 'F');

    if (useNt) {
        const bool ntEx = (item.variant[0] == 'F');
        nameOffset = offsetof(FILE_RENAME_INFORMATION_T, FileName);
        bytes = nameOffset + nameBytes + sizeof(wchar_t);
        std::vector<std::byte> storage(bytes, std::byte {0});
        auto* info = reinterpret_cast<FILE_RENAME_INFORMATION_T*>(storage.data());
        if (ntEx) {
            info->Flags = (item.replace ? kFlagReplaceIfExists : 0U) | kFlagPosixSemantics;
        }
        else {
            info->ReplaceIfExists = item.replace ? TRUE : FALSE;
        }
        info->RootDirectory = root;
        info->FileNameLength = nameBytes;
        memcpy(info->FileName, name.data(), nameBytes);

        auto* fn = ntSetInformationFile();
        if (fn == nullptr) {
            std::printf("%-3s | NtSetInformationFile unavailable\n", item.variant);
            CloseHandle(handle);
            if (parent != INVALID_HANDLE_VALUE) {
                CloseHandle(parent);
            }
            return;
        }
        IO_STATUS_BLOCK_T iosb {};
        ntStatus = fn(handle, &iosb, info, static_cast<ULONG>(bytes),
                      ntEx ? kFileRenameInformationEx : kFileRenameInformation);
        ok = (ntStatus >= 0) ? TRUE : FALSE;
        if (!ok) {
            error = static_cast<DWORD>(ntStatus);
        }
    }
    else if (item.useEx) {
        nameOffset = offsetof(RenameInfoEx, fileName);
        bytes = nameOffset + nameBytes + sizeof(wchar_t);
        std::vector<std::byte> storage(bytes, std::byte {0});
        auto* info = reinterpret_cast<RenameInfoEx*>(storage.data());
        info->flags = (item.replace ? kFlagReplaceIfExists : 0U) | kFlagPosixSemantics;
        info->rootDirectory = root;
        info->fileNameLength = nameBytes;
        memcpy(info->fileName, name.data(), nameBytes);
        ok = SetFileInformationByHandle(handle,
                                        static_cast<FILE_INFO_BY_HANDLE_CLASS>(kRenameInfoEx),
                                        info, static_cast<DWORD>(bytes));
    }
    else {
        nameOffset = offsetof(FILE_RENAME_INFO, FileName);
        bytes = nameOffset + nameBytes + sizeof(wchar_t);
        std::vector<std::byte> storage(bytes, std::byte {0});
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        info->ReplaceIfExists = item.replace ? TRUE : FALSE;
        info->RootDirectory = root;
        info->FileNameLength = nameBytes;
        memcpy(info->FileName, name.data(), nameBytes);
        ok = SetFileInformationByHandle(handle, FileRenameInfo, info, static_cast<DWORD>(bytes));
    }
    if (!ok) {
        error = GetLastError();
    }

    CloseHandle(handle);
    if (parent != INVALID_HANDLE_VALUE) {
        CloseHandle(parent);
    }

    const std::string afterSource = identityOf(source);
    const std::string afterDestination = identityOf(destination);
    const bool movedCorrectly = ok && afterSource == "<absent>" && afterDestination == sourceIdentity;

    if (useNt) {
        std::printf("%-3s nt   dst=%-7s repl=%d | %-4s status=0x%08lX bytes=%-3zu nameLen=%-3lu "
                    "| src=%-12s dst=%-12s | %s\n",
                    item.variant, item.destinationPresent ? "present" : "absent",
                    static_cast<int>(item.replace), ok ? "OK" : "FAIL",
                    static_cast<unsigned long>(ntStatus), bytes, nameBytes,
                    afterSource.c_str(), afterDestination.c_str(),
                    ok ? (movedCorrectly ? "moved-correctly" : "MOVED-WRONG") : "-");
        return;
    }
    std::printf("%-3s ex=%d dst=%-7s repl=%d | %-4s err=%-5lu bytes=%-3zu nameOff=%-3zu "
                "nameLen=%-3lu | src=%-12s dst=%-12s | %s\n",
                item.variant, static_cast<int>(item.useEx),
                item.destinationPresent ? "present" : "absent",
                static_cast<int>(item.replace),
                ok ? "OK" : "FAIL", error, bytes, nameOffset, nameBytes,
                afterSource.c_str(), afterDestination.c_str(),
                ok ? (movedCorrectly ? "moved-correctly" : "MOVED-WRONG") : "-");
}

}  // namespace

int main()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    wchar_t root[MAX_PATH];
    swprintf_s(root, L"%srenameprobe_%lu", temp, GetCurrentProcessId());
    CreateDirectoryW(root, nullptr);
    gRoot = root;

    // Deliberately put the process CWD somewhere other than the test dir, so
    // variant B cannot accidentally succeed via the current directory.
    SetCurrentDirectoryW(temp);

    wchar_t volumeName[MAX_PATH] = L"";
    wchar_t fileSystem[MAX_PATH] = L"";
    DWORD serial = 0, maxComponent = 0, flags = 0;
    wchar_t volumeRoot[MAX_PATH];
    swprintf_s(volumeRoot, L"%c:\\", root[0]);
    GetVolumeInformationW(volumeRoot, volumeName, MAX_PATH, &serial, &maxComponent, &flags,
                          fileSystem, MAX_PATH);

    OSVERSIONINFOEXW version {};
    version.dwOSVersionInfoSize = sizeof version;
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) {
            fn(reinterpret_cast<PRTL_OSVERSIONINFOW>(&version));
        }
    }

    std::printf("root       : %s\n", narrow(gRoot).c_str());
    std::printf("filesystem : %s  (volume %s, serial %08lx)\n",
                narrow(fileSystem).c_str(), narrow(volumeName).c_str(), serial);
    std::printf("os build   : %lu.%lu.%lu\n",
                version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    std::printf("cwd        : (set away from the test directory)\n");
    std::printf("source open: GENERIC_READ|GENERIC_WRITE|DELETE, share READ|DELETE\n");
    std::printf("parent open: FILE_LIST_DIRECTORY|FILE_ADD_FILE|FILE_READ_ATTRIBUTES, "
                "BACKUP_SEMANTICS\n");
    std::printf("sizeof(FILE_RENAME_INFO)=%zu offsetof(FileName)=%zu\n\n",
                sizeof(FILE_RENAME_INFO), offsetof(FILE_RENAME_INFO, FileName));

    std::printf("var ex  dst      repl | res  err   bytes nameOff nameLen | identities | outcome\n");
    std::printf("--------------------------------------------------------------------------------\n");

    int index = 0;
    for (const char* variant : {"A", "B", "C", "D", "E", "F"}) {
        const bool ntVariant = (variant[0] == 'E' || variant[0] == 'F');
        for (const bool useEx : {false, true}) {
            if (ntVariant && useEx) {
                continue;  // E/F select their own NT information class
            }
            for (const bool present : {false, true}) {
                for (const bool replace : {false, true}) {
                    if (present && !replace) {
                        // no-replace onto an existing destination must fail;
                        // keep it, it is the no-clobber proof
                    }
                    runCase(Case {variant, useEx, present, replace}, index++);
                }
            }
        }
        std::printf("\n");
    }
    return 0;
}
