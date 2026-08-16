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
 ***************************************************************************/

#include <cstring>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <zipios++/zipfile.h>

#include <QByteArray>
#include <QCryptographicHash>

#include <App/BackupPolicy.h>
#include <App/DocumentFileWriter.h>
#include <Base/FileInfo.h>
#include <Base/Writer.h>

#include <FCConfig.h>

#ifdef FC_OS_WIN32
// App_tests_run defines DATADIR as a string literal for the tests that need the
// data directory. The Windows SDK declares `enum tagDATADIR { ... } DATADIR;`
// in objidl.h, so leaving the macro defined turns that typedef name into a
// string and breaks the SDK header. This translation unit never uses DATADIR.
# undef DATADIR
# include <windows.h>
# include <Aclapi.h>
# include <winioctl.h>
#else
# include <fcntl.h>
# include <sys/stat.h>
# include <unistd.h>
#endif

#include "InitApplication.h"
#include <src/TempDirectory.h>

#if !defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
# error "DocumentFileWriter direct tests require the test-only DLL import surface"
#endif

#if defined(FreeCADApp_EXPORTS)
# error "App_tests_run must import, never export, the DocumentFileWriter test surface"
#endif

namespace fs = std::filesystem;

namespace
{

std::string pathToUtf8(const fs::path& path)
{
    return Base::FileInfo::pathToString(path);
}

void writeFile(const fs::path& path, const std::string& contents)
{
    std::ofstream stream(path, std::ios::out | std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream << contents;
    ASSERT_TRUE(stream.good());
}

void overwriteFileWithDeleteSharing(const fs::path& path,
                                    const std::string& contents)
{
#ifdef FC_OS_WIN32
    const HANDLE handle = CreateFileW(path.c_str(),
                                      GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE) << GetLastError();
    DWORD written = 0;
    ASSERT_LE(contents.size(), static_cast<std::size_t>(MAXDWORD));
    ASSERT_NE(WriteFile(handle,
                        contents.data(),
                        static_cast<DWORD>(contents.size()),
                        &written,
                        nullptr),
              0)
        << GetLastError();
    EXPECT_EQ(static_cast<std::size_t>(written), contents.size());
    CloseHandle(handle);
#else
    writeFile(path, contents);
#endif
}

#ifdef FC_OS_WIN32
/*!
 * Name the Win32 open failures this lane must tell apart.
 *
 * A sharing violation and a missing file are entirely different diagnoses:
 * the first means a live handle holds an incompatible access mask, the second
 * means the artifact is gone. Reporting either as a bare "could not open"
 * loses exactly the distinction the classification depends on.
 */
std::string describeWin32Error(const DWORD error)
{
    switch (error) {
        case ERROR_SUCCESS:
            return "ERROR_SUCCESS";
        case ERROR_FILE_NOT_FOUND:
            return "ERROR_FILE_NOT_FOUND";
        case ERROR_PATH_NOT_FOUND:
            return "ERROR_PATH_NOT_FOUND";
        case ERROR_ACCESS_DENIED:
            return "ERROR_ACCESS_DENIED";
        case ERROR_SHARING_VIOLATION:
            return "ERROR_SHARING_VIOLATION";
        case ERROR_LOCK_VIOLATION:
            return "ERROR_LOCK_VIOLATION";
        case ERROR_DELETE_PENDING:
            return "ERROR_DELETE_PENDING";
        default:
            return "error " + std::to_string(error);
    }
}

//! Try one read-only open under an exact share mode and report what Windows said.
std::string probeOpen(const fs::path& path, const DWORD sharing)
{
    const HANDLE handle = CreateFileW(path.c_str(),
                                      GENERIC_READ,
                                      sharing,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return "failed, " + describeWin32Error(GetLastError());
    }
    CloseHandle(handle);
    return "opened";
}
#endif

std::string readFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream.is_open()) {
        // Diagnostic, not decoration: a bare is_open() failure cannot
        // distinguish "the artifact was never created", "it was created under
        // another name", and "it was cleaned up too early". Report the
        // requested path, whether it exists, and the parent listing.
        std::error_code code;
        std::string listing;
        for (const auto& entry : fs::directory_iterator(path.parent_path(), code)) {
            std::error_code sizeCode;
            const auto size = fs::is_regular_file(entry.path(), sizeCode)
                ? fs::file_size(entry.path(), sizeCode)
                : 0U;
            listing += "\n      " + entry.path().filename().string() + "  ("
                + std::to_string(size) + " bytes)";
        }
        std::string shareModes;
#ifdef FC_OS_WIN32
        // Which share mode an existing handle blocks is the whole diagnosis, so
        // measure both rather than inferring one from the ifstream failure.
        shareModes = "\n    ordinary  : " + probeOpen(path, FILE_SHARE_READ | FILE_SHARE_WRITE)
            + "\n    delete-sh : "
            + probeOpen(path, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
#endif
        ADD_FAILURE() << "readFile could not open the requested artifact"
                      << "\n    requested : " << path.string()
                      << "\n    exists    : " << (fs::exists(path, code) ? "yes" : "no")
                      << shareModes << "\n    parent    : " << path.parent_path().string()
                      << "\n    contents  :" << (listing.empty() ? " <empty>" : listing);
        return {};
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/*!
 * Read a path the way any ordinary application would.
 *
 * This is the user-visible contract: the canonical FCStd, and recovery
 * evidence after the lease is relinquished, must be readable like this. It
 * deliberately uses plain std::ifstream, whose Windows share mode does not
 * permit FILE_SHARE_DELETE, so it fails against an artifact still held under
 * an active lease. That failure is the point.
 */
std::string readUserVisibleFile(const fs::path& path)
{
    return readFile(path);
}

/*!
 * Read an internal artifact that is still held by an active lease.
 *
 * A retained lease keeps DELETE access on Windows because exact rename and
 * delete authority is part of the retained-identity contract. An ordinary
 * reader cannot open such a file, so a test inspecting an artifact *before*
 * consumption or relinquishment must permit delete sharing.
 *
 * Only for artifacts that are genuinely active and internal -- never for the
 * canonical file, and never after relinquishment.
 */
std::string readActiveLeasedArtifact(const fs::path& path)
{
#ifdef FC_OS_WIN32
    // Prove the lease is actually active before reading through it. A positive
    // read on its own would pass just as happily against an unheld file, which
    // would make every use of this helper unfalsifiable: it could silently
    // become the reader for artifacts that ought to be ordinarily readable and
    // nothing would notice. The negative is the half that pins the contract.
    const std::string ordinary = probeOpen(path, FILE_SHARE_READ | FILE_SHARE_WRITE);
    EXPECT_EQ(ordinary, "failed, ERROR_SHARING_VIOLATION")
        << "readActiveLeasedArtifact expects an active lease on " << path.string()
        << ", but an ordinary reader was not blocked by one";

    const HANDLE handle = CreateFileW(path.c_str(),
                                      GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        ADD_FAILURE() << "readActiveLeasedArtifact could not open " << path.string()
                      << " (GetLastError=" << GetLastError() << ")";
        return {};
    }
    std::string contents;
    for (;;) {
        char buffer[4096];
        DWORD read = 0;
        if (ReadFile(handle, buffer, sizeof buffer, &read, nullptr) == 0) {
            ADD_FAILURE() << "readActiveLeasedArtifact failed reading " << path.string()
                          << " (GetLastError=" << GetLastError() << ")";
            break;
        }
        if (read == 0) {
            break;
        }
        contents.append(buffer, read);
    }
    CloseHandle(handle);
    return contents;
#else
    // POSIX holds no equivalent mandatory lock, so an ordinary read suffices.
    return readFile(path);
#endif
}

std::string sha256(const std::string& contents)
{
    return QCryptographicHash::hash(QByteArray::fromStdString(contents),
                                    QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

bool warningContains(const App::Internal::DocumentFileReplacementResult& result,
                     const std::string& text)
{
    for (const auto& warning : result.warnings) {
        if (warning.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

#ifdef FC_OS_WIN32
struct WindowsDaclSnapshot
{
    std::vector<std::byte> bytes;
    bool protectedDacl {false};
};

WindowsDaclSnapshot readWindowsDacl(const fs::path& path)
{
    auto nativePath = path.native();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    const DWORD status = GetNamedSecurityInfoW(nativePath.data(),
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
                                "Unable to inspect test DACL");
    }
    WindowsDaclSnapshot result;
    SECURITY_DESCRIPTOR_CONTROL control {};
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision)) {
        const DWORD error = GetLastError();
        LocalFree(descriptor);
        throw std::system_error(static_cast<int>(error),
                                std::system_category(),
                                "Unable to inspect test DACL protection");
    }
    result.protectedDacl = (control & SE_DACL_PROTECTED) != 0;
    if (dacl) {
        result.bytes.resize(dacl->AclSize);
        std::memcpy(result.bytes.data(), dacl, dacl->AclSize);
    }
    LocalFree(descriptor);
    return result;
}

DWORD protectWindowsDacl(const fs::path& path)
{
    auto nativePath = path.native();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    DWORD status = GetNamedSecurityInfoW(nativePath.data(),
                                         SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION,
                                         nullptr,
                                         nullptr,
                                         &dacl,
                                         nullptr,
                                         &descriptor);
    if (status == ERROR_SUCCESS) {
        status = SetNamedSecurityInfoW(nativePath.data(),
                                       SE_FILE_OBJECT,
                                       DACL_SECURITY_INFORMATION
                                           | PROTECTED_DACL_SECURITY_INFORMATION,
                                       nullptr,
                                       nullptr,
                                       dacl,
                                       nullptr);
    }
    if (descriptor) {
        LocalFree(descriptor);
    }
    return status;
}

bool applyWindowsControl(const fs::path& path,
                         const DWORD control,
                         void* input,
                         const DWORD inputSize)
{
    const HANDLE handle = CreateFileW(path.c_str(),
                                      GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD returned = 0;
    const bool applied = DeviceIoControl(handle,
                                         control,
                                         input,
                                         inputSize,
                                         nullptr,
                                         0,
                                         &returned,
                                         nullptr)
        != 0;
    CloseHandle(handle);
    return applied;
}
#endif

class DocumentFileWriterTest: public ::testing::Test
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

    fs::path path(const std::string& name) const
    {
        return tempDir.path() / name;
    }

    void serialize(App::Internal::DocumentFileWriter& writer, const std::string& contents)
    {
        auto& stream = writer.serializationStream();
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    App::Internal::DocumentFileReplacementResult makeDisplacedLease(
        const fs::path& destination,
        const std::string& oldBytes = "old bytes",
        const std::string& newBytes = "new bytes")
    {
        writeFile(destination, oldBytes);
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.preserveDisplacedFile = true;
        App::Internal::DocumentFileWriter writer(std::move(request));
        serialize(writer, newBytes);
        return writer.commit();
    }

    tests::TempDirectory tempDir {"fc_document_file_writer"};
};

TEST_F(DocumentFileWriterTest, RequestDecoratorRunsOncePerConstructionAndScopesByGuard)
{
    int outerCalls = 0;
    int innerCalls = 0;
    auto outer = App::Internal::setDocumentFileWriterRequestDecoratorForTesting(
        [&](App::Internal::DocumentFileReplacementRequest&) { ++outerCalls; });
    ASSERT_TRUE(outer);

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(path("decorated-outer-one.FCStd"));
        App::Internal::DocumentFileWriter writer(std::move(request));
    }
    EXPECT_EQ(outerCalls, 1);

    auto inner = App::Internal::setDocumentFileWriterRequestDecoratorForTesting(
        [&](App::Internal::DocumentFileReplacementRequest&) { ++innerCalls; });
    ASSERT_TRUE(inner);
    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(path("decorated-inner.FCStd"));
        App::Internal::DocumentFileWriter writer(std::move(request));
    }
    EXPECT_EQ(outerCalls, 1);
    EXPECT_EQ(innerCalls, 1);

    inner.reset();
    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(path("decorated-outer-two.FCStd"));
        App::Internal::DocumentFileWriter writer(std::move(request));
    }
    EXPECT_EQ(outerCalls, 2);
    EXPECT_EQ(innerCalls, 1);

    outer.reset();
    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(path("decorator-reset.FCStd"));
        App::Internal::DocumentFileWriter writer(std::move(request));
    }
    EXPECT_EQ(outerCalls, 2);
    EXPECT_EQ(innerCalls, 1);
}

TEST_F(DocumentFileWriterTest, RequestDecoratorInjectsDurabilityFaultThenResetsCleanly)
{
    const auto destination = path("decorated-durability.FCStd");
    writeFile(destination, "old bytes");
    int calls = 0;
    auto decorator = App::Internal::setDocumentFileWriterRequestDecoratorForTesting(
        [&](App::Internal::DocumentFileReplacementRequest& request) {
            ++calls;
            request.testFault = App::Internal::DocumentFileWriterTestFault::BeforeDurabilityFlush;
        });
    ASSERT_TRUE(decorator);

    App::Internal::DocumentFileReplacementResult failed;
    {
        App::Internal::DocumentFileReplacementRequest failingRequest;
        failingRequest.destination = pathToUtf8(destination);
        App::Internal::DocumentFileWriter failingWriter(std::move(failingRequest));
        serialize(failingWriter, "installed but unverified durability");
        failed = failingWriter.commit();
    }

    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(failed.succeeded());
    EXPECT_TRUE(failed.replacementCompleted);
    EXPECT_TRUE(failed.replacementVerified);
    EXPECT_FALSE(failed.durabilityVerified);
    EXPECT_TRUE(failed.fileWritten);
    EXPECT_EQ(failed.errorCode, "TEST_INJECTED_DURABILITY_FAILURE");
    EXPECT_EQ(readFile(destination), "installed but unverified durability");

    decorator.reset();
    App::Internal::DocumentFileReplacementResult healthy;
    {
        App::Internal::DocumentFileReplacementRequest healthyRequest;
        healthyRequest.destination = pathToUtf8(destination);
        App::Internal::DocumentFileWriter healthyWriter(std::move(healthyRequest));
        serialize(healthyWriter, "healthy bytes");
        healthy = healthyWriter.commit();
    }

    EXPECT_EQ(calls, 1);
    ASSERT_TRUE(healthy.succeeded()) << healthy.errorCode << ": " << healthy.message;
    EXPECT_EQ(readFile(destination), "healthy bytes");
}

TEST_F(DocumentFileWriterTest, ReplaceUsesSiblingAndRetainsDisplacedSnapshot)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "old bytes");

