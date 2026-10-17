# Test process quarantine (tech-debt #5, package A2)

`test-process-quarantine.json` is the explicit exception list used by
`check-test-results.py gtest`. An empty list means every process must exit 0.
Both the unit and nonblocking runners use `run-gtest.sh` to check every
invocation, including each isolated GUI filter.

An exception must identify the binary, exact gtest filter, failure mode, reason,
and a link (`tracking`) to a note explaining the unresolved bug and removal
criteria. No wildcard matching is performed on binary names or filters.
The only supported exception mode is `signal:11-after-results`: a native
SIGSEGV after complete, green XML and exactly one PASSED summary whose count
matches the executed XML records, with no FAILED marker. Explicit exit(139),
other signals, nonzero normal exits, missing/truncated/failing XML, zero
execution, and other binaries/filters fail. The checker parents the native
test process inside xvfb so a signal cannot be confused with exit(139).
Before launch it removes any previous XML at the report path.

## Investigation

Run date: 2026-10-17. Build and execution: 167.233.198.185, exclusively
`W=/srv/freecad-ci/FreeCAD-2`, CI deps image `freecad-ci-deps:24.04`, 6 GB run
limit. Baseline source: package A1, `8e94a061e4`. Build log:
`/srv/build-A2.log`; diagnostic log: `/srv/diagnose-A2.log`; per-invocation
logs, XML, and exit/signal records:
`/srv/freecad-ci/FreeCAD-2/.a2-evidence/`.

Every inventory binary is run unfiltered three times. In addition, every GUI
batch used by the unit runner and every distinct nonblocking lane/presentation
invocation is run three times. GUI processes use xcb under xvfb; App processes
run from `/tmp`, matching the unit runner's filesystem requirement.
Nonzero processes with PASSED and no FAILED marker are rerun under bounded
gdb (`-batch -nx`, `run`, `thread apply all bt 30`), following the e2e runner's
parent-debugger diagnostic approach.

Observed results: all 75 processes exited 0 (no signal), with PASSED and no
FAILED marker. No teardown crash or late nonzero exit reproduced, so there
is no backtrace to report, no diagnosed teardown bug to patch, and no
quarantine entry justified by this audit. Historical waiver comments alone
are not evidence for a quarantine.

| Binary | Filter | Exit codes (runs 1/2/3) | Signals |
| --- | --- | --- | --- |
| `App_tests_run` | `*` | 0 / 0 / 0 | none |
| `Base_tests_run` | `*` | 0 / 0 / 0 | none |
| `Zipios_tests_run` | `*` | 0 / 0 / 0 | none |
| `Gui_tests_run` | `*` | 0 / 0 / 0 | none |
| `GuiShutdown_tests_run` | `*` | 0 / 0 / 0 | none |
| `Assembly_tests_run` | `*` | 0 / 0 / 0 | none |
| `AssemblyGui_tests_run` | `*` | 0 / 0 / 0 | none |
| `CAM_tests_run` | `*` | 0 / 0 / 0 | none |
| `Material_tests_run` | `*` | 0 / 0 / 0 | none |
| `Measure_tests_run` | `*` | 0 / 0 / 0 | none |
| `Mesh_tests_run` | `*` | 0 / 0 / 0 | none |
| `MeshPart_tests_run` | `*` | 0 / 0 / 0 | none |
| `Part_tests_run` | `*` | 0 / 0 / 0 | none |
| `PartDesign_tests_run` | `*` | 0 / 0 / 0 | none |
| `Points_tests_run` | `*` | 0 / 0 / 0 | none |
| `Sketcher_tests_run` | `*` | 0 / 0 / 0 | none |
| `Spreadsheet_tests_run` | `*` | 0 / 0 / 0 | none |
| `Start_tests_run` | `*` | 0 / 0 / 0 | none |
| `TechDraw_tests_run` | `*` | 0 / 0 / 0 | none |
| `Gui_tests_run` | `-CollaborationDomainIntegration*:CollaborationDomainIntegrationStandalone*:CollaborationViewIsolation*:CollaborationResponsiveness*` | 0 / 0 / 0 | none |
| `Gui_tests_run` | `CollaborationDomainIntegration*:CollaborationDomainIntegrationStandalone*` | 0 / 0 / 0 | none |
| `Gui_tests_run` | `CollaborationViewIsolation*` | 0 / 0 / 0 | none |
| `Gui_tests_run` | `CollaborationResponsiveness*` | 0 / 0 / 0 | none |
| `Gui_tests_run` | `DocumentExecution*:DocumentPresentationCache*:PresentationApplyScheduler*` | 0 / 0 / 0 | none |
| `App_tests_run` | `DocumentExecutionLane*:RecomputeHandle*:DocumentRecomputeCoordinator*:DocumentCommitCoordinator*` | 0 / 0 / 0 | none |
