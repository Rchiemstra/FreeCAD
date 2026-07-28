# Interference Detection — Progress

## Status

**PASS — implementation complete and required validation green (2026-07-28).**

Check Interference is selection-aware for an active `App::Part` or Assembly.
Exactly two component/subelement picks select one top-level occurrence pair;
other selection cardinalities scan all applicable cross-component occurrences.
Hidden occurrences are omitted by default and included only when requested.

The final review covered occurrence identity and placement, hidden and linked
array behavior, spreadsheet parsing and canonical face paths, face-hit
classification/provenance, exclusion persistence and rollback, zero-row
results, exception recovery, stale/cancel/obsolete generations, document
closure, preview cleanup, GUI transactions, and worker/test-hook ownership.

## Completed milestones

| Commit | Milestone |
|--------|-----------|
| `7ddcdec448` | Recover worker exceptions as visible incomplete results (Task 7A baseline) |
| `4836d32566` | Recover synchronous preparation and launch failures |
| `528bfb9e4b` | Align table data and 3D preview with the governing face hit |
| `52234de1cf` | Share exclusion eligibility and affected-pair counting |
| `fd3f04ad60` | Make detached exclusion identity opt-in and make GUI mutations exception-safe |

Earlier feature commits provide selection-aware scope, placed/nested occurrence
collection, collapsed and expanded link-array handling, face-specific
spreadsheet clearances, persistent exclusions, accepted zero-row results, and
the mandatory GUI lifecycle harness.

## Final validation

Isolated Docker validation used a disposable `--rm --network=none` container
and disposable result volume. The retained seed volume was read-only and used
only to accelerate the build.

| Evidence | Result |
|----------|--------|
| Stamp | `20260728T145157Z-12989` |
| Build | `BUILD_EXIT:0` |
| Part | `InterferenceDetectionTest.*`: **23/23 passed** |
| Assembly | `InterferenceScanTest.*`: **107/107 passed**; `AssemblyObjectTest.*`: **1/1 passed**; **108/108 total** |
| AssemblyGui offscreen | `TaskInterferenceCheckTest.*`: **45/45 passed** |
| AssemblyGui Xvfb/xcb | mandatory lifecycle/preview subset: **8/8 passed** |
| Required markers | `ASSEMBLYGUI_PLATFORM_GUARD_OK platform=xcb`; `ALL_MANDATORY_PASSED` |
| Platform negative control | offscreen with `ASSEMBLYGUI_REQUIRE_XCB=1` rejected as expected: `NEGATIVE_OFFSCREEN_XCB_REJECTED_AS_EXPECTED` |
| Static checks | CRLF-aware `git diff --check`: pass; both validator scripts `bash -n`: pass |

The wrapper’s actual build command was
`cmake --build /data/build -j"$(nproc)"`; no lower parallel level is claimed.
The version-file step emitted expected messages because the isolated source
snapshot excludes Git metadata and has no Bazaar/Subversion clients
(`not a git repository`, `bzr`/`svnversion`/`svn` not found). The build still
completed successfully, and no compile or test failure was hidden by them.

## Remaining deferred work

- Continuous/live checking and multi-assembly batch workflows remain outside
  this on-demand feature.
- Broad-phase pairing is conservative but still grows quadratically for dense
  assemblies; larger-scale indexing is a future optimization.
- Cancellation is cooperative between expensive OCCT operations and cannot
  pre-empt a kernel call already in progress.
- Geometric classification near modeling tolerance remains subject to normal
  OCCT robustness limits; invalid or inconclusive results fail closed and stay
  visible.
- The final gate is the focused Part/Assembly/AssemblyGui feature matrix, not a
  claim that every unrelated FreeCAD module or platform suite was executed.