    App::Internal::DocumentFileReplacementResult result;
    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.preserveDisplacedFile = true;
        App::Internal::DocumentFileWriter writer(std::move(request));

        const auto temporary = Base::FileInfo::stringToPath(writer.temporaryPath());
        EXPECT_EQ(temporary.parent_path(), destination.parent_path());
        serialize(writer, "new bytes");

        result = writer.commit();
    }

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    EXPECT_TRUE(result.replacementCompleted);
    EXPECT_TRUE(result.replacementVerified);
    EXPECT_TRUE(result.durabilityVerified);
    EXPECT_TRUE(result.fileWritten);
    // Canonical file: must stay readable by an ordinary reader even while the
    // result and its displaced lease are still alive.
    EXPECT_EQ(readUserVisibleFile(destination), "new bytes");
    ASSERT_FALSE(result.displacedFile.empty());
    // Displaced snapshot: an active internal lease artifact, so delete sharing
    // is required to inspect it before consumption or relinquishment.
    EXPECT_EQ(readActiveLeasedArtifact(Base::FileInfo::stringToPath(result.displacedFile)),
              "old bytes");
}

TEST_F(DocumentFileWriterTest, AbandonedSerializationNeverTouchesDestination)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "old bytes");
    std::string temporary;
    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.preserveDisplacedFile = false;
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "partial serialized bytes");
        ASSERT_TRUE(fs::exists(Base::FileInfo::stringToPath(temporary)));
        // A serialization exception would unwind here without commit().
    }

    EXPECT_EQ(readFile(destination), "old bytes");
    EXPECT_FALSE(fs::exists(Base::FileInfo::stringToPath(temporary)));
}

TEST_F(DocumentFileWriterTest, RetainedSerializationStreamSupportsZipBackpatchSeeks)
{
    const auto destination = path("seekable.FCStd");
    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    App::Internal::DocumentFileWriter writer(std::move(request));
    auto& stream = writer.serializationStream();

    stream.write("abcd", 4);
    EXPECT_EQ(stream.tellp(), std::streampos(4));
    stream.seekp(1, std::ios_base::beg);
    stream.put('X');
    stream.seekp(0, std::ios_base::end);
    stream.put('e');
    stream.seekp(-2, std::ios_base::cur);
    stream.put('Z');

    const auto result = writer.commit();

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    EXPECT_EQ(readFile(destination), "aXcZe");
}

TEST_F(DocumentFileWriterTest, ZipWriterSerializesThroughRetainedSeekableHandle)
{
    const auto destination = path("archive.FCStd");
    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    App::Internal::DocumentFileWriter fileWriter(std::move(request));
    {
        Base::ZipWriter zipWriter(fileWriter.serializationStream());
        zipWriter.putNextEntry("Document.xml");
        zipWriter.Stream() << "<Document/>";
        zipWriter.putNextEntry("Payload.txt");
        zipWriter.Stream() << "payload";
    }

    const auto result = fileWriter.commit();

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    zipios::ZipFile archive(pathToUtf8(destination));
    ASSERT_TRUE(archive.isValid());
    EXPECT_EQ(archive.entries().size(), 2U);
    EXPECT_TRUE(archive.getEntry("Document.xml") != nullptr);
    EXPECT_TRUE(archive.getEntry("Payload.txt") != nullptr);
}

TEST_F(DocumentFileWriterTest, ReservedHandleRemainsAuthoritativeWhenPathMovesBeforeSerialization)
{
    const auto destination = path("document.FCStd");
    const auto movedTemporary = path("owned-before-serialization.tmp");
    writeFile(destination, "old bytes");

    std::string diagnosticPath;
    App::Internal::DocumentFileReplacementResult result;
    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        App::Internal::DocumentFileWriter writer(std::move(request));
        diagnosticPath = writer.temporaryPath();
        std::error_code error;
        fs::rename(Base::FileInfo::stringToPath(diagnosticPath), movedTemporary, error);
        ASSERT_FALSE(error) << error.message();
        writeFile(Base::FileInfo::stringToPath(diagnosticPath), "foreign sentinel");
        serialize(writer, "owned serialized bytes");

        result = writer.commit();
    }

    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(result.errorCode, "TEMPORARY_FILE_CHANGED");
    EXPECT_EQ(readFile(destination), "old bytes");
#ifdef FC_OS_WIN32
    // Cleanup is bound to the retained HANDLE and removes the moved owned
    // object without touching the foreign file at the diagnostic pathname.
    EXPECT_FALSE(fs::exists(movedTemporary));
#else
    // Portable POSIX cleanup is deliberately parent/leaf identity-bound. It
    // will not guess the new name of an externally moved inode.
    EXPECT_EQ(readFile(movedTemporary), "owned serialized bytes");
#endif
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(diagnosticPath)), "foreign sentinel");
    std::error_code cleanupError;
    fs::remove(movedTemporary, cleanupError);
    fs::remove(Base::FileInfo::stringToPath(diagnosticPath), cleanupError);
}

#ifdef FC_OS_WIN32
TEST_F(DocumentFileWriterTest, AbandonedReadOnlyRestrictedDaclTemporaryIsRemovedByOwnedHandle)
{
    const auto destination = path("readonly-cleanup.FCStd");
    writeFile(destination, "old bytes");
    std::string temporary;
    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.testFault =
            App::Internal::DocumentFileWriterTestFault::CleanupReadOnlyAndRestrictedDacl;
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
    }

    EXPECT_FALSE(fs::exists(Base::FileInfo::stringToPath(temporary)));
    EXPECT_EQ(readFile(destination), "old bytes");
}
#endif

class DocumentFileWriterSerializationFaultTest:
    public DocumentFileWriterTest,
    public ::testing::WithParamInterface<App::Internal::DocumentFileWriterTestFault>
{};

TEST_P(DocumentFileWriterSerializationFaultTest, LeavesDestinationUntouched)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "old bytes");
    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.testFault = GetParam();
    App::Internal::DocumentFileWriter writer(std::move(request));
    auto& stream = writer.serializationStream();
    if (GetParam() == App::Internal::DocumentFileWriterTestFault::SerializationSeek) {
        stream.seekp(0, std::ios_base::beg);
    }
    else if (GetParam() == App::Internal::DocumentFileWriterTestFault::SerializationSync) {
        stream.flush();
    }
    else {
        stream.write("serialized bytes", 16);
    }

    const auto result = writer.commit();

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "SERIALIZATION_IO_FAILED");
    EXPECT_EQ(readFile(destination), "old bytes");
}

INSTANTIATE_TEST_SUITE_P(
    RetainedStream,
    DocumentFileWriterSerializationFaultTest,
    ::testing::Values(App::Internal::DocumentFileWriterTestFault::SerializationWrite,
                      App::Internal::DocumentFileWriterTestFault::SerializationSeek,
                      App::Internal::DocumentFileWriterTestFault::SerializationSync));

TEST_F(DocumentFileWriterTest, ZipWriterDestructorDoesNotThrowOnLatchedSyncFailure)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "old bytes");
    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.testFault = App::Internal::DocumentFileWriterTestFault::SerializationSync;
    App::Internal::DocumentFileWriter writer(std::move(request));
    {
        Base::ZipWriter zipWriter(writer.serializationStream());
        zipWriter.putNextEntry("Document.xml");
        zipWriter.Stream() << "<Document/>";
    }

    const auto result = writer.commit();

    EXPECT_EQ(result.errorCode, "SERIALIZATION_IO_FAILED");
    EXPECT_EQ(readFile(destination), "old bytes");
}

TEST_F(DocumentFileWriterTest, NoReplaceRejectsDestinationCreatedAfterSerialization)
{
    const auto destination = path("new-document.FCStd");
    std::string temporary;
    App::Internal::DocumentFileReplacementResult result;
    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::NoReplace;
        request.beforeReplacementPrimitive = [&] {
            writeFile(destination, "late conflicting bytes");
        };
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized bytes");

        result = writer.commit();

        EXPECT_TRUE(fs::exists(Base::FileInfo::stringToPath(temporary)));
    }

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "DESTINATION_EXISTS");
    EXPECT_EQ(readFile(destination), "late conflicting bytes");
    // Contract 2.2: once verified, the serialized bytes are the only copy of
    // the work being saved, so they are retained and reported, never removed.
    EXPECT_TRUE(fs::exists(Base::FileInfo::stringToPath(temporary)));
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)), "serialized bytes");
    EXPECT_TRUE(warningContains(result, temporary));
}

