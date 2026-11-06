# OCCT 8 Boolean operations run serially

This note is the user-facing description of a temporary FreeCAD mitigation. The
fork keeps that kind of note under `doc/` (see the index in `doc/README.md`).
`docs/` only covers the WSLg launcher, and `src/Mod/Part/` has no narrative
notes.

## Symptom

On OCCT 8.x, a Boolean that runs on OCCT's internal thread pool races on
`TopoDS_TShape::myState` (`TopoDS_TShape.hxx:186`). That field is a `uint16_t`.
Bits 0-3 hold the shape type. The other bits hold the Free, Modified and
Checked flags. `setBit` (`TopoDS_TShape.hxx:176-182`) is a non-atomic
read-modify-write. Pool workers call `TopoDS_Builder::Add`, which clears Free,
and `BRep_Tool::Range`, which sets Modified, on sub-shapes that other workers
are reading with `ShapeType()`.

An OCC-only program reports the race under ThreadSanitizer on parallel cut,
fuse and common, and does not report it when those operations run one at a
time. A FreeCAD ThreadSanitizer build reports the same pairs during an ordinary
Part Cut, Fuse or Common recompute.

OCCT issue: TBD

## What FreeCAD does

`Part::booleanRunParallel()` is the only policy for Boolean-family algorithms
(`BRepAlgoAPI_*`, `BOPAlgo_*`, the BOP geometry check, defeaturing, split,
general fuse, maker volume, and the FreeCAD wrappers of those).

- OCCT >= 8.0.0: the algorithm runs serially. There is no upper version bound.
- OCCT < 8.0.0: FreeCAD keeps the previous behaviour and still runs in parallel
  where the caller asks for it.

The geometry check's "single-threaded" preference still forces a serial check.
On OCCT 8 the check stays serial when that preference is off as well. The process-wide
`BOPAlgo_Options` default is set through the same policy, so one geometry check
no longer leaves later Booleans, offsets, fillets and feature algorithms on the
pool for the rest of the process.

OCCT 7.x stores the same flags in `Standard_Integer myFlags` and updates them
with a non-atomic `setFlag`. `ShapeType()` is virtual there. That is the same
read-modify-write pattern by inspection. OCCT 7.x is outside this mitigation
and has not been verified safe.

Meshing and other `OSD_Parallel` users are unchanged.

## Performance

Measured on this shared machine with five runs of each case. The host was busy
(load average about 9.6 to 11.8). Treat the times as rough.

| Case | Parallel median | Serial median | Parallel spread | Serial spread |
| --- | ---: | ---: | ---: | ---: |
| Part Cut of two boxes | 0.019 s | 0.020 s | 0.117 s | 0.117 s |
| Part Fuse of two boxes | 0.021 s | 0.022 s | 0.004 s | 0.003 s |
| Part Common of two boxes | 0.017 s | 0.021 s | 0.003 s | 0.006 s |
| Fuse of 50 overlapping cylinders | 2.45 s | 4.73 s | 0.11 s | 0.35 s |
| PartDesign Pad plus 10 pockets | 0.55 s | 0.49 s | 0.10 s | 0.04 s |

Spread is max minus min. Volumes and `isValid()` matched between the two
policies (`35178.45` for the cylinders, `51200` for the pocket body, and the
usual box Cut `22000` / Fuse `25000` / Common `2000`). The cylinder fuse is the
case that shows the cost: the serial median was 4.73 s against a parallel
median of 2.45 s. The box operations and the pocket body stayed within a few
hundredths of a second.

Logs: `OCCSER/logs/time-old-paired.log` (parallel policy) and
`OCCSER/logs/time-new.log` (this mitigation).

## Retirement

Remove the OCCT 8 branch of `Part::booleanRunParallel()` only after an OCCT
release that contains a fix is verified with both of these:

- the OCC-only ThreadSanitizer program: 60 parallel runs, zero `myState` warnings
- the FreeCAD ThreadSanitizer proof: Part Cut, Fuse and Common, a PartDesign
  Pocket, a `generalFuse`, and a `Shape.check(True)` BOP check, with no warning
  whose stack has a `BOPAlgo_`, `BOPTools_` or `IntTools_` frame or a
  `TopoDS_TShape` frame

Then bound the version check to versions below that fixed release.

OCCT issue: TBD
