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

#ifndef APP_DOCUMENTFILEWRITER_H
#define APP_DOCUMENTFILEWRITER_H

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
# include <functional>
#endif

#include <FCGlobal.h>

namespace App::Internal
{

// DocumentFileWriter is a private implementation detail. Test-enabled builds
// export it solely so App_tests_run can exercise the Windows DLL boundary;
// production builds intentionally add no export/import annotation.
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
# define APP_DOCUMENTFILEWRITER_EXPORT AppExport
#else
# define APP_DOCUMENTFILEWRITER_EXPORT
#endif

/** The filesystem precondition enforced at the final replacement boundary. */
enum class DocumentFileReplacementMode
{
    Replace,
    NoReplace,
    CompareAndSwapSha256,
};

/**
 * The outcome of a retained-lease namespace operation.
 *
 * An installation can succeed and be durable while `sourceConsumed` is false:
 * the POSIX portable link fallback installs the destination but cannot remove
 * the source name, because POSIX has no inode-conditional unlink and any
 * pathname proof can be invalidated before the unlink runs. In that case
 * `error` carries the non-fatal reason the name was retained, and the caller
 * must report it as recoverable maintenance evidence rather than a failure.
 */
struct DisplacedFileLeaseOperationResult
{
    bool installed {false};
    bool sourceConsumed {false};
    bool durabilityVerified {false};
    bool destinationExists {false};
    std::string error;
};

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
enum class DisplacedFileLeaseCheckpoint
{
    BeforeNamespaceInstall,
    AfterLinkBeforeDirectoryFlush,
};
using DisplacedFileLeaseCheckpointHook =
    std::function<void(DisplacedFileLeaseCheckpoint)>;
#endif

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
enum class DocumentFileWriterTestFault
{
    None,
    SerializationWrite,
    SerializationSeek,
    SerializationSync,
    AfterReplacementBeforeVerification,
    BeforeDurabilityFlush,
    CompareAndSwapAuthorityUnavailable,
    CompareAndSwapDurabilityUnavailable,
    StrictNoReplaceUnavailable,
    CompareAndSwapGuardValidation,
    CompareAndSwapPostInstallGuardInspection,
    CleanupReadOnlyAndRestrictedDacl,
    LegacyDisplacedDiscardDeletePending,
};
#endif

struct DocumentFileReplacementRequest
{
    std::string destination;
    DocumentFileReplacementMode mode {DocumentFileReplacementMode::Replace};
    std::string expectedDestinationSha256;
    // When non-empty, the destination must not be this file or an alias of it.
    std::string forbiddenAliasPath;
    // Retained for source compatibility. Replacement destinations are always
    // rejected when the final entry is a symbolic link or reparse point.
    bool rejectDestinationLinks {false};
    bool createParentDirectories {true};
    bool preserveDisplacedFile {false};
    int lockTimeoutMs {0};
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
    // Deterministic developer-test seams. This field and all corresponding
    // branches are absent when developer tests are disabled.
    std::function<void()> beforeFinalBoundaryValidation;
    std::function<void()> afterFinalBoundaryValidationBeforeReplace;
    // For Replace/NoReplace, invoked after every userspace proof and just
    // before the OS namespace primitive. Compare-and-swap revalidates both
    // identity and content after this seam before moving its recovery guard.
    std::function<void()> beforeReplacementPrimitive;
    // Compare-and-swap only: invoked after final destination validation and
    // immediately before the strict move to the named absent guard.
    std::function<void(const std::string&)> beforeCompareAndSwapGuardMove;
    // Compare-and-swap only: the expected destination has been moved to its
    // owned recovery guard, but the serialized temporary has not been
    // installed. The guard path is diagnostic; the retained handle remains
    // authoritative.
    std::function<void(const std::string&)> afterCompareAndSwapGuardMove;
    // Compare-and-swap only: the recovery guard has passed its identity/hash
    // proof and the strict no-replace install is the next namespace action.
    std::function<void(const std::string&)>
        afterCompareAndSwapGuardValidationBeforeInstall;
    // POSIX compare-and-swap only: invoked after the strict primitive has
    // proved that its source leaf names the retained handle, but before the
    // pathname-based no-replace rename. The post-rename identity proof must
    // reject any source-leaf substitution performed here.
    std::function<void(const std::string&, const std::string&)>
        afterStrictNoReplaceSourceValidationBeforeRename;
    // Invoked before a failed compare-and-swap attempts to restore its exact
    // recovery guard into an empty canonical destination.
    std::function<void(const std::string&)> beforeCompareAndSwapGuardRestore;
    // Invoked after that strict restore primitive reports success, but before
    // the canonical entry is inspected. The retained predecessor handle is
    // still authoritative if this hook swaps or removes the restored name.
    std::function<void(const std::string&)>
        afterCompareAndSwapGuardRestoreBeforeInspection;
    // Invoked after the serialized CAS replacement is installed and its
    // irreversible result flags are recorded, but before the obsolete guard
    // is inspected for cleanup. A changed guard must be preserved and the
    // exact predecessor materialized from its retained handle.
    std::function<void(const std::string&)>
        afterCompareAndSwapInstallBeforeGuardInspection;
    // Model a filesystem that cannot resolve the installed destination name
    // while this process still holds a descriptor on the installed inode, as
    // the 9p bind mount does. Forces the release-then-re-resolve path.
    bool simulatePathInvisibleUntilDescriptorRelease {false};
    // Invoked after the installed inode's descriptor has been released and
    // before the destination name is re-resolved. Substituting the destination
    // here must be detected and must leave the substituted entry untouched.
    std::function<void(const std::string&)> afterInstalledDescriptorRelease;
    std::function<void()> afterDisplacedSourceHashBeforeCopy;
    std::function<void(const std::string&)> beforeDisplacedReservationAttempt;
    DocumentFileWriterTestFault testFault {DocumentFileWriterTestFault::None};
#endif
};

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
using DocumentFileWriterRequestDecorator =
    std::function<void(DocumentFileReplacementRequest&)>;
using DocumentFileWriterRequestDecoratorGuard = std::shared_ptr<void>;

/**
 * Install a developer-test-only request decorator.
 *
 * Keep the returned guard alive for the intended scope. Destroying or
 * resetting it unregisters this decorator without disturbing a newer or
 * outer registration. The active decorator is copied under a mutex and
 * invoked once, before DocumentFileWriter creates its Impl or any file.
 */
[[nodiscard]] APP_DOCUMENTFILEWRITER_EXPORT DocumentFileWriterRequestDecoratorGuard
setDocumentFileWriterRequestDecoratorForTesting(
    DocumentFileWriterRequestDecorator decorator);
#endif

struct APP_DOCUMENTFILEWRITER_EXPORT DocumentFileReplacementResult
{
    bool replacementCompleted {false};
    bool replacementVerified {false};
    bool durabilityVerified {false};
    // True once the serialized replacement was installed at the destination,
    // even if a later verification or durability check failed. Moving only an
    // expected CAS predecessor to a recovery guard does not set this flag.
    bool fileWritten {false};
    std::string destination;
    // An exact pre-replacement snapshot for BackupPolicy or an exact moved
    // compare-and-swap guard retained as recovery evidence. A failed CAS may
    // therefore transfer this lease before replacementCompleted becomes true.
    std::string displacedFile;
    // Keeps the exclusively opened displaced snapshot pinned until backup
    // management has consumed it or the result is released.
    std::shared_ptr<void> displacedFileLease;
    std::string errorCode;
    std::string message;
    std::vector<std::string> warnings;