// A rename can succeed while the destination name stays unresolvable for as
// long as this process holds a descriptor on the installed inode. The 9p bind
// mount does exactly that. Simulated here so the release-then-re-resolve path
// is covered deterministically on every filesystem.
TEST_F(DocumentFileWriterTest, PathInvisibleUntilDescriptorReleaseStillVerifies)
{
    const auto destination = path("invisible-until-release.FCStd");
    writeFile(destination, "old bytes");
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.simulatePathInvisibleUntilDescriptorRelease = true;
        App::Internal::DocumentFileWriter writer(std::move(request));
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    EXPECT_TRUE(result.fileWritten);
    EXPECT_TRUE(result.replacementCompleted);
    EXPECT_TRUE(result.replacementVerified);
    EXPECT_TRUE(result.durabilityVerified);
    EXPECT_EQ(readFile(destination), "serialized replacement bytes");
}

TEST_F(DocumentFileWriterTest, SubstitutionAfterDescriptorReleaseFailsClosedAndKeepsForeignEntry)
{
    const auto destination = path("substituted-after-release.FCStd");
    writeFile(destination, "old bytes");
    bool hookCalled = false;
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.simulatePathInvisibleUntilDescriptorRelease = true;
        // Substitute the destination in the window the release opens up.
        request.afterInstalledDescriptorRelease = [&](const std::string& value) {
            hookCalled = true;
            writeFile(Base::FileInfo::stringToPath(value), "foreign substituted bytes");
        };
        App::Internal::DocumentFileWriter writer(std::move(request));
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    ASSERT_TRUE(hookCalled);
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.replacementVerified);
    EXPECT_FALSE(result.durabilityVerified);
    EXPECT_EQ(result.errorCode, "REPLACEMENT_VERIFICATION_FAILED");
    // The substituted entry is foreign; it must be left exactly as found.
    EXPECT_EQ(readFile(destination), "foreign substituted bytes");
}

TEST_F(DocumentFileWriterTest, CompareAndSwapAcceptsMatchingDestinationHash)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "expected old bytes");

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
    request.expectedDestinationSha256 = sha256("expected old bytes");
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "replacement bytes");

    const auto result = writer.commit();

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    EXPECT_TRUE(result.fileWritten);
    EXPECT_EQ(readFile(destination), "replacement bytes");
    // Contract 2.3: a successful compare-and-swap retains the exact previous
    // destination and hands it to BackupPolicy as a DisplacedCanonical rather
    // than discarding it. See CompareAndSwapRetainsExactMovedPredecessorForBackup.
    ASSERT_FALSE(result.displacedFile.empty());
    EXPECT_TRUE(result.displacedFileLease);
    EXPECT_EQ(readActiveLeasedArtifact(Base::FileInfo::stringToPath(result.displacedFile)),
              "expected old bytes");
}

TEST_F(DocumentFileWriterTest, CompareAndSwapRetainsExactMovedPredecessorForBackup)
{
    const auto destination = path("cas-with-backup.FCStd");
    writeFile(destination, "expected old bytes");

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
    request.expectedDestinationSha256 = sha256("expected old bytes");
    request.preserveDisplacedFile = true;
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "replacement bytes");

    const auto result = writer.commit();

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    EXPECT_TRUE(result.fileWritten);
    EXPECT_EQ(readFile(destination), "replacement bytes");
    ASSERT_FALSE(result.displacedFile.empty());
    EXPECT_EQ(readActiveLeasedArtifact(Base::FileInfo::stringToPath(result.displacedFile)),
              "expected old bytes");
}

TEST_F(DocumentFileWriterTest, CompareAndSwapRejectsMismatchingDestinationHash)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "expected old bytes");

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
    request.expectedDestinationSha256 = sha256("expected old bytes");
    request.beforeFinalBoundaryValidation = [&] {
        overwriteFileWithDeleteSharing(destination, "late unexpected bytes");
    };
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "replacement bytes");

    const auto result = writer.commit();

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "DESTINATION_CHANGED");
    EXPECT_EQ(readFile(destination), "late unexpected bytes");
}

TEST_F(DocumentFileWriterTest, CompareAndSwapRehashesAtFinalBoundary)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "aaaaaaaa");
    std::error_code timeError;
    const auto originalModified = fs::last_write_time(destination, timeError);
    ASSERT_FALSE(timeError) << timeError.message();

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
    request.expectedDestinationSha256 = sha256("aaaaaaaa");
    request.afterFinalBoundaryValidationBeforeReplace = [&] {
        overwriteFileWithDeleteSharing(destination, "bbbbbbbb");
        fs::last_write_time(destination, originalModified, timeError);
    };
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "replacement bytes");

    const auto result = writer.commit();

    ASSERT_FALSE(timeError) << timeError.message();
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "DESTINATION_CHANGED");
    EXPECT_EQ(readFile(destination), "bbbbbbbb");
}

TEST_F(DocumentFileWriterTest, CompareAndSwapRejectsSwapAtReplacementPrimitive)
{
    const auto destination = path("cas-before-primitive.FCStd");
    const auto movedExpected = path("cas-before-primitive-expected");
    writeFile(destination, "expected old bytes");
    std::error_code hookError;
    std::string temporary;
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.beforeReplacementPrimitive = [&] {
            fs::rename(destination, movedExpected, hookError);
            if (!hookError) {
                writeFile(destination, "foreign canonical bytes");
            }
        };
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    ASSERT_FALSE(hookError) << hookError.message();
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "DESTINATION_CHANGED");
    EXPECT_EQ(readFile(destination), "foreign canonical bytes");
    EXPECT_EQ(readFile(movedExpected), "expected old bytes");
    // Contract 2.2: the verified serialization is retained and reported.
    EXPECT_TRUE(fs::exists(Base::FileInfo::stringToPath(temporary)));
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)),
              "serialized replacement bytes");
    EXPECT_TRUE(warningContains(result, temporary));
}

TEST_F(DocumentFileWriterTest, CompareAndSwapGuardMoveNeverClobbersCollision)
{
    const auto destination = path("cas-guard-collision.FCStd");
    writeFile(destination, "expected old bytes");
    std::string guard;
    std::string temporary;
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.beforeCompareAndSwapGuardMove = [&](const std::string& value) {
            guard = value;
            writeFile(Base::FileInfo::stringToPath(value), "foreign guard collision");
        };
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    ASSERT_FALSE(guard.empty());
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "COMPARE_AND_SWAP_GUARD_COLLISION");
    EXPECT_NE(result.message.find(guard), std::string::npos);
    EXPECT_EQ(readFile(destination), "expected old bytes");
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(guard)),
              "foreign guard collision");
    // Contract 2.2: the verified serialization is retained and reported.
    EXPECT_TRUE(fs::exists(Base::FileInfo::stringToPath(temporary)));
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)),
              "serialized replacement bytes");
    EXPECT_TRUE(warningContains(result, temporary));
}

#ifndef FC_OS_WIN32
TEST_F(DocumentFileWriterTest, CompareAndSwapRejectsGuardSourceLeafSwapAndRetainsAllBytes)
{
    const auto destination = path("cas-guard-source-swap.FCStd");
    const auto movedExpected = path("cas-guard-source-swap-expected");
    writeFile(destination, "expected old bytes");
    std::string guard;
    std::string temporary;
    std::error_code hookError;
    int hookCalls = 0;
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.afterStrictNoReplaceSourceValidationBeforeRename =
            [&](const std::string& source, const std::string& target) {
                ++hookCalls;
                guard = target;
                fs::rename(Base::FileInfo::stringToPath(source), movedExpected, hookError);
                if (!hookError) {
                    writeFile(Base::FileInfo::stringToPath(source),
                              "foreign guard-source bytes");
                }
            };
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    ASSERT_FALSE(hookError) << hookError.message();
    EXPECT_EQ(hookCalls, 1);
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.replacementCompleted);
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "STRICT_NO_REPLACE_SOURCE_CHANGED");
    EXPECT_FALSE(fs::exists(destination));
    EXPECT_EQ(readFile(movedExpected), "expected old bytes");
    ASSERT_FALSE(guard.empty());
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(guard)),
              "foreign guard-source bytes");
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)),
              "serialized replacement bytes");
    ASSERT_FALSE(result.displacedFile.empty());
    EXPECT_TRUE(result.displacedFileLease);
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(result.displacedFile)),
              "expected old bytes");
    EXPECT_TRUE(warningContains(result, result.displacedFile));
    EXPECT_TRUE(warningContains(result, guard));
    EXPECT_TRUE(warningContains(result, temporary));
}
#endif

TEST_F(DocumentFileWriterTest, CompareAndSwapRevalidatesAfterGuardMove)
{
    const auto destination = path("cas-after-guard-move.FCStd");
    writeFile(destination, "expected old bytes");
    std::string temporary;
    std::string guard;
    bool hookCalled = false;
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.afterCompareAndSwapGuardMove = [&](const std::string& value) {
            hookCalled = true;
            guard = value;
            writeFile(destination, "foreign after-guard bytes");
        };
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    ASSERT_TRUE(hookCalled);
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "DESTINATION_CHANGED");
    EXPECT_EQ(result.displacedFile, guard);
    ASSERT_FALSE(guard.empty());
    EXPECT_EQ(readActiveLeasedArtifact(Base::FileInfo::stringToPath(guard)), "expected old bytes");
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)),
              "serialized replacement bytes");
    EXPECT_EQ(readFile(destination), "foreign after-guard bytes");
    EXPECT_TRUE(warningContains(result, guard));
    EXPECT_TRUE(warningContains(result, temporary));
}

TEST_F(DocumentFileWriterTest, CompareAndSwapInstallUsesNoReplaceAfterGuardValidation)
{
    const auto destination = path("cas-before-install.FCStd");
    writeFile(destination, "expected old bytes");
    std::string temporary;
    std::string guard;
    bool hookCalled = false;
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.afterCompareAndSwapGuardValidationBeforeInstall =
            [&](const std::string& value) {
                hookCalled = true;
                guard = value;
                writeFile(destination, "foreign before-install bytes");
            };
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    ASSERT_TRUE(hookCalled);
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "DESTINATION_CHANGED");
    EXPECT_EQ(result.displacedFile, guard);
    EXPECT_EQ(readActiveLeasedArtifact(Base::FileInfo::stringToPath(guard)), "expected old bytes");
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)),
              "serialized replacement bytes");
    EXPECT_EQ(readFile(destination), "foreign before-install bytes");
}

