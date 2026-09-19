# Photo-inspection validation

The normative catalog is `TestManifest.json`. It contains 178 mandatory test
records and is validated before any CMake configuration. Physical records stay
`physical-evidence-required`; an automated or synthetic result must never mark
one complete.

The focused C++ executable currently covers the deterministic App-domain
boundary: OpenCV capability/version compatibility, canonical planar geometry,
sheet vectors, strict profiles, safe image preflight/decode, QR/ArUco
detection, homography rectification, segmentation, comparison, dimensions,
uncertainty, lifecycle fencing, sealed objects, reports, and atomic output.
The separate GUI executable parses the emitted PDF media box, exercises
A4/A3 orientation and atomic output, and compile-checks the layered review
widget and vector renderer.

Run from the repository root on Windows:

```powershell
.\scripts\ci\photo-inspection-validate.ps1 -Mode off
.\scripts\ci\photo-inspection-validate.ps1 -Mode on -Image freecad-photo-inspection-opencv:4.6
.\scripts\ci\photo-inspection-validate.ps1 -Mode on -Image freecad-photo-inspection-opencv:4.10
.\scripts\ci\photo-inspection-validate.ps1 -Mode on -Image freecad-photo-inspection-opencv:4.13-source
```

The Linux shell wrapper provides the same modes. Each run:

1. mounts the host source read-only;
2. creates unique build/home volumes and uses a compiler-cache-only shared
   volume;
3. disables networking;
4. excludes the MCP submodule from the snapshot;
5. validates the 178-case manifest and typed MCP integration contract;
6. configures the requested OpenCV capability;
7. builds `Inspection`, `InspectionGui`, `Inspection_tests_run`, and
   `InspectionGui_tests_run`;
8. runs every App and digital print-path `PhotoInspection*` test without
   starting FreeCAD or MCP; and
9. emits provenance and the `ALL_MANDATORY_PASSED` sentinel only after the
   exact expected test count passes.

Logs and source manifests are written under `build/photo-inspection-ci/`.
Those logs are evidence for the local Linux container lane only. They do not
replace Windows/macOS installed-package testing, physical printer/camera work,
or the isolated MCP end-to-end lane.

## Adding coverage

Every new test should map to one or more catalog IDs only when it exercises the
complete stated stimulus and oracle. Partial coverage remains `planned`.
Security and boundary tests must include the exact limit and the plus-one case.
No lane may use a skip to satisfy a mandatory record; capability-off behavior
must have an explicit truthful oracle.

Phase gates require all assigned tests, provenance, and external evidence.
Rerunning a failed race/physical case to green does not erase the failure; the
defect and rerun evidence both remain part of the gate record.
