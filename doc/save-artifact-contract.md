# Save-Artifact Contract

**Status:** Proposed — **awaiting approval**
**Applies to:** `src/App/DocumentFileWriter.{h,cpp}`, `src/App/BackupPolicy.{h,cpp}`, and the
`App::Document` save entry points that drive them.

This document is normative. "MUST", "MUST NOT", and "MAY" carry their usual force. Where the
current implementation deviates, the deviation is called out explicitly as a defect rather
than silently blessed.

Until this contract is approved, no writer test expectation may be reclassified as stale on
the grounds that "the behaviour changed deliberately". The contract is what decides which
side of each disagreement is wrong.

---

## 1. POSIX threat model

### 1.1 The general rule

POSIX offers no portable way to remove a directory entry conditionally on the inode it names,
and no portable unlink-by-descriptor. Every

```
fstatat(dirfd, name)   →   prove identity
unlinkat(dirfd, name)  →   remove
```

sequence therefore contains a substitution window. A writer that can create entries in the
directory can replace `name` between the two calls and redirect the removal onto a file we
never inspected.

**A userspace identity check does not close this window.** Adding a second or third proof
narrows it but never eliminates it. Any design whose safety argument rests on "we checked
just before unlinking" is rejected.

Consequently:

> **R1.** Code MUST NOT unlink a pathname whose removal could destroy a version of the user's
> document, no matter how recently its identity was proved.

The same reasoning applies to `renameat` and `linkat` when the *source* is named by pathname.
Where a strict no-replace primitive exists (`renameat2(RENAME_NOREPLACE)`,
`renameatx_np(RENAME_EXCL)`, Windows `FILE_RENAME_INFO` with `ReplaceIfExists=FALSE`) it MUST
be preferred, and the post-primitive identity of the installed entry MUST be proved before
the operation is reported as installed.

### 1.2 The one narrow exception

R1 is scoped to *entries whose removal could destroy user data*. Exactly one artifact class
falls outside that scope: the `EphemeralPartial` serialization temporary (§2.1). For it, and
only for it, an identity-proved unlink is permitted, under this explicitly stated model:

- **What the attacker must have.** Write access to the destination directory, and the
  unpredictable UUID name this process just generated.
- **What they already have.** Anyone with write access to that directory can rename, replace,
  or delete the user's document outright. The temporary is not the weak link.
- **What is lost if the race is lost.** One attacker-planted file, in a directory the attacker
  already controls. Never a canonical document, a displaced predecessor, a backup, or recovery
  evidence — none of those are ever routed through this path.
- **What is not claimed.** This is a practical desktop threat model, not a hardened
  multi-tenant one. It is not adequate for a world-writable or shared spool directory, and it
  is deliberately narrower than the guarantees the rest of this writer makes.

> **R2.** The identity-proved unlink MAY be used only for an artifact explicitly marked
> `EphemeralPartial`. On a failed proof it MUST retain the entry and report it. It MUST NOT be
> generalised to any other class.

### 1.3 Windows

Windows has exact-handle primitives and is not subject to R1. Removal uses the delete
disposition on the retained handle; replacement uses `FILE_RENAME_INFO` /
`FILE_RENAME_INFO_EX` against the retained handle and a pinned parent. Windows MAY therefore
clean up classes that POSIX must retain, and the contract records where the two platforms
legitimately differ (§8).

---

## 2. Artifact classes

Every file this subsystem creates belongs to exactly one class at any moment. Class is a
property of the *role*, not the inode: an inode can be promoted from `EphemeralPartial` to
`VerifiedSerialization` (§2.2) and from `DisplacedCanonical` to `BackupHistory` (§2.4).

### 2.1 EphemeralPartial

| | |
| --- | --- |
| Name | `<destination-filename>.<uuid>.tmp`, sibling of the destination |
| Created | `O_CREAT\|O_EXCL\|O_RDWR\|O_CLOEXEC`, mode `0666` masked by umask (POSIX); `CREATE_NEW` (Windows) |
| Holds | An incomplete, or complete but not yet verified, serialization |
| Published | Never. The name is not shown to the user as a result path |
| Cleanup | **MAY be removed** by identity-proved unlink (§1.2) |

An `EphemeralPartial` holds no version of the user's document: if the save fails here, the
canonical file is still untouched and still authoritative.

> **R3.** The serialization temporary MUST be the only artifact marked `EphemeralPartial`, and
> the mark MUST be applied at construction by the writer that owns it.

### 2.2 VerifiedSerialization

| | |
| --- | --- |
| Name | Same inode and name as the `EphemeralPartial` it was promoted from |
| Promoted when | Its SHA-256 has been computed and stably re-verified against an unchanged observed file |
| Holds | The exact bytes intended to become canonical |
| Cleanup | **MUST NOT** be removed by generic cleanup |