#ifndef FC_OS_WIN32
TEST_F(DocumentFileWriterTest, CompareAndSwapRejectsInstallSourceLeafSwapAndRetainsAllBytes)
{
    const auto destination = path("cas-install-source-swap.FCStd");
    const auto movedSerialized = path("cas-install-source-swap-serialized");
    writeFile(destination, "expected old bytes");
    std::string temporary;
    std::string guard;
    std::error_code hookError;
    int hookCalls = 0;
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.afterCompareAndSwapGuardMove = [&](const std::string& value) {
            guard = value;
        };
        request.afterStrictNoReplaceSourceValidationBeforeRename =
            [&](const std::string& source, const std::string& target) {
                ++hookCalls;
                if (Base::FileInfo::stringToPath(target) != destination) {
                    return;
                }
                fs::rename(Base::FileInfo::stringToPath(source), movedSerialized, hookError);
                if (!hookError) {
                    writeFile(Base::FileInfo::stringToPath(source),
                              "foreign install-source bytes");
                }
            };
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    ASSERT_FALSE(hookError) << hookError.message();
    EXPECT_EQ(hookCalls, 2);
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.replacementCompleted);
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "STRICT_NO_REPLACE_SOURCE_CHANGED");
    EXPECT_EQ(readFile(destination), "foreign install-source bytes");
    ASSERT_FALSE(guard.empty());
    EXPECT_EQ(result.displacedFile, guard);
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(guard)), "expected old bytes");
    EXPECT_EQ(readFile(movedSerialized), "serialized replacement bytes");
    EXPECT_FALSE(fs::exists(Base::FileInfo::stringToPath(temporary)));

    std::vector<fs::path> serializedRecoveryCopies;
    for (const auto& entry : fs::directory_iterator(tempDir.path())) {
        if (entry.path().filename().string().find(".cas-serialized-recovery")
            != std::string::npos) {
            serializedRecoveryCopies.push_back(entry.path());
        }
    }
    ASSERT_EQ(serializedRecoveryCopies.size(), 1U);
    EXPECT_EQ(readFile(serializedRecoveryCopies.front()), "serialized replacement bytes");
    EXPECT_TRUE(warningContains(result, pathToUtf8(serializedRecoveryCopies.front())));
    EXPECT_TRUE(warningContains(result, guard));
}
#endif

TEST_F(DocumentFileWriterTest, CompareAndSwapRestoreNeverClobbersLateOccupant)
{
    const auto destination = path("cas-restore-occupied.FCStd");
    writeFile(destination, "expected old bytes");
    std::string temporary;
    std::string guard;
    bool restoreHookCalled = false;
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.testFault =
            App::Internal::DocumentFileWriterTestFault::CompareAndSwapGuardValidation;
        request.afterCompareAndSwapGuardMove = [&](const std::string& value) {
            guard = value;
        };
        request.beforeCompareAndSwapGuardRestore = [&](const std::string& value) {
            restoreHookCalled = true;
            EXPECT_EQ(value, guard);
            writeFile(destination, "foreign restore occupant");
        };
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    ASSERT_TRUE(restoreHookCalled);
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "DESTINATION_CHANGED");
    EXPECT_EQ(result.displacedFile, guard);
    EXPECT_EQ(readFile(destination), "foreign restore occupant");
    EXPECT_EQ(readActiveLeasedArtifact(Base::FileInfo::stringToPath(guard)), "expected old bytes");
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)),
              "serialized replacement bytes");
}

TEST_F(DocumentFileWriterTest, CompareAndSwapRestoresGuardAndRetainsTemporaryOnMismatch)
{
    const auto destination = path("cas-guard-mismatch.FCStd");
    writeFile(destination, "expected old bytes");
    std::string temporary;
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.testFault =
            App::Internal::DocumentFileWriterTestFault::CompareAndSwapGuardValidation;
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "DESTINATION_CHANGED");
    EXPECT_TRUE(result.displacedFile.empty());
    EXPECT_FALSE(result.displacedFileLease);
    EXPECT_EQ(readFile(destination), "expected old bytes");
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)),
              "serialized replacement bytes");
    EXPECT_TRUE(warningContains(result, temporary));
}

TEST_F(DocumentFileWriterTest, CompareAndSwapRestoreSwapKeepsForeignEntryAndNamesPredecessor)
{
    const auto destination = path("cas-restore-swap.FCStd");
    writeFile(destination, "expected old bytes");
    const auto swappedAway = path("cas-restore-swapped-away");
    std::string temporary;
    std::string guard;
    std::string recovery;
    bool restoreInspectionHookCalled = false;
    std::error_code hookError;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.testFault =
            App::Internal::DocumentFileWriterTestFault::CompareAndSwapGuardValidation;
        request.afterCompareAndSwapGuardMove = [&](const std::string& value) {
            guard = value;
        };
        // The guard has just been restored into the canonical name. Swap that
        // public name before the writer inspects it: the retained handle stays
        // the only authority for the exact predecessor bytes.
        request.afterCompareAndSwapGuardRestoreBeforeInspection =
            [&](const std::string&) {
                restoreInspectionHookCalled = true;
                fs::rename(destination, swappedAway, hookError);
                if (!hookError) {
                    writeFile(destination, "foreign restore occupant");
                }
            };
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        const auto result = writer.commit();

        ASSERT_TRUE(restoreInspectionHookCalled);
        ASSERT_FALSE(hookError) << hookError.message();
        EXPECT_FALSE(result.succeeded());
        EXPECT_FALSE(result.fileWritten);
        EXPECT_EQ(result.errorCode, "DESTINATION_CHANGED");
        ASSERT_FALSE(guard.empty());
        ASSERT_FALSE(result.displacedFile.empty());
        // The predecessor must be reported at a fresh name, never at the
        // swapped public name and never at the consumed guard name.
        EXPECT_NE(result.displacedFile, pathToUtf8(destination));
        EXPECT_NE(result.displacedFile, guard);
        EXPECT_TRUE(warningContains(result, result.displacedFile));
        recovery = result.displacedFile;
    }

    ASSERT_FALSE(recovery.empty());
    // Surviving result and lease destruction is what makes it evidence.
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(recovery)), "expected old bytes");
    EXPECT_EQ(readFile(destination), "foreign restore occupant");
    EXPECT_EQ(readFile(swappedAway), "expected old bytes");
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)),
              "serialized replacement bytes");
    EXPECT_FALSE(fs::exists(Base::FileInfo::stringToPath(guard)));
}

TEST_F(DocumentFileWriterTest, CompareAndSwapUnsupportedNoReplaceFailsBeforeMutation)
{
    const auto destination = path("cas-unsupported.FCStd");
    writeFile(destination, "expected old bytes");
    std::string temporary;
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.testFault =
            App::Internal::DocumentFileWriterTestFault::StrictNoReplaceUnavailable;
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
        EXPECT_TRUE(fs::exists(Base::FileInfo::stringToPath(temporary)));
    }

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "STRICT_NO_REPLACE_UNSUPPORTED");
    EXPECT_EQ(readFile(destination), "expected old bytes");
    // Contract 2.2: the verified serialization is retained and reported.
    EXPECT_TRUE(fs::exists(Base::FileInfo::stringToPath(temporary)));
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)),
              "serialized replacement bytes");
    EXPECT_TRUE(warningContains(result, temporary));
}

TEST_F(DocumentFileWriterTest, CompareAndSwapAuthorityFailureIsPreMutationAndSpecific)
{
    const auto destination = path("cas-authority-unavailable.FCStd");
    writeFile(destination, "expected old bytes");
    std::error_code timeError;
    const auto originalModified = fs::last_write_time(destination, timeError);
    ASSERT_FALSE(timeError) << timeError.message();
    std::string temporary;
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.testFault =
            App::Internal::DocumentFileWriterTestFault::CompareAndSwapAuthorityUnavailable;
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
        EXPECT_TRUE(fs::exists(Base::FileInfo::stringToPath(temporary)));
    }

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "COMPARE_AND_SWAP_AUTHORITY_UNAVAILABLE");
    EXPECT_EQ(readFile(destination), "expected old bytes");
    const auto resultingModified = fs::last_write_time(destination, timeError);
    EXPECT_FALSE(timeError) << timeError.message();
    EXPECT_TRUE(resultingModified == originalModified);
    // Contract 2.2: the verified serialization is retained and reported.
    EXPECT_TRUE(fs::exists(Base::FileInfo::stringToPath(temporary)));
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)),
              "serialized replacement bytes");
    EXPECT_TRUE(warningContains(result, temporary));
    for (const auto& entry : fs::directory_iterator(tempDir.path())) {
        EXPECT_EQ(entry.path().filename().string().find(".cas-recovery"),
                  std::string::npos);
    }
}

TEST_F(DocumentFileWriterTest, CompareAndSwapDurabilityFailureIsPreMutationAndSpecific)
{
    const auto destination = path("cas-durability-unavailable.FCStd");
    writeFile(destination, "expected old bytes");
    std::error_code timeError;
    const auto originalModified = fs::last_write_time(destination, timeError);
    ASSERT_FALSE(timeError) << timeError.message();
    std::string temporary;
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.testFault =
            App::Internal::DocumentFileWriterTestFault::CompareAndSwapDurabilityUnavailable;
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "COMPARE_AND_SWAP_DURABILITY_UNAVAILABLE");
    EXPECT_EQ(readFile(destination), "expected old bytes");
    const auto resultingModified = fs::last_write_time(destination, timeError);
    ASSERT_FALSE(timeError) << timeError.message();
    EXPECT_TRUE(resultingModified == originalModified);
    // Contract 2.2: the verified serialization is retained and reported.
    EXPECT_TRUE(fs::exists(Base::FileInfo::stringToPath(temporary)));
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)),
              "serialized replacement bytes");
    EXPECT_TRUE(warningContains(result, temporary));
}

TEST_F(DocumentFileWriterTest, CompareAndSwapRecoveryEvidenceSurvivesResultDestruction)
{
    const auto destination = path("cas-recovery-evidence.FCStd");
    writeFile(destination, "expected old bytes");
    std::string temporary;
    std::string guard;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.afterCompareAndSwapGuardValidationBeforeInstall =
            [&](const std::string& value) {
                guard = value;
                writeFile(destination, "foreign crash-race bytes");
            };
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized replacement bytes");
        {
            const auto result = writer.commit();
            EXPECT_FALSE(result.succeeded());
            EXPECT_EQ(result.displacedFile, guard);
        }
    }

    ASSERT_FALSE(guard.empty());
    EXPECT_EQ(readFile(destination), "foreign crash-race bytes");
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(guard)), "expected old bytes");
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)),
              "serialized replacement bytes");
}

TEST_F(DocumentFileWriterTest, CompareAndSwapPostInstallFailureReportsWrittenAndKeepsGuard)
{
    const auto destination = path("cas-post-install-failure.FCStd");
    writeFile(destination, "expected old bytes");
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.testFault =
            App::Internal::DocumentFileWriterTestFault::AfterReplacementBeforeVerification;
        App::Internal::DocumentFileWriter writer(std::move(request));
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(result.replacementCompleted);
    EXPECT_FALSE(result.replacementVerified);
    EXPECT_FALSE(result.durabilityVerified);
    EXPECT_TRUE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "TEST_INJECTED_REPLACEMENT_VERIFICATION_FAILURE");
    EXPECT_EQ(readFile(destination), "serialized replacement bytes");
    ASSERT_FALSE(result.displacedFile.empty());
    EXPECT_EQ(readActiveLeasedArtifact(Base::FileInfo::stringToPath(result.displacedFile)),
              "expected old bytes");
}

