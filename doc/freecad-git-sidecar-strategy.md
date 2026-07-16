# Git-Friendly Versioning for FreeCAD Documents

Status: research and architecture proposal
Target branch: `FreeCAD-start`
Scope: normal `.FCStd` files plus deterministic `.FCStd.git.json` sidecars

## 1. Executive conclusion

Keep each normal `.FCStd` document alongside a deterministic `.FCStd.git.json` sidecar.

```text
AutoCurtains.FCStd
AutoCurtains.FCStd.git.json
```

The `.FCStd` remains the authoritative and complete FreeCAD document. The JSON is a generated review artifact containing stable document structure, parameters, expressions, constraints, links, and other selected semantic information. It is not a reconstruction format and must not be edited or merged as a substitute for resolving the FreeCAD document.

Use defensive direct ZIP/XML parsing for the committed sidecar. Make headless FreeCAD inspection an optional, sandboxed diagnostic facility for trusted files rather than part of mandatory pull-request verification.

For normal interactive use, provide an opt-in **Generate Git sidecar after save** adapter. FreeCAD first completes its normal `.FCStd` save. Only after the finalized archive is in place does the adapter invoke the standalone exporter. A sidecar failure warns the user but cannot invalidate the successful model save.

This provides readable Git and GitHub history without discarding FreeCAD information or requiring every reviewer to install a custom diff driver.

## 2. Verified FCStd format findings