Once promoted, the file is either consumed by the install primitive or is meaningful evidence:
it is the only copy of work the user asked to save.

> **R4.** After promotion the artifact MUST either be consumed by the install rename, or be
> retained and reported by path in `result.warnings`. It MUST NOT be silently discarded.

**Open question for approval (Q1).** Promotion is currently implicit — the same
`_ephemeralPartial` mark persists across it, so a post-verification abandon would take the
§1.2 unlink path and delete verified bytes. Two ways to resolve, and the contract needs a
ruling:

- **Q1-a:** clear the mark at verification, so a verified serialization is always retained.
  Safer; means a failure after verification leaves a named file.
- **Q1-b:** keep the mark, on the grounds that the canonical file is still intact and the
  user's editor still holds the document in memory, so the bytes are reproducible.

The recommendation is **Q1-a**: the bytes are cheap to keep and the user may have no other
copy if the application then crashes.

### 2.3 DisplacedCanonical

| | |
| --- | --- |
| Name | `<destination-filename>.<uuid>.displaced`, sibling of the destination |
| Created | By moving the exact previous canonical file aside through its retained handle |
| Holds | **A real version of the user's document** — the previous save |
| Cleanup | **MUST NOT** be removed by generic cleanup |

> **R5.** A `DisplacedCanonical` MUST NOT be removed by destructor cleanup, discard-on-error,
> or any other generic path. It MAY be removed **only** by an explicit, user-configured
> retention decision (§2.4, `numberOfFiles == 0`), and only after an exact identity proof; on a
> failed proof it MUST be retained and reported as unconsumed.

**Open question for approval (Q2).** R5's exception is not currently implemented: the POSIX
lease discard fails closed unconditionally. This is the direct cause of one of the eleven
writer failures (`RetainedLeaseDiscardRemovesOnlyOwnedSnapshot`), and the two behaviours are
mutually exclusive:

- **Q2-a:** implement the R5 exception — identity-proved removal when the user has explicitly
  configured zero backups. `RetainedLeaseDiscardRemovesOnlyOwnedSnapshot` then passes as
  written, and `DiscardAfterSourceSwapNeverDeletesForeignEntry` still passes because a swapped
  source fails the proof.
- **Q2-b:** keep failing closed. The displaced predecessor accumulates in the user's document
  directory on every save even though they asked for no backups, and the test expectation must
  change instead.

The recommendation is **Q2-a**. Deleting the previous version is precisely what
`numberOfFiles == 0` means; refusing to honour it is not a safety win, it is unbounded growth
of stale copies of the user's document. The identity proof is a genuine safety improvement
over the historical unconditional `unlink`, and its failure mode is retention.

Note this is an unlink of user data and so is a deliberate, narrow carve-out from R1, justified
by explicit user configuration rather than by the §1.2 blast-radius argument.

### 2.4 BackupHistory

| | |
| --- | --- |
| Name | `<base>N` (standard) or `<base>.<timestamp>[-N].FCBak` (timestamp) |
| Created | By installing a `DisplacedCanonical` under a history name, no-replace |
| Holds | A retained previous version of the user's document |
| Cleanup | Pruned **only** by retention policy, and only after a durable, accounted install |

> **R6.** History MUST NOT be pruned unless the new entry was installed **and** its source-name
> transition was durably accounted (`installationDurable`). A collision candidate that existed
> before the attempt MUST be treated as protected and never pruned.

> **R7.** Installation MUST use a strict no-replace primitive. Where only a portable `linkat`
> fallback is available, the source name MUST be retained and reported unconsumed (R1), and
> history MUST NOT be pruned on that path.

### 2.5 NamedRecoveryEvidence

| | |
| --- | --- |
| Names | `.cas-recovery`, `.cas-predecessor-recovery`, `.cas-restored-predecessor-recovery`, `.cas-post-install-predecessor-recovery`, `.cas-serialized-recovery` |
| Created | By moving or hash-verified copying from a retained handle when a compare-and-swap path cannot complete safely |
| Holds | The exact predecessor, or the exact serialized replacement |
| Cleanup | **Never** removed automatically |

> **R8.** Every `NamedRecoveryEvidence` artifact MUST be reported by absolute path in
> `result.warnings`, MUST survive destruction of the result and its lease, and MUST NOT be
> removed by any automatic path.

> **R9.** Evidence MUST be hash-verified against the expected digest at materialisation. If it
> cannot be verified, the failure MUST be reported and the retained handle MUST remain
> authoritative; a partial or unverified copy MUST NOT be reported as evidence.

---

## 3. Cleanup permission matrix