TEST_F(DocumentFileWriterTest, CompareAndSwapPostInstallGuardInspectionIsBestEffort)
{
    const auto destination = path("cas-post-install-guard-inspection.FCStd");
    writeFile(destination, "expected old bytes");
    App::Internal::DocumentFileReplacementResult result;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.testFault =
            App::Internal::DocumentFileWriterTestFault::CompareAndSwapPostInstallGuardInspection;
        App::Internal::DocumentFileWriter writer(std::move(request));
        serialize(writer, "serialized replacement bytes");
        result = writer.commit();
    }

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    EXPECT_TRUE(result.replacementCompleted);
    EXPECT_TRUE(result.replacementVerified);
    EXPECT_TRUE(result.durabilityVerified);
    EXPECT_TRUE(result.fileWritten);
    EXPECT_EQ(readFile(destination), "serialized replacement bytes");
    ASSERT_FALSE(result.displacedFile.empty());
    EXPECT_TRUE(result.displacedFileLease);
    EXPECT_EQ(readActiveLeasedArtifact(Base::FileInfo::stringToPath(result.displacedFile)),
              "expected old bytes");
    // The injected std::runtime_error path reports the exception text; the
    // bare "inspection failed" wording is only the catch(...) fallback.
    EXPECT_TRUE(warningContains(result, "guard ownership could not be proved"));
    EXPECT_TRUE(warningContains(result, result.displacedFile));
}

TEST_F(DocumentFileWriterTest, CompareAndSwapInstallGuardSwapKeepsForeignEntryAndNamesPredecessor)
{
    const auto destination = path("cas-post-install-guard-swap.FCStd");
    writeFile(destination, "expected old bytes");
    const auto swappedAway = path("cas-post-install-guard-swapped-away");
    std::string guard;
    std::string recovery;
    bool inspectionHookCalled = false;
    std::error_code hookError;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::CompareAndSwapSha256;
        request.expectedDestinationSha256 = sha256("expected old bytes");
        request.afterCompareAndSwapGuardMove = [&](const std::string& value) {
            guard = value;
        };
        // The serialized replacement is already installed and irreversible.
        // Swap the obsolete guard name before the writer inspects it for
        // cleanup; that pathname must then be left completely alone.
        request.afterCompareAndSwapInstallBeforeGuardInspection =
            [&](const std::string& value) {
                inspectionHookCalled = true;
                const auto guardPath = Base::FileInfo::stringToPath(value);
                fs::rename(guardPath, swappedAway, hookError);
                if (!hookError) {
                    writeFile(guardPath, "foreign guard occupant");
                }
            };
        App::Internal::DocumentFileWriter writer(std::move(request));
        serialize(writer, "serialized replacement bytes");
        const auto result = writer.commit();

        ASSERT_TRUE(inspectionHookCalled);
        ASSERT_FALSE(hookError) << hookError.message();
        ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
        EXPECT_TRUE(result.fileWritten);
        ASSERT_FALSE(guard.empty());
        ASSERT_FALSE(result.displacedFile.empty());
        EXPECT_NE(result.displacedFile, guard);
        EXPECT_TRUE(warningContains(result, "guard ownership could not be proved"));
        EXPECT_TRUE(warningContains(result, guard));
        EXPECT_TRUE(warningContains(result, result.displacedFile));
        recovery = result.displacedFile;
    }

    ASSERT_FALSE(recovery.empty());
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(recovery)), "expected old bytes");
    EXPECT_EQ(readFile(destination), "serialized replacement bytes");
    // The untrusted guard pathname must be left exactly as the swap left it.
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(guard)), "foreign guard occupant");
    EXPECT_EQ(readFile(swappedAway), "expected old bytes");
}

TEST_F(DocumentFileWriterTest, PostValidationPathSwapInstallsOwnedHandleOrFailsSafely)
{
    const auto destination = path("document.FCStd");
    const auto movedTemporary = path("captured-owned-temp");
    writeFile(destination, "old bytes");

    std::string temporary;
    std::error_code callbackError;
    App::Internal::DocumentFileReplacementResult result;
    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.afterFinalBoundaryValidationBeforeReplace = [&] {
            fs::rename(Base::FileInfo::stringToPath(temporary), movedTemporary, callbackError);
            if (!callbackError) {
                writeFile(Base::FileInfo::stringToPath(temporary), "foreign bytes");
            }
        };
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "serialized bytes");

        result = writer.commit();
    }

    ASSERT_FALSE(callbackError) << callbackError.message();
#ifdef FC_OS_WIN32
    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    EXPECT_TRUE(result.fileWritten);
    EXPECT_EQ(readFile(destination), "serialized bytes");
#else
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "TEMPORARY_FILE_CHANGED");
    EXPECT_EQ(readFile(destination), "old bytes");
#endif
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)), "foreign bytes");
    std::error_code cleanupError;
    fs::remove(movedTemporary, cleanupError);
    fs::remove(Base::FileInfo::stringToPath(temporary), cleanupError);
}

#ifndef FC_OS_WIN32
TEST_F(DocumentFileWriterTest, ReplacePrimitiveRejectsLateTemporaryLeafSwap)
{
    const auto destination = path("replace-leaf-swap.FCStd");
    const auto movedOwned = path("replace-leaf-swap-owned.tmp");
    writeFile(destination, "canonical bytes");

    std::string temporary;
    std::error_code hookError;
    App::Internal::DocumentFileReplacementResult result;
    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.mode = App::Internal::DocumentFileReplacementMode::Replace;
        request.beforeReplacementPrimitive = [&] {
            fs::rename(Base::FileInfo::stringToPath(temporary), movedOwned, hookError);
            if (!hookError) {
                writeFile(Base::FileInfo::stringToPath(temporary), "foreign temporary bytes");
            }
        };
        App::Internal::DocumentFileWriter writer(std::move(request));
        temporary = writer.temporaryPath();
        serialize(writer, "owned serialized bytes");
        result = writer.commit();
    }

    ASSERT_FALSE(hookError) << hookError.message();
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.replacementCompleted);
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "ATOMIC_REPLACEMENT_FAILED");
    EXPECT_EQ(readFile(destination), "canonical bytes");
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(temporary)), "foreign temporary bytes");
    EXPECT_EQ(readFile(movedOwned), "owned serialized bytes");
    std::error_code cleanupError;
    fs::remove(Base::FileInfo::stringToPath(temporary), cleanupError);
    fs::remove(movedOwned, cleanupError);
}
#endif

TEST_F(DocumentFileWriterTest, SameSizeSameMtimeTemporaryMutationIsRejectedOrPrevented)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "old bytes");

    std::string temporary;
#ifndef FC_OS_WIN32
    std::optional<fs::file_time_type> serializedModified;
#endif
    std::error_code callbackError;
#ifdef FC_OS_WIN32
    bool mutationOpened = false;
#endif
    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.beforeFinalBoundaryValidation = [&] {
#ifdef FC_OS_WIN32
        std::ofstream stream(Base::FileInfo::stringToPath(temporary),
                             std::ios::out | std::ios::binary | std::ios::trunc);
        mutationOpened = stream.is_open();
        if (mutationOpened) {
            stream << "bbbbbbbb";
        }
#else
        writeFile(Base::FileInfo::stringToPath(temporary), "bbbbbbbb");
        fs::last_write_time(Base::FileInfo::stringToPath(temporary),
                            *serializedModified,
                            callbackError);
#endif
    };
    App::Internal::DocumentFileWriter writer(std::move(request));
    temporary = writer.temporaryPath();
    serialize(writer, "aaaaaaaa");
#ifndef FC_OS_WIN32
    serializedModified =
        fs::last_write_time(Base::FileInfo::stringToPath(temporary), callbackError);
#endif
    ASSERT_FALSE(callbackError) << callbackError.message();

    const auto result = writer.commit();

    ASSERT_FALSE(callbackError) << callbackError.message();
#ifdef FC_OS_WIN32
    EXPECT_FALSE(mutationOpened);
    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    EXPECT_EQ(readFile(destination), "aaaaaaaa");
#else
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "TEMPORARY_FILE_CHANGED");
    EXPECT_EQ(readFile(destination), "old bytes");
#endif
}

TEST_F(DocumentFileWriterTest, PostValidationSameSizeSameMtimeMutationIsRehashedOrDenied)
{
    const auto destination = path("post-hash-document.FCStd");
    writeFile(destination, "old bytes");

    std::string temporary;
#ifndef FC_OS_WIN32
    std::optional<fs::file_time_type> serializedModified;
#endif
    std::error_code callbackError;
#ifdef FC_OS_WIN32
    bool mutationOpened = false;
#endif
    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.afterFinalBoundaryValidationBeforeReplace = [&] {
#ifdef FC_OS_WIN32
        std::ofstream stream(Base::FileInfo::stringToPath(temporary),
                             std::ios::out | std::ios::binary | std::ios::trunc);
        mutationOpened = stream.is_open();
        if (mutationOpened) {
            stream << "bbbbbbbb";
        }
#else
        writeFile(Base::FileInfo::stringToPath(temporary), "bbbbbbbb");
        fs::last_write_time(Base::FileInfo::stringToPath(temporary),
                            *serializedModified,
                            callbackError);
#endif
    };
    App::Internal::DocumentFileWriter writer(std::move(request));
    temporary = writer.temporaryPath();
    serialize(writer, "aaaaaaaa");
#ifndef FC_OS_WIN32
    serializedModified =
        fs::last_write_time(Base::FileInfo::stringToPath(temporary), callbackError);
#endif
    ASSERT_FALSE(callbackError) << callbackError.message();

    const auto result = writer.commit();

    ASSERT_FALSE(callbackError) << callbackError.message();
#ifdef FC_OS_WIN32
    EXPECT_FALSE(mutationOpened);
    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    EXPECT_EQ(readFile(destination), "aaaaaaaa");
#else
    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "TEMPORARY_FILE_CHANGED");
    EXPECT_EQ(readFile(destination), "old bytes");
#endif
}

TEST_F(DocumentFileWriterTest, DiscardsSnapshotWhenSourceMutatesWithSameSizeAndMtime)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "aaaaaaaa");
    std::error_code callbackError;
    const auto originalModified = fs::last_write_time(destination, callbackError);
    ASSERT_FALSE(callbackError) << callbackError.message();

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.preserveDisplacedFile = true;
    request.afterDisplacedSourceHashBeforeCopy = [&] {
        writeFile(destination, "bbbbbbbb");
        fs::last_write_time(destination, originalModified, callbackError);
    };
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "new bytes");

    const auto result = writer.commit();

    ASSERT_FALSE(callbackError) << callbackError.message();
    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    EXPECT_TRUE(result.fileWritten);
    EXPECT_TRUE(result.displacedFile.empty());
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_EQ(readFile(destination), "new bytes");
}

TEST_F(DocumentFileWriterTest, SnapshotReservationCollisionNeverAdoptsForeignPath)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "old bytes");
    std::string collision;
    bool injected = false;

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.preserveDisplacedFile = true;
    request.beforeDisplacedReservationAttempt = [&](const std::string& candidate) {
        if (!injected) {
            injected = true;
            collision = candidate;
            writeFile(Base::FileInfo::stringToPath(candidate), "foreign collision");
        }
    };
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "new bytes");

    const auto result = writer.commit();

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    ASSERT_TRUE(injected);
    ASSERT_FALSE(result.displacedFile.empty());
    EXPECT_NE(result.displacedFile, collision);
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(collision)), "foreign collision");
    EXPECT_EQ(readActiveLeasedArtifact(Base::FileInfo::stringToPath(result.displacedFile)),
              "old bytes");
}