    /**
     * Relinquish automatic cleanup while leaving an owned displaced snapshot
     * at displacedFile as recovery evidence. This is the required handoff
     * when post-replacement backup installation reports
     * displacedFileConsumed=false.
     */
    [[nodiscard]] bool retainDisplacedFileForRecovery() noexcept;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return replacementCompleted && replacementVerified && durabilityVerified
            && errorCode.empty();
    }
};

/**
 * Consume an opaque displaced-file lease without reopening its source path.
 * These are private App-library primitives used by BackupPolicy; the source
 * string is a diagnostic identity assertion, never the transfer authority.
 */
[[nodiscard]] DisplacedFileLeaseOperationResult installDisplacedFileLeaseNoReplace(
    const std::shared_ptr<void>& lease,
    const std::string& expectedSource,
    const std::string& destination
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
    ,
    const DisplacedFileLeaseCheckpointHook& checkpointHook = {},
    bool forcePortableLinkFallback = false
#endif
);
[[nodiscard]] APP_DOCUMENTFILEWRITER_EXPORT DisplacedFileLeaseOperationResult
discardDisplacedFileLease(
    const std::shared_ptr<void>& lease,
    const std::string& expectedSource
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
    ,
    const std::function<void(const std::string&)>& beforePosixFailClosedDecision = {}
#endif
);