| class | POSIX generic cleanup | POSIX explicit discard | Windows | on failed identity proof |
| --- | --- | --- | --- | --- |
| EphemeralPartial | identity-proved unlink | identity-proved unlink | delete disposition | retain + report |
| VerifiedSerialization | never | never | delete disposition only before publication | retain + report |
| DisplacedCanonical | never | only when `numberOfFiles == 0` (Q2) | handle rename / disposition | retain + report unconsumed |
| BackupHistory | never | retention prune only | retention prune only | retain + report |
| NamedRecoveryEvidence | never | never | never | retain + report |

> **R10.** No code path may remove an entry whose class it has not established. "Owned by this
> writer" is not sufficient authority to unlink.

---

## 4. File modes and metadata

> **R11.** A newly created artifact is opened with mode `0666` and left to the process umask;
> the writer MUST NOT widen it.

> **R12.** When replacing an existing destination, the replacement MUST carry the existing
> file's permissions, taken from the destination's **retained descriptor** (`fstat`/`fchmod`),
> never from a re-resolved pathname. A mode that cannot be verified after being set MUST fail
> the save before replacement.

> **R13.** On Windows, basic attributes and the DACL MUST be preserved. Advanced filesystem
> state that cannot be reproduced on the replacement — sparse, compressed, encrypted,
> integrity-stream, alternate data streams, reparse points — MUST fail closed **before**
> replacement rather than being silently dropped.

> **R14.** Snapshot copies (`DisplacedCanonical`, `NamedRecoveryEvidence`) MUST additionally
> preserve the source modification time, so a retained predecessor remains recognisable.

---

## 5. Structured warnings and recovery reporting

> **R15.** Any artifact left on disk that the user may need MUST be reported, by absolute
> path, in `result.warnings`. Silent retention is a defect.

> **R16.** Warning text MUST state what the file is, why it was kept, and whether it is safe to
> delete. "Could not be removed" alone is insufficient.

> **R17.** The single `displacedFile` / `displacedFileLease` result slot MUST name the artifact
> most valuable for recovery. When a CAS path materialises fresh evidence, that evidence
> supersedes the earlier guard path in the slot, and the superseded warning MUST be replaced,
> not appended alongside — a result MUST NOT report two different paths for "the exact previous
> destination".

> **R18.** `fileWritten` and `replacementCompleted`, once true, are irrevocable. A failure after
> durable installation MAY add warnings but MUST NOT reclassify installed bytes as unwritten,
> nor roll back an adopted Save As identity.

---

## 6. The legacy no-lease BackupPolicy route

`BackupPolicy::apply(source, target)` and the two-argument `applyAfterReplacement` predate the
retained-handle lease. With no lease they fall through to `atomicInstallNoReplace`, which
operates entirely on caller-supplied pathnames and performs its own `::unlink` of the source.

> **R19.** The no-lease route is **compatibility-only**. It MUST NOT be used by new code, and it
> MUST NOT be presented as providing the retained-handle guarantees in §1–§4. It is retained
> solely for callers that predate the lease.

> **R20.** The no-lease route MUST remain behaviourally unchanged for its existing callers. It
> is explicitly outside the scope of R1: its source is a pathname the caller chose, not an
> artifact this subsystem created and can identify.

> **R21.** Any test that exercises the no-lease route is testing the compatibility path, and its
> result MUST NOT be used as evidence about the lease-based design, in either direction.

---

## 7. Invariants that hold across all classes

> **R22.** The retained descriptor, never a pathname, is the authority for content, identity,
> hashing, durability, and metadata.

> **R23.** Every namespace primitive MUST be followed by an identity proof of the entry it
> created before the operation is reported as successful.

> **R24.** No canonical, displaced, backup, or verified-recovery pathname is unlinked on POSIX
> outside the single explicitly approved carve-out in §2.3.

> **R25.** A destructor MUST NOT perform a namespace mutation that could remove an artifact the
> result has already reported.

---

## 8. Deliberate platform differences

These are legitimate, and tests MUST assert them per-platform rather than treating either as
the bug:

| behaviour | POSIX | Windows |
| --- | --- | --- |
| Discard of a swapped displaced snapshot | retained, reported unconsumed | consumed via retained handle |
| Abandoned `EphemeralPartial` | identity-proved unlink | delete disposition |
| Directory durability after install | `fsync` on the pinned parent | handle flush; no portable directory contract |

---

## 9. Items requiring a ruling before implementation

1. **Q1** (§2.2) — does the `EphemeralPartial` mark survive promotion to
   `VerifiedSerialization`? Recommendation: **Q1-a**, clear it.
2. **Q2** (§2.3) — is the `numberOfFiles == 0` identity-proved removal of a
   `DisplacedCanonical` in scope for this release? Recommendation: **Q2-a**, implement it.

Both questions change which of the eleven outstanding writer failures are production defects
and which are stale expectations, so neither reconciliation can be finalised until they are
answered.