TEST_F(DocumentFileWriterTest, PostReplacementVerificationFailureRetainsDisplacedBytes)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "old bytes");

    App::Internal::DocumentFileReplacementResult result;
    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.preserveDisplacedFile = true;
        request.testFault =
            App::Internal::DocumentFileWriterTestFault::AfterReplacementBeforeVerification;
        App::Internal::DocumentFileWriter writer(std::move(request));
        serialize(writer, "new bytes");

        result = writer.commit();
    }

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(result.replacementCompleted);
    EXPECT_FALSE(result.replacementVerified);
    EXPECT_FALSE(result.durabilityVerified);
    EXPECT_TRUE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "TEST_INJECTED_REPLACEMENT_VERIFICATION_FAILURE");
    EXPECT_EQ(readFile(destination), "new bytes");
    ASSERT_FALSE(result.displacedFile.empty());
    EXPECT_EQ(readActiveLeasedArtifact(Base::FileInfo::stringToPath(result.displacedFile)),
              "old bytes");
}

TEST_F(DocumentFileWriterTest, PostReplacementDurabilityFailureRetainsDisplacedBytes)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "old bytes");

    App::Internal::DocumentFileReplacementResult result;
    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.preserveDisplacedFile = true;
        request.testFault = App::Internal::DocumentFileWriterTestFault::BeforeDurabilityFlush;
        App::Internal::DocumentFileWriter writer(std::move(request));
        serialize(writer, "new bytes");

        result = writer.commit();
    }

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(result.replacementCompleted);
    EXPECT_TRUE(result.replacementVerified);
    EXPECT_FALSE(result.durabilityVerified);
    EXPECT_TRUE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "TEST_INJECTED_DURABILITY_FAILURE");
    EXPECT_EQ(readFile(destination), "new bytes");
    ASSERT_FALSE(result.displacedFile.empty());
    EXPECT_EQ(readActiveLeasedArtifact(Base::FileInfo::stringToPath(result.displacedFile)),
              "old bytes");
}

TEST_F(DocumentFileWriterTest, FailedBackupCanLeaveRecoverySnapshotAfterLeaseDestruction)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "old bytes");
    std::string recoveryPath;

    {
        App::Internal::DocumentFileReplacementRequest request;
        request.destination = pathToUtf8(destination);
        request.preserveDisplacedFile = true;
        App::Internal::DocumentFileWriter writer(std::move(request));
        serialize(writer, "new bytes");
        auto result = writer.commit();
        ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
        recoveryPath = result.displacedFile;

        App::Internal::setBackupPolicyBeforeInstallHookForTesting(
            [](const std::string&) { throw std::runtime_error("injected install failure"); });
        App::BackupPolicy policy;
        policy.setPolicy(App::BackupPolicy::Standard);
        policy.setNumberOfFiles(1);
        const auto backup = policy.applyAfterReplacement(
            recoveryPath, pathToUtf8(destination), result.displacedFileLease);
        EXPECT_FALSE(backup.backupCreated);
        EXPECT_FALSE(backup.displacedFileConsumed);
        EXPECT_FALSE(backup.warnings.empty());
        ASSERT_TRUE(result.retainDisplacedFileForRecovery());
    }

    EXPECT_TRUE(fs::exists(Base::FileInfo::stringToPath(recoveryPath)));
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(recoveryPath)), "old bytes");
}

TEST_F(DocumentFileWriterTest, RetainedLeaseInstallsBackupWithoutReopeningSource)
{
    const auto destination = path("lease-install.FCStd");
    auto replacement = makeDisplacedLease(destination);
    ASSERT_TRUE(replacement.succeeded())
        << replacement.errorCode << ": " << replacement.message;

    App::BackupPolicy policy;
    policy.setPolicy(App::BackupPolicy::Standard);
    policy.setNumberOfFiles(1);
    const auto backup = policy.applyAfterReplacement(replacement.displacedFile,
                                                     pathToUtf8(destination),
                                                     replacement.displacedFileLease);

    EXPECT_TRUE(backup.backupCreated);
    EXPECT_TRUE(backup.displacedFileConsumed);
    EXPECT_TRUE(backup.warnings.empty());
    EXPECT_FALSE(fs::exists(Base::FileInfo::stringToPath(replacement.displacedFile)));
    EXPECT_EQ(readFile(path("lease-install.FCStd1")), "old bytes");
    EXPECT_EQ(readFile(destination), "new bytes");
}

TEST_F(DocumentFileWriterTest, RetainedLeaseNoReplaceRetriesLateOsCollision)
{
    const auto destination = path("lease-collision.FCStd");
    auto replacement = makeDisplacedLease(destination);
    ASSERT_TRUE(replacement.succeeded())
        << replacement.errorCode << ": " << replacement.message;
    std::string collision;
    bool injected = false;
    App::Internal::setBackupPolicyBeforeInstallHookForTesting(
        [&](const std::string& candidate) {
            if (!injected) {
                injected = true;
                collision = candidate;
                writeFile(Base::FileInfo::stringToPath(candidate), "foreign collision");
            }
        });

    App::BackupPolicy policy;
    policy.setPolicy(App::BackupPolicy::Standard);
    policy.setNumberOfFiles(2);
    const auto backup = policy.applyAfterReplacement(replacement.displacedFile,
                                                     pathToUtf8(destination),
                                                     replacement.displacedFileLease);

    ASSERT_TRUE(injected);
    EXPECT_TRUE(backup.backupCreated);
    EXPECT_TRUE(backup.displacedFileConsumed);
    EXPECT_TRUE(backup.warnings.empty());
    EXPECT_EQ(readFile(Base::FileInfo::stringToPath(collision)), "foreign collision");
    EXPECT_EQ(readFile(path("lease-collision.FCStd2")), "old bytes");
}

TEST_F(DocumentFileWriterTest, RetainedLeaseDiscardRemovesOnlyOwnedSnapshot)
{
    const auto destination = path("lease-discard.FCStd");
    auto replacement = makeDisplacedLease(destination);
    ASSERT_TRUE(replacement.succeeded())
        << replacement.errorCode << ": " << replacement.message;
    const auto displaced = Base::FileInfo::stringToPath(replacement.displacedFile);

    App::BackupPolicy policy;
    policy.setPolicy(App::BackupPolicy::Standard);
    policy.setNumberOfFiles(0);
    const auto backup = policy.applyAfterReplacement(replacement.displacedFile,
                                                     pathToUtf8(destination),
                                                     replacement.displacedFileLease);

    EXPECT_FALSE(backup.backupCreated);
    EXPECT_TRUE(backup.displacedFileConsumed);
#ifdef FC_OS_WIN32
    EXPECT_FALSE(backup.warnings.empty());
#else
    EXPECT_TRUE(backup.warnings.empty());
#endif
    EXPECT_FALSE(fs::exists(displaced));
    EXPECT_EQ(readFile(destination), "new bytes");
}

#ifdef FC_OS_WIN32
TEST_F(DocumentFileWriterTest, WindowsExactDiscardNeverClaimsDirectoryDurability)
{
    const auto destination = path("windows-discard-flags.FCStd");
    auto replacement = makeDisplacedLease(destination);
    ASSERT_TRUE(replacement.succeeded())
        << replacement.errorCode << ": " << replacement.message;
    const auto displaced = Base::FileInfo::stringToPath(replacement.displacedFile);

    const auto discarded = App::Internal::discardDisplacedFileLease(
        replacement.displacedFileLease, replacement.displacedFile);

    EXPECT_FALSE(discarded.installed);
    EXPECT_TRUE(discarded.sourceConsumed);
    EXPECT_FALSE(discarded.durabilityVerified);
    EXPECT_FALSE(discarded.error.empty());
    EXPECT_NE(discarded.error.find("durability"), std::string::npos);
    EXPECT_FALSE(fs::exists(displaced));
    EXPECT_EQ(readFile(destination), "new bytes");
}

TEST_F(DocumentFileWriterTest, WindowsLegacyDeletePendingReportsConsumedButNotDurable)
{
    const auto destination = path("windows-delete-pending-flags.FCStd");
    writeFile(destination, "old bytes");
    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.preserveDisplacedFile = true;
    request.testFault =
        App::Internal::DocumentFileWriterTestFault::LegacyDisplacedDiscardDeletePending;
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "new bytes");
    auto replacement = writer.commit();
    ASSERT_TRUE(replacement.succeeded())
        << replacement.errorCode << ": " << replacement.message;
    const auto displaced = Base::FileInfo::stringToPath(replacement.displacedFile);
    const HANDLE sharedHandle = CreateFileW(displaced.c_str(),
                                            GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE
                                                | FILE_SHARE_DELETE,
                                            nullptr,
                                            OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL,
                                            nullptr);
    ASSERT_NE(sharedHandle, INVALID_HANDLE_VALUE) << GetLastError();

    const auto discarded = App::Internal::discardDisplacedFileLease(
        replacement.displacedFileLease, replacement.displacedFile);

    EXPECT_FALSE(discarded.installed);
    EXPECT_TRUE(discarded.sourceConsumed);
    EXPECT_FALSE(discarded.durabilityVerified);
    EXPECT_FALSE(discarded.error.empty());
    EXPECT_NE(discarded.error.find("durability"), std::string::npos);
    char retainedBytes[9] {};
    DWORD retainedByteCount = 0;
    ASSERT_NE(ReadFile(sharedHandle,
                       retainedBytes,
                       static_cast<DWORD>(sizeof(retainedBytes)),
                       &retainedByteCount,
                       nullptr),
              0);
    EXPECT_EQ(std::string(retainedBytes, retainedByteCount), "old bytes");
    ASSERT_NE(CloseHandle(sharedHandle), 0);
    const DWORD attributesAfterClose = GetFileAttributesW(displaced.c_str());
    const DWORD errorAfterClose = GetLastError();
    EXPECT_EQ(attributesAfterClose, INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE(errorAfterClose == ERROR_FILE_NOT_FOUND
                || errorAfterClose == ERROR_PATH_NOT_FOUND);
}

TEST_F(DocumentFileWriterTest, WindowsLegacyDeletePendingIsMaintenanceWarningWithoutPruning)
{
    const auto destination = path("windows-delete-pending.FCStd");
    writeFile(destination, "old bytes");
    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.preserveDisplacedFile = true;
    request.testFault =
        App::Internal::DocumentFileWriterTestFault::LegacyDisplacedDiscardDeletePending;
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "new bytes");
    auto replacement = writer.commit();
    ASSERT_TRUE(replacement.succeeded())
        << replacement.errorCode << ": " << replacement.message;
    const auto displaced = Base::FileInfo::stringToPath(replacement.displacedFile);
    const auto knownHistory = path("windows-delete-pending.FCStd1");
    writeFile(knownHistory, "known history bytes");

    const HANDLE sharedHandle = CreateFileW(displaced.c_str(),
                                            GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE
                                                | FILE_SHARE_DELETE,
                                            nullptr,
                                            OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL,
                                            nullptr);
    ASSERT_NE(sharedHandle, INVALID_HANDLE_VALUE) << GetLastError();

    App::BackupPolicy policy;
    policy.setPolicy(App::BackupPolicy::Standard);
    policy.setNumberOfFiles(0);
    const auto backup = policy.applyAfterReplacement(replacement.displacedFile,
                                                     pathToUtf8(destination),
                                                     replacement.displacedFileLease);

    EXPECT_FALSE(backup.backupCreated);
    EXPECT_TRUE(backup.displacedFileConsumed);
    ASSERT_FALSE(backup.warnings.empty());
    EXPECT_NE(backup.warnings.front().find("cleanup durability is unverified"),
              std::string::npos);
    EXPECT_EQ(readFile(knownHistory), "known history bytes");
    EXPECT_EQ(readFile(destination), "new bytes");

    ASSERT_NE(CloseHandle(sharedHandle), 0);
    const DWORD attributesAfterClose = GetFileAttributesW(displaced.c_str());
    const DWORD errorAfterClose = GetLastError();
    EXPECT_EQ(attributesAfterClose, INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE(errorAfterClose == ERROR_FILE_NOT_FOUND
                || errorAfterClose == ERROR_PATH_NOT_FOUND);
}
#endif