/** Process-local plus cross-process lock shared by replacement and backup history. */
class APP_DOCUMENTFILEWRITER_EXPORT DocumentFileLock
{
public:
    explicit DocumentFileLock(const std::string& destination, int timeoutMs);
    ~DocumentFileLock();

    DocumentFileLock(const DocumentFileLock&) = delete;
    DocumentFileLock(DocumentFileLock&&) = delete;
    DocumentFileLock& operator=(const DocumentFileLock&) = delete;
    DocumentFileLock& operator=(DocumentFileLock&&) = delete;

    [[nodiscard]] bool isLocked() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

/**
 * Own one same-directory temporary file and atomically install it.
 *
 * The constructor reserves a sibling file with exclusive-create semantics.
 * The caller serializes through serializationStream(), whose retained native
 * handle is authoritative from the first byte through replacement, then calls
 * commit(). Uncommitted temporary files and unclaimed displaced-file snapshots
 * are removed by the destructor, but only while their filesystem identities
 * still match. temporaryPath() is diagnostic/test-only and must not be opened
 * for production serialization. Windows can remove an owned path through its
 * retained handle. POSIX exposes no portable inode-conditional unlink, so
 * pathname cleanup fails closed: successful rename consumes the ordinary
 * temporary, while abandoned serialization and failed commits leave their
 * still-owned sibling name intact instead of risking deletion of a hostile
 * replacement. A failed commit reports that recovery path when it can still
 * prove the name; callers that abandon a writer can retain temporaryPath()
 * for their own recovery/maintenance policy.
 *
 * Content-hash compare-and-swap is serialized against other FreeCAD writers by
 * a path lock. It uses genuine no-replace namespace primitives to move the
 * validated destination to an owned recovery guard, verifies that exact guard,
 * and then installs the serialized file without clobbering a late entrant.
 * This conflict-safe, recoverable sequence deliberately includes a brief
 * absent canonical entry; it is not a single content-conditioned OS primitive.
 * Unsupported filesystems fail before the canonical namespace is changed.
 */
class APP_DOCUMENTFILEWRITER_EXPORT DocumentFileWriter
{
public:
    explicit DocumentFileWriter(DocumentFileReplacementRequest request);
    ~DocumentFileWriter();

    DocumentFileWriter(const DocumentFileWriter&) = delete;
    DocumentFileWriter(DocumentFileWriter&&) noexcept;
    DocumentFileWriter& operator=(const DocumentFileWriter&) = delete;
    DocumentFileWriter& operator=(DocumentFileWriter&&) noexcept;

    [[nodiscard]] const std::string& destinationPath() const noexcept;
    [[nodiscard]] std::ostream& serializationStream() noexcept;
    [[nodiscard]] const std::string& temporaryPath() const noexcept;
    [[nodiscard]] DocumentFileReplacementResult commit();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace App::Internal

#undef APP_DOCUMENTFILEWRITER_EXPORT

#endif  // APP_DOCUMENTFILEWRITER_H