An `.FCStd` document is a ZIP container. FreeCAD writes its archive from [`Document::saveToFile`](../src/App/Document.cpp), using the ZIP writer in [`Base/Writer.cpp`](../src/Base/Writer.cpp). The [FreeCAD Developers Handbook](https://freecad.github.io/DevelopersHandbook/gettingstarted/dependencies.html) also describes FCStd as XML plus BREP data in a ZIP archive.

A representative `AutoCurtains.FCStd` contained:

- 1,003 ZIP entries and 376 document objects.
- `Document.xml`, approximately 1.89 MB.
- `GuiDocument.xml`, approximately 1.27 MB.
- 238 `.brp` shape entries.
- 166 `.Map.txt` topology-map entries.
- GUI appearance arrays, a thumbnail, and supporting tables.
- Approximately 1.13 MB compressed and 8.49 MB uncompressed content.

`Document.xml` contains the application-level model:

- Document properties and metadata.
- Object internal names, labels, and type IDs.
- Serialized properties, expressions, and placements.
- Group, Part, Body, Origin, Tip, and dependency relationships.
- Links and external references.
- Spreadsheet cells and aliases.
- Sketch geometry and constraints.
- PartDesign and assembly parameters.

`GuiDocument.xml` contains presentation state:

- View-provider properties and visibility.
- Colors and display modes.
- Tree expansion and camera state.
- Other GUI-only settings.

Geometry is normally stored as textual `.brp` OpenCASCADE BREP data, with supporting `.Map.txt` element maps. FreeCAD can instead emit binary `.bin` BREP content. GUI appearance arrays and thumbnails are binary.

Sources of non-semantic archive noise include:

- ZIP entry timestamps and archive byte layout.
- `LastModifiedDate` during an ordinary save.
- GUI state and thumbnails.
- Numeric object IDs after object recreation.
- External-link modification stamps.
- Touched, error, frozen, and other transient state.
- BREP or topology-map serialization differences.
- Object insertion order and workbench-specific properties.

The bundled ZIP writer sets each entry timestamp to the current local time when closing it in [`zipoutputstreambuf.cpp`](../src/3rdParty/zipios++/zipoutputstreambuf.cpp). Consequently, a no-op save can change the archive hash even when every entry payload is unchanged.

Document UIDs are normally persistent rather than changing on every save, but they are not useful for semantic review and should be omitted from the default sidecar.

## 3. Comparison table of Git strategies

| Strategy | Diff and GitHub review | Storage | Fidelity and reliability | Merge safety | Complexity and determinism |
|---|---|---|---|---|---|
| Commit only `.FCStd` | Binary-only; no meaningful PR diff | Simple, but ZIP noise weakens deltas | Complete document retained | Unsafe binary merge | Lowest maintenance; not reproducible by default |
| Git LFS for `.FCStd` | Usually only an LFS pointer is visible | Moves large data outside normal Git | Complete document retained | Still a binary merge; locking can help | Extra service, bandwidth, and checkout requirements |
| Git `textconv` or external diff | Useful locally; not a native stored GitHub artifact | Binary remains unchanged | Depends on converter coverage | Display-only; cannot merge converted text | Per-developer configuration and tooling |
| Unpacked FCStd directory | XML is visible but noisy; many files | Good Git packing, much larger worktree | Can preserve content if repacking is exact | Text merges may create inconsistent documents | Complex filters/hooks and platform-sensitive filenames |
| `.FCStd` plus JSON | Focused native GitHub diffs | Some worktree duplication; JSON packs well | Complete FCStd retained; sidecar is selective | Binary remains manually resolved | Moderate implementation; pure Python can be deterministic |
| Deterministic ZIP writer | Still a binary PR artifact | May improve binary delta efficiency | Potentially complete | Still unsafe to merge | High FreeCAD-core cost; does not stabilize all payloads |
| Script-based source model | Excellent for source-first designs | Usually efficient | Arbitrary FCStd conversion can be lossy | Normal source merging may work | Requires a pinned execution environment and may execute code |
| Existing FreeCAD tools | Useful local or visual comparison | Varies | Useful complements | Generally not semantic merge systems | Installation and FreeCAD-version requirements vary |

Git documents `textconv` as a one-way diff conversion, not a merge or patch representation. See [gitattributes](https://git-scm.com/docs/gitattributes). GitHub cannot be expected to run each developer's locally configured converter.

Git LFS stores a pointer in Git and the large object separately. It is useful for genuinely large authoritative binaries, but it does not create semantic review. See [Git LFS](https://git-lfs.com/) and [GitHub's LFS documentation](https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-git-large-file-storage).

Existing references include:

- Repository tool [`src/Tools/fcinfo`](../src/Tools/fcinfo), which supports local text conversion but is not a complete sidecar schema.
- [GitCAD](https://github.com/MikeOpsGit/GitCAD), which uses unpacking, filters, hooks, and LFS.
- [diff-freecad](https://github.com/SebKuzminsky/diff-freecad), a local visual/external diff tool.
- [HistoryWorkbench](https://github.com/eblanshey/HistoryWorkbench), an interactive FreeCAD comparison workbench.

## 4. Recommendation

Adopt the paired `.FCStd` and `.FCStd.git.json` convention for every tracked model.

Rules of authority:

1. The `.FCStd` is always authoritative.
2. The JSON is generated and never edited manually.
3. The JSON must not be used to reconstruct `.FCStd`.
4. Merging JSON does not resolve a binary model conflict.
5. Resolve concurrent model changes in FreeCAD, then regenerate the sidecar.
6. CI verifies consistency but never regenerates and commits files.
7. Automatic generation is opt-in and runs only after a successful eligible save.
8. Sidecar failure is non-fatal to the completed `.FCStd` save.

Call the user-facing feature **Generate Git sidecar after save** or **Git sidecar on successful save**. Avoid names such as *Composite Save*, *Atomic Document Pair*, or *Dual-file transaction*: replacing two adjacent files cannot be one portable atomic filesystem operation.

Do not enable LFS solely for the current approximately 1.1 MB example. LFS remains an independent repository policy for substantially larger documents.

## 5. Proposed JSON schema

A compact conceptual representation is:

```json
{
  "schema": "freecad-git-sidecar/v1",
  "generator": {
    "name": "freecad-git",
    "version": "0.1.0",
    "profile": "semantic"
  },
  "source": {
    "filename": "AutoCurtains.FCStd",
    "semantic_sha256": "5f27e1..."
  },
  "document": {
    "name": "AutoCurtains",
    "label": "AutoCurtains",
    "freecad_version": "1.2R46769"
  },
  "objects": {
    "Parameters": {
      "type": "Spreadsheet::Sheet",
      "label": "Parameters",
      "spreadsheet": {
        "B1": {
          "alias": "printerOffset",
          "content": "=0.2 mm"
        }
      }
    },
    "TrackPad": {
      "type": "PartDesign::Pad",
      "label": "Track",
      "membership": {
        "body": "TrackBody"
      },
      "placement": {
        "position_mm": ["0", "0", "0"],
        "rotation_xyzw": ["0", "0", "0", "1"]
      },
      "properties": {
        "Length": {
          "type": "App::PropertyLength",
          "value": "25",
          "unit": "mm"
        }
      }
    }
  },
  "dependencies": [
    ["TrackProfileSketch", "TrackPad"]
  ],
  "external_references": []
}
```

### Essential data

- Schema, generator version, and selected profile.
- Source filename and a digest of the normalized semantic model.
- Logical document name, label, and FreeCAD file version.
- Objects keyed by internal object name.
- Object type ID and label.
- Hierarchy and Body, Part, Group, Origin, and Tip membership.
- Meaningful persisted properties and their property types.
- Expression source text.
- Spreadsheet cell content and aliases.
- Canonical placements, attachments, map modes, and support references.
- Local and external links.
- Sketch geometry summaries and complete driving constraints.
- PartDesign feature parameters.
- Assembly joints, component references, and referenced subelements.
- Explicit dependencies.

Unknown custom properties should use a safe typed XML fallback where possible instead of being silently discarded.

### Optional diagnostic data

- Visibility and selected presentation properties.
- Raw BREP SHA-256.
- Raw FCStd archive SHA-256 for uncommitted diagnostics.
- Shape bounding box, volume, area, and element counts.
- Full sketch geometry rather than summaries.
- Recompute errors and invalid-object messages.
- Resolved external-sidecar digests.
- FreeCAD API-derived enum display names.
- Touched or frozen state.

Live geometry and recompute diagnostics normally require headless FreeCAD and must not be part of mandatory untrusted pull-request verification.

### Excluded by default

- Creation and modification timestamps.
- ZIP timestamps and compression metadata.
- Document UID and numeric object IDs.
- Transient status bitmasks.
- Camera, selection, tree expansion, thumbnail, and colors.
- `StringHasher` implementation details.
- Raw BREP and GUI binary arrays.
- Raw FCStd archive SHA-256 in the default committed profile.
- External-link modification stamps.
- Absolute filesystem paths.
- Python-object serialized blobs.
- Redundant placement axis-angle data.
- Duplicate summaries of the same property.

## 6. Determinism rules

The serialization contract should require:

- UTF-8 without BOM.
- LF newlines and exactly one final newline.
- Two-space JSON indentation.
- Keys sorted by Unicode code-point order.
- No trailing whitespace.
- Objects ordered by internal name and properties by property name.
- Dependencies deduplicated and sorted as tuples.
- Sorting only for semantically unordered collections; preserve meaningful list order.
- Finite numbers encoded as canonical decimal strings.
- Negative zero normalized to `"0"`.
- Redundant trailing zeros removed.
- Documented base units such as millimetres and radians, with the unit retained.
- NaN and infinity rejected rather than emitted as non-standard JSON.
- Expression source preserved without evaluation, except for newline normalization.
- Placements encoded as position plus normalized quaternion `x,y,z,w`.
- Quaternion sign normalized by choosing positive `w`, with a lexicographic tie-break when `w` is zero.
- Relative paths normalized to `/`, with case preserved.
- External references represented but not dereferenced.
- Timestamps, UIDs, numeric IDs, ZIP metadata, and GUI-only state excluded.
- A `source.semantic_sha256` computed over the canonical semantic model before the digest field is added.
- No raw archive digest in the committed semantic profile. A no-op FreeCAD save changes ZIP timestamps and therefore the raw FCStd SHA-256, which would reintroduce noisy diffs.
- Schema and generator version included; rule changes require an explicit version change.
- Atomic output through a temporary file followed by replacement.

## 7. Generation architecture

### Primary method: direct ZIP/XML parsing

The production exporter should parse the archive without launching FreeCAD.

It can safely extract:

- Object names, labels, and type IDs.
- Serialized properties, expressions, and placements.
- Links, membership, hierarchy, and dependencies.
- Spreadsheet cells and aliases.
- Sketch geometry and constraints.
- PartDesign and assembly parameters.
- Raw shape-data checksums without interpreting geometry.

It cannot reliably:

- Interpret every custom workbench property semantically.
- Import Python feature classes safely.
- Evaluate expressions or recompute the document.
- Calculate live bounding boxes, volumes, or topology counts.
- Diagnose recompute failures.
- Interpret opaque Python-object blobs.
- Resolve linked documents without consulting external state.

Use a hardened XML parser such as `defusedxml`. Existing [`doctools.py`](../src/Tools/doctools.py) is a useful defensive-parsing reference, although it is not a semantic exporter.

### Optional method: headless FreeCAD diagnostics

Headless FreeCAD provides a richer live API but is unsuitable as the mandatory exporter. Restoring Python features can import Python modules, as shown in [`PropertyPythonObject.cpp`](../src/App/PropertyPythonObject.cpp). Opening a document may also open external documents, invoke assembly behavior, or depend on installed workbenches.

Recommended flow:

```text
FCStd
  -> defensive archive reader
  -> Document.xml semantic parser
  -> canonical in-memory model
  -> deterministic JSON writer

Optional trusted command
  -> sandboxed FreeCADCmd diagnostics
  -> non-authoritative diagnostic report
```

### Opt-in post-save adapter

The automatic integration is a thin adapter around the standalone exporter. It must never serialize from the live `document` argument.

FreeCAD writes a temporary FCStd archive, closes it, applies its backup/rename policy, and then emits `signalFinishSave`. Python document observers receive this as `slotFinishSaveDocument(document, filename)`. This is the correct integration point because the callback identifies the finalized path after a successful save.

```text
Normal FreeCAD save
        |
        v
FCStd temporary archive written
        |
        v
Temporary archive renamed to final FCStd
        |
        v
signalFinishSave / slotFinishSaveDocument
        |
        v
Deterministic exporter parses finalized FCStd
        |
        v
FCStd.git.json.tmp-<unique-id> written and validated
        |
        v
Atomic replacement of FCStd.git.json
```

Component responsibilities remain separate:

```text
Standalone deterministic exporter
|- ZIP validation
|- Document.xml parsing
|- semantic normalization
|- deterministic JSON generation
`- atomic JSON writing

FreeCAD post-save adapter
|- checks the preference
|- classifies the final save target
|- prevents re-entrant export for the same path
|- invokes the exporter by filename
`- reports non-fatal warnings

CI verifier
|- regenerates expected JSON in memory
`- detects stale or missing sidecars
```

Conceptually:

```python
class GitSidecarSaveObserver:
    def slotFinishSaveDocument(self, document, filename):
        if not settings.generate_git_sidecar_after_save:
            return
        if not is_eligible_fcstd_target(filename):
            return

        try:
            export_sidecar_from_fcstd(
                source_path=filename,
                output_path=filename + ".git.json",
            )
        except Exception as exc:
            report_nonfatal_sidecar_warning(filename, exc)
```

The `document` parameter is notification context only. The finalized `filename` is the semantic input.

### Save eligibility and suppression

Generate only when:

- `GenerateGitSidecarAfterSave` is enabled.
- The final extension is `.FCStd`, case-insensitively.
- The save completed successfully.
- The target is not a recovery, autosave, backup, temporary, snapshot, or worker file.
- No export is already active for the canonical target path.

Path classification must cover FreeCAD recovery areas, MCP snapshots, managed snapshots, and isolated worker workspaces. Filename exclusions such as `*.FCStd1`, `*.FCStd2`, `*.bak`, `*.tmp`, `*.recovery`, and `~*` are useful but insufficient by themselves.

`saveCopy()` should produce a sidecar when its destination is a deliberate `.FCStd` user target. It should not produce one for an internal snapshot or temporary copy. Because the existing finish-save callback provides the document and filename but not the save purpose, internal snapshot/recovery callers need an explicit suppression context or application-level scoped flag, for example:

```python
with suppress_git_sidecar():
    document.saveCopy(snapshot_path)
```

### Atomic sidecar publication and failure behavior

Write the temporary JSON in the destination directory:

```text
AutoCurtains.FCStd.git.json.tmp-<unique-id>
```

Then write all bytes, flush, optionally `fsync`, validate the generated JSON, and atomically replace the final sidecar. Remove the temporary output on failure. A previous valid sidecar must remain untouched if generation fails.

Sidecar failure does not change the successful FCStd save result. Report a concise non-modal warning in Report View and, where available, the status bar or notification system. Keep detailed diagnostics in the log. Avoid a modal dialog on every failed save.

The first version should run synchronously. This prevents another save from racing the exporter, avoids shutdown lifetime problems, and gives direct error reporting. If measurements later show unacceptable latency for large assemblies, an asynchronous implementation can add per-path serialization, source revision checks, cancellation, and shutdown flushing.

## 8. Repository integration

Place the implementation in `tools/freecad_git/` as a standalone repository utility.

It should not initially be placed in:

- FreeCAD core, because the repository can validate the schema without changing the document format.
- The MCP submodule, because sidecars are useful independently of MCP and a submodule change requires a separate repository commit.
- The root `scripts/` directory, because the design needs a package, schema, tests, and documentation rather than one script.

Suggested commands:

```text
freecad-git export path/to/AutoCurtains.FCStd
freecad-git export --all
freecad-git check path/to/AutoCurtains.FCStd
freecad-git check --all
freecad-git export --stdout path/to/AutoCurtains.FCStd
```

Use `.freecad-git.toml` for include/exclude globs, resource limits, the default profile, property overrides, and external-reference policy.

The implementation must work for every `.FCStd`, not only AutoCurtains.

The adapter should be distributed as an optional FreeCAD add-on/bootstrap layer within the same tool package. It registers the observer and exposes the `GenerateGitSidecarAfterSave` preference, while all parsing and writing remains in the standalone package.

## 9. Git and CI workflow

Recommended developer workflow:

```text
Save in FreeCAD
-> optional post-save adapter runs freecad-git export automatically
-> inspect FCStd.git.json
-> stage FCStd and JSON together
-> commit
-> CI regenerates in memory and compares
```

Trigger policy:

- Manual command: always available as an explicit and recovery path.
- Pre-commit hook: optional convenience; hooks cannot be assumed to be installed.
- GitHub Actions: mandatory read-only verification.
- FreeCAD post-save adapter: automatic path when `GenerateGitSidecarAfterSave` is enabled.
- MCP post-save hook: optional adapter invoking the same standalone CLI.
- CI regeneration and commit: prohibited.

CI should find every tracked FCStd, confirm its sidecar exists, generate expected bytes without changing the checkout, compare them, and fail with a focused repair command. It may compare `source.semantic_sha256` first as a quick diagnostic, but the authoritative check remains full deterministic regeneration and byte comparison:

```text
Stale sidecar: AutoCurtains.FCStd.git.json

Run:
  freecad-git export AutoCurtains.FCStd
  git add AutoCurtains.FCStd AutoCurtains.FCStd.git.json
```

Recommended attributes:

```gitattributes
*.FCStd binary
*.fcstd binary
*.FCStd.git.json text eol=lf
*.fcstd.git.json text eol=lf
```

The sidecar is not a merge driver. Resolve a binary conflict manually or through a lock/rebase workflow, then regenerate it.

Large assemblies should use one sidecar per FCStd. Represent external documents as normalized references rather than recursively embedding them.

A compact schema is important because GitHub documents raw and rendered pull-request diff limits. See [GitHub repository limits](https://docs.github.com/en/repositories/creating-and-managing-repositories/repository-limits).

## 10. Security considerations

Treat every FCStd as hostile input.

The direct parser must:

- Never extract entries to arbitrary filesystem paths.
- Reject absolute paths, drive-qualified paths, `..`, traversal through backslashes, symlinks, duplicate names, and encrypted entries.
- Require one unambiguous `Document.xml`.
- Enforce configurable entry count, compressed size, uncompressed size, XML size, and compression-ratio limits.
- Reject DTDs and external XML entities.
- Bound runtime, memory, nesting depth, and list/property sizes.
- Avoid `eval`, expression evaluation, pickle, and Python-object deserialization.
- Avoid importing workbench or model-provided modules.
- Avoid opening external documents, following arbitrary paths, or accessing the network.

Initial limits can be approximately:

- 10,000 ZIP entries.
- 256 MiB compressed input.
- 512 MiB total uncompressed data.
- 64 MiB per XML document.
- A bounded compression ratio per entry.

CI should use read-only contents permissions, no secrets for fork pull requests, and no network or FreeCAD requirement.

Optional headless diagnostics should use a pinned container, isolated HOME and configuration, read-only model mounts, no network, temporary output, and CPU/memory/time limits. They should run only for trusted inputs.

## 11. Prototype results

The disposable prototype was created outside the repository and did not save or modify the original model.

### Deterministic export

Two exports of unchanged `AutoCurtains.FCStd` were byte-identical:

```text
Size: 1,154,201 bytes
SHA-256: 78412d7df5d5b9b61a428c964d7cbaee1c88980a59c8d6dd3fb0107974643ce4
```

The size shows that the generic prototype was too verbose. A production schema must avoid duplicate fields and summarize large repeated arrays.

### Focused semantic changes

| Test | Result |
|---|---|
| Spreadsheet `printerOffset`: `=0.2 mm` to `=0.25 mm` | One removed and one added line |
| Placement `Px`: `0` to `1` | One removed and one added line |
| Label `Track` to `Track renamed` | Four lines because the prototype duplicated the label; production should emit it once |
| GUI-only visibility change | No change in the semantic profile |
| GUI visibility with presentation profile | One removed and one added line |
| External references | Represented without opening target documents |
| PartDesign bodies and sketches | Successfully represented |
| Truncated or non-ZIP input | Rejected as an unsafe archive |

### Reproducible ZIP experiment

A deterministic no-op repacker using sorted entries and fixed metadata produced identical archives:

```text
Size: 1,140,406 bytes
SHA-256: a4c28f40521e5364a31ea8a4c96f42c63e8509897a3e6cb684eb54015b9a54ae
```

This proves deterministic ZIP construction is possible, but it neither stabilizes all FreeCAD payloads nor creates a readable PR diff.

### FreeCAD save experiment

Two `saveCopy()` operations on a trusted example produced different archive hashes:

```text
8ec763968fa8598f06f75560949f40e679a58678da98e09e9e4cb4c18c352708
d82007e414d8b33ee9913f461fed3e4ba49cfb0f17d5605019a144ee2b2b6291
```

All 36 entry payloads were unchanged; all 36 ZIP timestamps changed. Direct-parser sidecars for those archives were identical:

```text
Size: 77,880 bytes
SHA-256: b430f05a713c60549c7bee917bc4c19ed6546668c2b97be949d4b7264985e22f
```

### Git storage experiment

For two versions after aggressive Git packing:

| Representation | Git pack | Worktree |
|---|---:|---:|
| Deterministically packed FCStd | 1,064,192 B | 1,140,406 B |
| JSON sidecar | 51,635 B | 1,154,202 B |
| Unpacked FCStd | 626,162 B | 8,508,169 B |

The JSON has an initial worktree cost but packs and deltas efficiently. The unpacked approach increases worktree size and operational complexity substantially.

## 12. Exact files that would be added or modified

### Add

```text
tools/freecad_git/pyproject.toml
tools/freecad_git/README.md
tools/freecad_git/src/freecad_git/__init__.py
tools/freecad_git/src/freecad_git/cli.py
tools/freecad_git/src/freecad_git/archive.py
tools/freecad_git/src/freecad_git/document_xml.py
tools/freecad_git/src/freecad_git/model.py
tools/freecad_git/src/freecad_git/normalize.py
tools/freecad_git/src/freecad_git/freecad_adapter.py
tools/freecad_git/schema/freecad-git-sidecar.schema.json
tools/freecad_git/freecad_addon/GitSidecar/Init.py
tools/freecad_git/freecad_addon/GitSidecar/GitSidecarObserver.py
tools/freecad_git/tests/test_archive_security.py
tools/freecad_git/tests/test_determinism.py
tools/freecad_git/tests/test_semantic_diffs.py
tools/freecad_git/tests/test_external_references.py
tools/freecad_git/tests/test_freecad_adapter.py
tools/freecad_git/tests/fixtures/...
.freecad-git.toml
.github/workflows/freecad-sidecars.yml
```

Use small purpose-built FCStd fixtures rather than copying full user models.

### Modify

```text
.gitattributes
.pre-commit-config.yaml
CONTRIBUTING.md
```

No MCP submodule change is required in phase one. A later MCP adapter would need its own submodule-repository commit followed by an intentional gitlink update in the main repository.

## 13. Implementation phases

1. **Schema and golden fixtures**
   Finalize semantic inclusion rules, canonical values and units, schema versioning, and minimal representative fixtures.

2. **Safe direct exporter**
   Implement archive validation, hardened XML parsing, canonical model construction, JSON output, and the `export` and `check` commands.

3. **Repository workflow**
   Add attributes, configuration, documentation, manual commands, and read-only GitHub Actions verification.

4. **Opt-in post-save adapter**
   Add the FreeCAD observer, `GenerateGitSidecarAfterSave` preference, destination eligibility rules, snapshot/recovery suppression, re-entrancy guard, synchronous invocation, atomic sidecar publication, and non-fatal reporting. Test ordinary save, Save As, deliberate `saveCopy()`, suppressed snapshots, output failure, and unchanged repeated saves.

5. **Developer and MCP convenience**
   Add optional pre-commit integration and, if useful, an MCP adapter or suppression context. Every integration must invoke the standalone exporter and must not duplicate serialization logic.

6. **Diagnostic tooling**
   Add explicitly sandboxed headless FreeCAD reports for trusted documents, separate from mandatory sidecars.

7. **Independent reproducible-FCStd evaluation**
   Consider fixed ZIP metadata and ordering as a FreeCAD-core improvement, but not as a prerequisite or replacement for semantic sidecars.

## 14. Open risks and limitations

- Custom workbench and Python feature properties may be opaque without executing code.
- Internal object names are the best available stable identity, but object recreation can appear as removal and addition.
- Topological naming changes may cause broad link or geometry diffs.
- BREP hashes may differ across FreeCAD or OpenCASCADE versions.
- External path case and relative-path behavior vary across platforms.
- A sidecar may be stale or manually changed until CI verifies it.
- The two adjacent files cannot be published as one atomic transaction; a crash can leave a stale sidecar, which CI and `freecad-git check` must detect.
- A raw FCStd SHA-256 changes after no-op saves because ZIP timestamps change. It must not be committed in the default semantic profile.
- The finish-save signal does not expose save purpose, so internal `saveCopy()` snapshot callers need explicit suppression to avoid unwanted sidecars.
- Synchronous generation adds save latency; asynchronous generation is deferred until measurements justify its additional race and shutdown handling.
- JSON is not a safe reconstruction or merge format.
- Concurrent binary edits still require coordination, locking, or rebasing.
- Large assemblies may produce sidecars too large for comfortable GitHub rendering.
- If compacting is insufficient, a later schema may need sharded object JSON, but one sidecar per FCStd should be tried first.
- Direct parsing cannot provide trustworthy live recompute errors, evaluated expressions, volumes, or bounding boxes.
- Headless loading cannot be guaranteed passive merely by avoiding `save()`; it can import modules, follow external links, and invoke solvers.
- Git LFS may still be appropriate for very large authoritative binaries, but it is independent of the sidecar design.

## Decision

**RECOMMEND FCSTD + JSON SIDECAR**