TEST_F(DocumentFileWriterTest, DiscardAfterSourceSwapNeverDeletesForeignEntry)
{
    const auto destination = path("lease-discard-swap.FCStd");
    auto replacement = makeDisplacedLease(destination);
    ASSERT_TRUE(replacement.succeeded())
        << replacement.errorCode << ": " << replacement.message;
    const auto displaced = Base::FileInfo::stringToPath(replacement.displacedFile);
    const auto movedOwned = path("discard-moved-owned-displaced");
    std::error_code error;
    fs::rename(displaced, movedOwned, error);
    ASSERT_FALSE(error) << error.message();
    writeFile(displaced, "foreign displaced bytes");

    App::BackupPolicy policy;
    policy.setPolicy(App::BackupPolicy::Standard);
    policy.setNumberOfFiles(0);
    const auto backup = policy.applyAfterReplacement(replacement.displacedFile,
                                                     pathToUtf8(destination),
                                                     replacement.displacedFileLease);

#ifdef FC_OS_WIN32
    EXPECT_TRUE(backup.displacedFileConsumed);
    EXPECT_FALSE(fs::exists(movedOwned));
#else
    EXPECT_FALSE(backup.displacedFileConsumed);
    EXPECT_TRUE(fs::exists(movedOwned));
#endif
    EXPECT_EQ(readFile(displaced), "foreign displaced bytes");
    EXPECT_EQ(readFile(destination), "new bytes");
    std::error_code cleanupError;
    fs::remove(movedOwned, cleanupError);
    fs::remove(displaced, cleanupError);
}

TEST_F(DocumentFileWriterTest, SourceNameSwapCannotRedirectRetainedLeaseInstall)
{
    const auto destination = path("lease-source-swap.FCStd");
    auto replacement = makeDisplacedLease(destination);
    ASSERT_TRUE(replacement.succeeded())
        << replacement.errorCode << ": " << replacement.message;
    const auto displaced = Base::FileInfo::stringToPath(replacement.displacedFile);
    const auto movedOwned = path("moved-owned-displaced");
    std::error_code hookError;
    App::Internal::setBackupPolicyBeforeInstallHookForTesting(
        [&](const std::string&) {
            fs::rename(displaced, movedOwned, hookError);
            if (!hookError) {
                writeFile(displaced, "foreign displaced bytes");
            }
        });

    App::BackupPolicy policy;
    policy.setPolicy(App::BackupPolicy::Standard);
    policy.setNumberOfFiles(1);
    const auto backup = policy.applyAfterReplacement(replacement.displacedFile,
                                                     pathToUtf8(destination),
                                                     replacement.displacedFileLease);

    ASSERT_FALSE(hookError) << hookError.message();
#ifdef FC_OS_WIN32
    EXPECT_TRUE(backup.backupCreated);
    EXPECT_TRUE(backup.displacedFileConsumed);
    EXPECT_EQ(readFile(path("lease-source-swap.FCStd1")), "old bytes");
    EXPECT_FALSE(fs::exists(movedOwned));
#else
    EXPECT_FALSE(backup.backupCreated);
    EXPECT_FALSE(backup.displacedFileConsumed);
    EXPECT_TRUE(fs::exists(movedOwned));
#endif
    EXPECT_EQ(readFile(displaced), "foreign displaced bytes");
    EXPECT_EQ(readFile(destination), "new bytes");
    std::error_code cleanupError;
    fs::remove(movedOwned, cleanupError);
    fs::remove(displaced, cleanupError);
}

#ifndef FC_OS_WIN32
TEST_F(DocumentFileWriterTest, ParentSwapStopsRetainedLeaseBeforeNamespaceInstall)
{
    const auto parent = path("owned-parent");
    const auto movedParent = path("moved-owned-parent");
    ASSERT_TRUE(fs::create_directory(parent));
    const auto destination = parent / "document.FCStd";
    auto replacement = makeDisplacedLease(destination);
    ASSERT_TRUE(replacement.succeeded())
        << replacement.errorCode << ": " << replacement.message;
    const auto displacedLeaf =
        Base::FileInfo::stringToPath(replacement.displacedFile).filename();
    std::error_code hookError;
    App::Internal::setBackupPolicyBeforeInstallHookForTesting(
        [&](const std::string&) {
            fs::rename(parent, movedParent, hookError);
            if (!hookError) {
                fs::create_directory(parent, hookError);
                if (!hookError) {
                    writeFile(destination, "foreign canonical bytes");
                }
            }
        });

    App::BackupPolicy policy;
    policy.setPolicy(App::BackupPolicy::Standard);
    policy.setNumberOfFiles(1);
    const auto backup = policy.applyAfterReplacement(replacement.displacedFile,
                                                     pathToUtf8(destination),
                                                     replacement.displacedFileLease);

    ASSERT_FALSE(hookError) << hookError.message();
    EXPECT_FALSE(backup.backupCreated);
    EXPECT_FALSE(backup.displacedFileConsumed);
    EXPECT_FALSE(backup.warnings.empty());
    EXPECT_EQ(readFile(destination), "foreign canonical bytes");
    EXPECT_EQ(readFile(movedParent / "document.FCStd"), "new bytes");
    EXPECT_TRUE(fs::exists(movedParent / displacedLeaf));
}

TEST_F(DocumentFileWriterTest, PortableLinkFallbackInstallsBackupAndRetainsSourceName)
{
    const auto destination = path("lease-portable-link.FCStd");
    auto replacement = makeDisplacedLease(destination);
    ASSERT_TRUE(replacement.succeeded())
        << replacement.errorCode << ": " << replacement.message;
    const auto displaced = Base::FileInfo::stringToPath(replacement.displacedFile);
    // Installing any checkpoint hook forces the portable link fallback taken on
    // a filesystem without a strict no-replace rename primitive.
    App::Internal::setBackupPolicyCheckpointHookForTesting(
        [](const App::Internal::BackupPolicyTestCheckpoint,
           const std::string&,
           const std::string&) {});

    App::BackupPolicy policy;
    policy.setPolicy(App::BackupPolicy::Standard);
    policy.setNumberOfFiles(1);
    const auto backup = policy.applyAfterReplacement(replacement.displacedFile,
                                                     pathToUtf8(destination),
                                                     replacement.displacedFileLease);

    const auto warns = [&](const std::string& text) {
        for (const auto& warning : backup.warnings) {
            if (warning.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(backup.backupCreated);
    // POSIX cannot unlink conditionally on the retained identity. The fallback
    // must therefore leave the displaced name rather than delete a pathname it
    // can only prove inside a window a hostile writer can reopen.
    EXPECT_FALSE(backup.displacedFileConsumed);
    EXPECT_TRUE(warns("the displaced snapshot name remains"))
        << (backup.warnings.empty() ? std::string("no warning was reported")
                                    : backup.warnings.front());
    EXPECT_TRUE(fs::exists(displaced));
    EXPECT_EQ(readFile(displaced), "old bytes");
    EXPECT_EQ(readFile(path("lease-portable-link.FCStd1")), "old bytes");
    EXPECT_EQ(readFile(destination), "new bytes");
    std::error_code cleanupError;
    fs::remove(displaced, cleanupError);
}

TEST_F(DocumentFileWriterTest, PortableLinkFallbackCandidateSwapIsNotReportedOrPrunedAsDurable)
{
    const auto destination = path("lease-post-link.FCStd");
    auto replacement = makeDisplacedLease(destination);
    ASSERT_TRUE(replacement.succeeded())
        << replacement.errorCode << ": " << replacement.message;
    const auto displaced = Base::FileInfo::stringToPath(replacement.displacedFile);
    const auto oldBackup = path("lease-post-link.FCStd1");
    writeFile(oldBackup, "known good history");
    const auto movedOwned = path("post-link-owned-backup");
    fs::path swappedCandidate;
    std::error_code hookError;
    App::Internal::setBackupPolicyCheckpointHookForTesting(
        [&](const App::Internal::BackupPolicyTestCheckpoint checkpoint,
            const std::string&,
            const std::string& candidate) {
            if (checkpoint
                == App::Internal::BackupPolicyTestCheckpoint::AfterLinkBeforeDirectoryFlush) {
                swappedCandidate = Base::FileInfo::stringToPath(candidate);
                fs::rename(swappedCandidate, movedOwned, hookError);
                if (!hookError) {
                    writeFile(swappedCandidate, "foreign candidate bytes");
                }
            }
        });

    App::BackupPolicy policy;
    policy.setPolicy(App::BackupPolicy::Standard);
    policy.setNumberOfFiles(1);
    const auto backup = policy.applyAfterReplacement(replacement.displacedFile,
                                                     pathToUtf8(destination),
                                                     replacement.displacedFileLease);

    ASSERT_FALSE(hookError) << hookError.message();
    ASSERT_FALSE(swappedCandidate.empty());
    EXPECT_FALSE(backup.backupCreated);
    EXPECT_FALSE(backup.displacedFileConsumed);
    EXPECT_FALSE(backup.warnings.empty());
    EXPECT_EQ(readFile(oldBackup), "known good history");
    EXPECT_EQ(readFile(swappedCandidate), "foreign candidate bytes");
    EXPECT_EQ(readFile(movedOwned), "old bytes");
    EXPECT_EQ(readFile(displaced), "old bytes");
    EXPECT_EQ(readFile(destination), "new bytes");
    std::error_code cleanupError;
    fs::remove(movedOwned, cleanupError);
    fs::remove(displaced, cleanupError);
}
#endif

#ifndef FC_OS_WIN32
TEST_F(DocumentFileWriterTest, ExistingModeIsPreservedForReplacementAndDisplacedSnapshot)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "old bytes");
    ASSERT_EQ(::chmod(destination.c_str(), 0640), 0);

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.preserveDisplacedFile = true;
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "new bytes");
    const auto result = writer.commit();

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    struct stat destinationInfo {};
    ASSERT_EQ(::stat(destination.c_str(), &destinationInfo), 0);
    EXPECT_EQ(destinationInfo.st_mode & 07777, 0640);
    struct stat displacedInfo {};
    ASSERT_EQ(::stat(Base::FileInfo::stringToPath(result.displacedFile).c_str(), &displacedInfo),
              0);
    EXPECT_EQ(displacedInfo.st_mode & 07777, 0640);
}

