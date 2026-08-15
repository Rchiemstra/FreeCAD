# Save-Artifact Contract

**Status:** **Approved** — both open questions ruled on; implemented and verified green.
**Applies to:** `src/App/DocumentFileWriter.{h,cpp}`, `src/App/BackupPolicy.{h,cpp}`, and the
`App::Document` save entry points that drive them.

**Rulings:** Q1 — retain and report verified serialized bytes (§2.2). Q2 — permit an
identity-proved removal of a `DisplacedCanonical` when the user configured
`numberOfFiles == 0` (§2.3).

This document is normative. "MUST", "MUST NOT", and "MAY" carry their usual force. Where the
current implementation deviates, the deviation is called out explicitly as a defect rather
than silently blessed.

No writer test expectation may be reclassified as stale on the grounds that "the behaviour
changed deliberately". This contract is what decides which side of each disagreement is wrong,
and every reconciliation must cite the rule it follows.

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

**Q1 — RULED: retain and report (Q1-a).**

> **R4a.** Promotion MUST clear the `EphemeralPartial` mark. `DocumentFileWriter::commit()`
> calls `markVerifiedSerialization()` immediately after the serialized baseline hash is taken,
> so from that point no cleanup path can remove the file.

> **R4b.** A save that fails after promotion MUST report the retained path, and the warning
> MUST describe it as the verified document being saved and state that it is safe to delete
> once no longer needed — not as an unremovable temporary.

The bytes are cheap to keep and the user may have no other copy if the application then
crashes. A failure *before* promotion still leaves nothing behind, because the artifact is
still an `EphemeralPartial` and cleanup removes it.

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

**Q2 — RULED: permit it, with an identity proof (Q2-a).**

> **R5a.** The removal MUST use its own entry point, `discardDisplacedCanonicalExact()`, which
> is never reachable from `discardExact()` or from destructor cleanup. Deleting the previous
> version is precisely what `numberOfFiles == 0` means; refusing to honour it is not a safety
> win, it is unbounded growth of stale copies of the user's document.

> **R5b.** The POSIX path MUST prove the entry still names the exact retained inode before
> unlinking, and MUST retain and report it as unconsumed when that proof fails. This is a
> genuine improvement on the historical unconditional `unlink`, whose failure mode was silent
> deletion of a foreign entry.

This is an unlink of user data and therefore a deliberate, narrow carve-out from R1, justified
by explicit user configuration rather than by the §1.2 blast-radius argument. Both affected
tests now pass unchanged: `RetainedLeaseDiscardRemovesOnlyOwnedSnapshot` because the proof
succeeds, and `DiscardAfterSourceSwapNeverDeletesForeignEntry` because a swapped source fails
it.

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
| DisplacedCanonical | never | only via `discardDisplacedCanonicalExact()`, `numberOfFiles == 0` | handle rename / disposition | retain + report unconsumed |
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
| Retention of a `VerifiedSerialization` after a failed save | retained and reported (R4a/R4b) | still removed by the handle delete disposition |

The last row is a **known and deliberate gap**, not a settled decision. R4a is expressed
through the `_ephemeralPartial` mark, which only gates the POSIX path; the Windows destructor
removes the file through its retained handle regardless. Windows users therefore do not get
the recovery evidence Q1 was ruled to provide. Making Windows honour R4a is a follow-up,
tracked separately because it changes existing Windows test expectations.

---

## 9. Ruling record and reconciliation

Both questions were ruled on, implemented, and verified. `DocumentFileWriterTest.*` and
`BackupPolicyTest.*` are **87 passed, 2 platform skips, 0 failed**.

| # | ruling | implementation |
| --- | --- | --- |
| Q1 | retain and report | `markVerifiedSerialization()` at the serialized-baseline hash; failure warning rewritten per R4b |
| Q2 | permit, with identity proof | `discardDisplacedCanonicalExact()`, used only by the lease discard |

How each of the original eleven disagreements was resolved:

| test | resolution |
| --- | --- |
| `AbandonedSerializationNeverTouchesDestination` | production fix — EphemeralPartial cleanup restored; passes unchanged |
| `RetainedLeaseDiscardRemovesOnlyOwnedSnapshot` | production fix — Q2/R5a; passes unchanged |
| `BackupPolicyTest.TimestampTargetHasNoExtension` | not a writer defect — pre-existing latent `Base::FileInfo::extension()` bug, fires only when the temp directory path contains a dot; filed separately |
| `NoReplaceRejectsDestinationCreatedAfterSerialization` | expectation corrected under R4a/R4b |
| `CompareAndSwapRejectsSwapAtReplacementPrimitive` | expectation corrected under R4a/R4b |
| `CompareAndSwapGuardMoveNeverClobbersCollision` | expectation corrected under R4a/R4b |
| `CompareAndSwapUnsupportedNoReplaceFailsBeforeMutation` | expectation corrected under R4a/R4b |
| `CompareAndSwapAuthorityFailureIsPreMutationAndSpecific` | expectation corrected under R4a/R4b |
| `CompareAndSwapDurabilityFailureIsPreMutationAndSpecific` | expectation corrected under R4a/R4b |
| `CompareAndSwapAcceptsMatchingDestinationHash` | expectation corrected under §2.3/R17 — a successful CAS hands its predecessor to BackupPolicy |
| `CompareAndSwapPostInstallGuardInspectionIsBestEffort` | stale warning string only; no behaviour change |

Only two of the eleven were production defects. Seven were expectations that the approved
contract shows to be wrong, one was an unrelated pre-existing bug, and one was a wording drift.

## 10. Known follow-ups

1. Windows does not honour R4a (see §8).
2. `Base::FileInfo::extension()` returns everything after the last dot in the whole path, so an
   extensionless target inside a dotted directory makes `applyTimeStamp` write its backup
   outside the document directory. Pre-existing; not caused by this work.
3. An `O_TMPFILE` lane on tmpfs would let unnamed serialization be adopted and exercised; it is
   unavailable on overlayfs and 9p, which is why the earlier attempt was reverted.
