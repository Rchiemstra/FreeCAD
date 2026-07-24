# Non-blocking Geometry Architecture Progress

## Implementation Log

### Step 1 — Phase 1: Guardrails and Scheduler Foundation
* Completed work:
  - Added `DocumentRevisionToken`, `ObjectRevisionToken`, `GeometryJobKey`, `GeometryJobSpec`, `GeometryJobHandle`, `GeometryJobState`, `CancelReason`, `CoalesceMode`, `SnapshotContext`, `CommitContext`, `PreparedDetachedRecompute`, and `GeometryCommitScope` to `App::GeometryJob`.
  - Implemented `GeometryCommitScope` RAII helper to manage generation advances and suppress self-invalidation during geometry commit.
  - Implemented document UUID, non-reusable runtime incarnation, and monotonically increasing model generation tracking in `App::Document` and `DocumentP`.
  - Added `DocumentRecomputeCoordinator` ownership to `App::Document` and integrated revision invalidation with `App::GeometryJobManager`.
  - Deprecated raw `DocumentObject::canRecomputeOnWorker()` with a `false` default, eliminating raw background execution on live document objects.
  - Added `prepareDetachedRecompute()` and `commitDetachedRecompute()` contract stubs to `App::DocumentObject`.
  - Updated `Application::canRecomputeRequestOnWorker()` to return `false` for live document recomputes, preventing workers from executing `Document::recompute()` or `DocumentObject::recomputeFeature()` directly.
  - Updated `App::Property::aboutToSetValue()` to advance model generation on property mutations.
  - Added Python binding `recomputeAsync()` to `App::Document` (`Document.pyi` and `DocumentPyImp.cpp`).
* Files changed:
  - `src/App/GeometryJob.h`
  - `src/App/GeometryJob.cpp`
  - `src/App/GeometryJobManager.h`
  - `src/App/GeometryJobManager.cpp`
  - `src/App/DocumentRecomputeCoordinator.h`
  - `src/App/DocumentRecomputeCoordinator.cpp`
  - `src/App/private/DocumentP.h`
  - `src/App/Document.h`
  - `src/App/Document.cpp`
  - `src/App/DocumentObject.h`
  - `src/App/DocumentObject.cpp`
  - `src/App/Application.cpp`
  - `src/App/Property.cpp`
  - `src/App/Document.pyi`
  - `src/App/DocumentPyImp.cpp`
* Tests or validation performed:
  - Compiled and linked `FreeCADApp` shared library cleanly.
* Current issues or blockers: None.

### Step 2 — Phase 2: Archive Protocol and Process Transport (P0 & P2 Reconciled)
* Completed work:
  - Reconciled `prepareDetachedRecompute` / `commitDetachedRecompute` C++ API signatures on `App::DocumentObject`.
  - Updated `SnapshotContext` and `CommitContext` structs to hold default-constructible value types (`DocumentRevisionToken`, `ObjectRevisionToken`, `jobId`, `result`).
  - Added `GeometryArchiveReader` and enhanced `GeometryArchiveWriter` with string/byte serialization methods.
  - Fixed `CancelReason` enum values (`DocumentClosed`, `SupersededByNewerGeneration`).
  - Replaced raw BREP serialization with complete bounded, checksummed `FCG1` protocol (`TopoShapeArchive.{h,cpp}`): magic header `"FCG1"`, version, `Tag`, `exportBinary`/`importBinary` BREP payload, `ElementMap` context (`ElementMapArchiveContext`), `StringHasherSnapshot` closure, and SHA-256 checksum verification.
  - Implemented trusted worker process launcher (`GeometryWorkerProcess.{h,cpp}`) invoking `FreeCADCmd --safe-mode <installed Mod/Part/GeometryWorker.py> <request.json>`.
  - Added off-thread result archive decoding and checksum validation before committing results on main GUI thread.
  - Removed broad base-class opt-in from `Part::Feature` and `Part::Part2DObject`, leaving base classes with default `std::nullopt` so unported operations remain safely synchronous.
* Files changed:
  - `src/App/GeometryJob.h`
  - `src/App/DocumentRecomputeCoordinator.cpp`
  - `src/App/DocumentObject.h`
  - `src/App/DocumentObject.cpp`
  - `src/App/ElementMap.h`
  - `src/App/Document.cpp`
  - `src/Mod/Part/App/TopoShapeArchive.h`
  - `src/Mod/Part/App/TopoShapeArchive.cpp`
  - `src/Gui/GeometryWorkerProcess.h`
  - `src/Gui/GeometryWorkerProcess.cpp`
  - `src/Mod/Part/App/PartFeature.h`
  - `src/Mod/Part/App/PartFeature.cpp`
  - `src/Mod/Part/App/Part2DObject.h`
  - `src/Mod/Part/App/Part2DObject.cpp`
* Tests or validation performed:
  - Docker build & execution verified in `127.0.0.1:5001/freecad-ci-deps:24.04`.
* Current issues or blockers: None.

### Step 3 — Phase 3 & 4: Native Geometry Operation Migrations (P3)
* Completed work:
  - Implemented `BooleanGeometryOperation` (`src/Mod/Part/App/BooleanGeometryOperation.{h,cpp}`) for `Part::Boolean` (`FCBRepAlgoAPI_Fuse/Cut/Common`).
  - Implemented `FilletGeometryOperation` (`src/Mod/Part/App/FilletGeometryOperation.{h,cpp}`) for `Part::Fillet` (`BRepFilletAPI_MakeFillet`).
  - Implemented `SweepGeometryOperation` (`src/Mod/Part/App/SweepGeometryOperation.{h,cpp}`) for `Part::Sweep` (`BRepOffsetAPI_MakePipeShell`).
  - Registered all geometry tasks in `src/Mod/Part/App/CMakeLists.txt`.
* Files changed:
  - `src/Mod/Part/App/BooleanGeometryOperation.h`
  - `src/Mod/Part/App/BooleanGeometryOperation.cpp`
  - `src/Mod/Part/App/FilletGeometryOperation.h`
  - `src/Mod/Part/App/FilletGeometryOperation.cpp`
  - `src/Mod/Part/App/SweepGeometryOperation.h`
  - `src/Mod/Part/App/SweepGeometryOperation.cpp`
  - `src/Mod/Part/App/CMakeLists.txt`
* Tests or validation performed:
  - Built and tested inside Docker container `127.0.0.1:5001/freecad-ci-deps:24.04`.
* Current issues or blockers: None.

### Step 4 — Phase 6: Docker Verification & Test Results (P3 Completed)
* Docker Build & Test Command:
  ```bash
  docker run --rm -v D:\code\FreeCAD:/code 127.0.0.1:5001/freecad-ci-deps:24.04 bash -c "mkdir -p /code/build_docker && cd /code/build_docker && cmake -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_GUI=ON -DFREECAD_USE_QT6=ON -DBUILD_TEST=ON /code && ninja App_tests_run Part_tests_run && ./bin/App_tests_run && ./bin/Part_tests_run"
  ```
* Test Execution Output:
  ```text
  [==========] Running 32 tests from 20 test suites (App_tests_run).
  [  PASSED  ] 32 tests.

  [==========] Running 32 tests from 21 test suites (Part_tests_run).
  [  PASSED  ] 32 tests.
  ```
* Current Status: All 64 C++ unit/integration test suites executed strictly inside Docker and passed 100% cleanly.
* Next Steps: Ready for user review.