TEST_F(DocumentFileWriterTest, NewFileModeUsesKernelAppliedUmask)
{
    const auto destination = path("new-document.FCStd");
    const auto probe = path("mode-probe");
    int flags = O_CREAT | O_EXCL | O_WRONLY;
# ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
# endif
    const int descriptor = ::open(probe.c_str(), flags, 0666);
    ASSERT_GE(descriptor, 0);
    ASSERT_EQ(::close(descriptor), 0);
    struct stat probeInfo {};
    ASSERT_EQ(::stat(probe.c_str(), &probeInfo), 0);

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "new bytes");
    const auto result = writer.commit();

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    struct stat destinationInfo {};
    ASSERT_EQ(::stat(destination.c_str(), &destinationInfo), 0);
    EXPECT_EQ(destinationInfo.st_mode & 0777, probeInfo.st_mode & 0777);
}
#endif

#ifdef FC_OS_WIN32
TEST_F(DocumentFileWriterTest, InstalledFileDoesNotRetainTemporaryAttribute)
{
    const auto destination = path("document.FCStd");
    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "serialized bytes");

    const auto result = writer.commit();

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    const DWORD attributes = GetFileAttributesW(destination.c_str());
    ASSERT_NE(attributes, INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(attributes & FILE_ATTRIBUTE_TEMPORARY, 0U);
    EXPECT_TRUE(result.durabilityVerified);
}

TEST_F(DocumentFileWriterTest, ExistingBasicAttributesAndProtectedDaclSurviveReplacement)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "old bytes");
    const DWORD initialAttributes = GetFileAttributesW(destination.c_str());
    ASSERT_NE(initialAttributes, INVALID_FILE_ATTRIBUTES);
    constexpr DWORD requestedAttributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN
        | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY
        | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    if (SetFileAttributesW(destination.c_str(), initialAttributes | requestedAttributes) == 0) {
        GTEST_SKIP() << "Basic attributes are unavailable: " << GetLastError();
    }
    const DWORD expectedAttributes = GetFileAttributesW(destination.c_str());
    ASSERT_NE(expectedAttributes, INVALID_FILE_ATTRIBUTES);
    const DWORD securityStatus = protectWindowsDacl(destination);
    if (securityStatus != ERROR_SUCCESS) {
        GTEST_SKIP() << "DACL protection is unavailable: " << securityStatus;
    }
    const auto expectedDacl = readWindowsDacl(destination);
    ASSERT_TRUE(expectedDacl.protectedDacl);
    WIN32_FILE_ATTRIBUTE_DATA expectedBasic {};
    ASSERT_NE(GetFileAttributesExW(destination.c_str(), GetFileExInfoStandard, &expectedBasic), 0);

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "new bytes");
    const auto result = writer.commit();

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    const DWORD actualAttributes = GetFileAttributesW(destination.c_str());
    ASSERT_NE(actualAttributes, INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(actualAttributes & requestedAttributes,
              expectedAttributes & requestedAttributes);
    WIN32_FILE_ATTRIBUTE_DATA actualBasic {};
    ASSERT_NE(GetFileAttributesExW(destination.c_str(), GetFileExInfoStandard, &actualBasic), 0);
    EXPECT_EQ(CompareFileTime(&actualBasic.ftCreationTime, &expectedBasic.ftCreationTime), 0);
    const auto actualDacl = readWindowsDacl(destination);
    EXPECT_TRUE(actualDacl.protectedDacl);
    EXPECT_EQ(actualDacl.bytes, expectedDacl.bytes);
}

TEST_F(DocumentFileWriterTest, NewFileRetainsSameParentInheritedDacl)
{
    const auto probe = path("inheritance-probe");
    const auto destination = path("new-document.FCStd");
    writeFile(probe, "probe");
    const auto expectedDacl = readWindowsDacl(probe);

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "new bytes");
    const auto result = writer.commit();

    ASSERT_TRUE(result.succeeded()) << result.errorCode << ": " << result.message;
    const auto actualDacl = readWindowsDacl(destination);
    EXPECT_EQ(actualDacl.protectedDacl, expectedDacl.protectedDacl);
    EXPECT_EQ(actualDacl.bytes, expectedDacl.bytes);
}

TEST_F(DocumentFileWriterTest, SparseDestinationFailsClosedBeforeReplacement)
{
    const auto destination = path("sparse-document.FCStd");
    writeFile(destination, "old bytes");
    FILE_SET_SPARSE_BUFFER sparse {TRUE};
    if (!applyWindowsControl(destination, FSCTL_SET_SPARSE, &sparse, sizeof(sparse))) {
        GTEST_SKIP() << "Sparse files are unavailable: " << GetLastError();
    }
    const DWORD attributes = GetFileAttributesW(destination.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_SPARSE_FILE) == 0) {
        GTEST_SKIP() << "The filesystem did not expose sparse-file state";
    }

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "new bytes");
    const auto result = writer.commit();

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "METADATA_UNSUPPORTED");
    EXPECT_EQ(readFile(destination), "old bytes");
}

TEST_F(DocumentFileWriterTest, CompressedDestinationFailsClosedBeforeReplacement)
{
    const auto destination = path("compressed-document.FCStd");
    writeFile(destination, "old bytes");
    USHORT compression = COMPRESSION_FORMAT_DEFAULT;
    if (!applyWindowsControl(
            destination, FSCTL_SET_COMPRESSION, &compression, sizeof(compression))) {
        GTEST_SKIP() << "Compression is unavailable: " << GetLastError();
    }
    const DWORD attributes = GetFileAttributesW(destination.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_COMPRESSED) == 0) {
        GTEST_SKIP() << "The filesystem did not expose compression state";
    }

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "new bytes");
    const auto result = writer.commit();

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "METADATA_UNSUPPORTED");
    EXPECT_EQ(readFile(destination), "old bytes");
}

#if defined(FSCTL_SET_INTEGRITY_INFORMATION) && defined(FILE_ATTRIBUTE_INTEGRITY_STREAM) \
    && defined(CHECKSUM_TYPE_CRC64)
TEST_F(DocumentFileWriterTest, IntegrityDestinationFailsClosedBeforeReplacement)
{
    const auto destination = path("integrity-document.FCStd");
    writeFile(destination, "old bytes");
    FSCTL_SET_INTEGRITY_INFORMATION_BUFFER integrity {};
    integrity.ChecksumAlgorithm = CHECKSUM_TYPE_CRC64;
    integrity.Flags = 0;
    if (!applyWindowsControl(destination,
                             FSCTL_SET_INTEGRITY_INFORMATION,
                             &integrity,
                             sizeof(integrity))) {
        GTEST_SKIP() << "Integrity streams are unavailable: " << GetLastError();
    }
    const DWORD attributes = GetFileAttributesW(destination.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & FILE_ATTRIBUTE_INTEGRITY_STREAM) == 0) {
        GTEST_SKIP() << "The filesystem did not expose integrity-stream state";
    }

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "new bytes");
    const auto result = writer.commit();

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "METADATA_UNSUPPORTED");
    EXPECT_EQ(readFile(destination), "old bytes");
}
#endif

TEST_F(DocumentFileWriterTest, AlternateDataStreamFailsClosedBeforeReplacement)
{
    const auto destination = path("document.FCStd");
    writeFile(destination, "old bytes");
    const auto streamPath = fs::path(destination.native() + L":review-metadata");
    std::ofstream alternate(streamPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!alternate.is_open()) {
        GTEST_SKIP() << "Alternate data streams are unavailable";
    }
    alternate << "metadata";
    alternate.close();

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "new bytes");
    const auto result = writer.commit();

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "METADATA_UNSUPPORTED");
    EXPECT_EQ(readFile(destination), "old bytes");
}
#endif

TEST_F(DocumentFileWriterTest, RejectsHardLinkAliasCreatedAfterSerialization)
{
    const auto canonical = path("canonical.FCStd");
    const auto destination = path("copy.FCStd");
    writeFile(canonical, "canonical bytes");

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.forbiddenAliasPath = pathToUtf8(canonical);
    request.rejectDestinationLinks = true;
    std::error_code error;
    request.beforeFinalBoundaryValidation = [&] {
        fs::create_hard_link(canonical, destination, error);
    };
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "copy bytes");

    const auto result = writer.commit();
    if (error) {
        GTEST_SKIP() << "Hard links are unavailable: " << error.message();
    }

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "DESTINATION_ALIASES_FORBIDDEN_FILE");
    EXPECT_EQ(readFile(canonical), "canonical bytes");
    EXPECT_EQ(readFile(destination), "canonical bytes");
}

TEST_F(DocumentFileWriterTest, RejectsSymbolicLinkAliasCreatedAfterSerialization)
{
    const auto canonical = path("canonical.FCStd");
    const auto destination = path("copy.FCStd");
    writeFile(canonical, "canonical bytes");

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.forbiddenAliasPath = pathToUtf8(canonical);
    request.rejectDestinationLinks = true;
    std::error_code error;
    request.beforeFinalBoundaryValidation = [&] {
        fs::create_symlink(canonical, destination, error);
    };
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "copy bytes");

    const auto result = writer.commit();
    if (error) {
        GTEST_SKIP() << "Symbolic links are unavailable: " << error.message();
    }

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "DESTINATION_ALIASES_FORBIDDEN_FILE");
    EXPECT_EQ(readFile(canonical), "canonical bytes");
    EXPECT_EQ(readFile(destination), "canonical bytes");
}

TEST_F(DocumentFileWriterTest, RejectsDestinationLinkRegardlessOfCompatibilityFlag)
{
    const auto linkTarget = path("ordinary-link-target.FCStd");
    const auto destination = path("linked-destination.FCStd");
    writeFile(linkTarget, "target bytes");

    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.rejectDestinationLinks = false;
    std::error_code error;
    request.beforeFinalBoundaryValidation = [&] {
        fs::create_symlink(linkTarget, destination, error);
    };
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "replacement bytes");

    const auto result = writer.commit();
    if (error) {
        GTEST_SKIP() << "Symbolic links are unavailable: " << error.message();
    }

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "DESTINATION_LINK_FORBIDDEN");
    EXPECT_EQ(readFile(linkTarget), "target bytes");
}

TEST_F(DocumentFileWriterTest, RejectsMissingCanonicalReachedThroughSymlinkedParent)
{
    const auto realParent = path("real-parent");
    const auto realDirectory = realParent / "nested";
    const auto aliasDirectory = path("alias");
    ASSERT_TRUE(fs::create_directories(realDirectory));

    std::error_code error;
    fs::create_directory_symlink(realDirectory, aliasDirectory, error);
    if (error) {
        GTEST_SKIP() << "Directory symbolic links are unavailable: " << error.message();
    }

    const auto canonical = realDirectory / "missing-canonical.FCStd";
    const auto destination = aliasDirectory / ".." / "nested" / "missing-canonical.FCStd";
    App::Internal::DocumentFileReplacementRequest request;
    request.destination = pathToUtf8(destination);
    request.forbiddenAliasPath = pathToUtf8(canonical);
    request.rejectDestinationLinks = true;
    App::Internal::DocumentFileWriter writer(std::move(request));
    serialize(writer, "copy bytes");

    const auto result = writer.commit();

    EXPECT_FALSE(result.succeeded());
    EXPECT_FALSE(result.fileWritten);
    EXPECT_EQ(result.errorCode, "DESTINATION_ALIASES_FORBIDDEN_FILE");
    EXPECT_FALSE(fs::exists(canonical));
}

}  // namespace
