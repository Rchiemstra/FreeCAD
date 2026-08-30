<a href="https://freecad.org"><img src="/src/Gui/Icons/freecad.svg" height="100px" width="100px"></a>

### Your own 3D Parametric Modeler

[Website](https://www.freecad.org) •
[Documentation](https://wiki.freecad.org) •
[Forum](https://forum.freecad.org/) •
[Bug tracker](https://github.com/FreeCAD/FreeCAD/issues) •
[Git repository](https://github.com/FreeCAD/FreeCAD) •
[Blog](https://blog.freecad.org)


[![Release](https://img.shields.io/github/release/freecad/freecad.svg)](https://github.com/freecad/freecad/releases/latest) [![Crowdin](https://d322cqt584bo4o.cloudfront.net/freecad/localized.svg)](https://crowdin.com/project/freecad)

<img src="/.github/images/partdesign.png" width="800"/>

Fork additions — `FreeCAD-start`
--------------------------------

This branch is upstream FreeCAD plus an agent-oriented CAD toolchain: assembly
review and interference tooling, a scriptable Assembly API, detachable document
windows, a collaboration core for coordinated edits, deterministic Git sidecars
for `.FCStd` files, and an MCP server. Measured against `main`: **175 commits,
405 files, +89,690 / −3,499 lines**.

<img src="/.github/images/fork-features.gif" width="800"/>

### Assembly workbench

| Addition | What it does | Source |
| --- | --- | --- |
| Review Notes | `Assembly::ReviewNote` / `ReviewNoteGroup` — 3D-anchored review annotations with leader lines glued to component faces, `@Object.FaceN` autocomplete, clickable refs, and resolved/broken states | [ReviewNote.cpp](src/Mod/Assembly/App/ReviewNote.cpp), [CommandReviewNote.py](src/Mod/Assembly/CommandReviewNote.py) |
| Interference detection | `Check Interference` and `Check Selected Components` commands, backed by a cancellable scan session with per-pair exclusion rules that can cite a Review Note as the reason | [InterferenceScan.cpp](src/Mod/Assembly/App/InterferenceScan.cpp), [TaskInterferenceCheck.cpp](src/Mod/Assembly/Gui/TaskInterferenceCheck.cpp) |
| Webots PROTO export | Exports an assembly as a Webots R2025a PROTO robot description | [WebotsExport.py](src/Mod/Assembly/WebotsExport.py) |
| Headless Python API | `Assembly.api` — create assemblies, joints and joint references without the GUI | [api.py](src/Mod/Assembly/Assembly/api.py) |
| Nested-container fixes | Attachment placement across nested `App::Part` groups, cross-body datum staleness after joint moves, overlay icons and Origin-in-Group handling | [UtilsAssembly.py](src/Mod/Assembly/UtilsAssembly.py), [Groups.cpp](src/Mod/Assembly/App/Groups.cpp) |

### Sketcher

| Addition | What it does | Source |
| --- | --- | --- |
| Delta position constraint | Driving/driven constraint for the dx/dy between two points, with an edit dialog and toolbar entry | [EditDeltaPositionDialog.cpp](src/Mod/Sketcher/Gui/EditDeltaPositionDialog.cpp) |
| Constrained offset modes | The offset tool emits geometry that stays constrained instead of loose copies | [DrawSketchHandlerOffset.h](src/Mod/Sketcher/Gui/DrawSketchHandlerOffset.h) |
| `addExternal()` returns a GeoId | External geometry is addressable from scripts immediately after creation | [SketchObjectPyImp.cpp](src/Mod/Sketcher/App/SketchObjectPyImp.cpp) |
| Malformed-constraint guard | Single-point `DistanceX`/`DistanceY` constraints raise on creation instead of producing a corrupt sketch | [SketchObjectConstraints.cpp](src/Mod/Sketcher/App/SketchObjectConstraints.cpp) |

### Part

| Addition | What it does | Source |
| --- | --- | --- |
| `removeSplitter()` corruption fix | Stops the refine pass mutating the shape it was called on | [modelRefine.cpp](src/Mod/Part/App/modelRefine.cpp) |
| `Direction` / `Normal` aliases | `Part.Circle` and `Part.Ellipse` expose both as aliases of `Axis`, removing the Direction-vs-Axis trap | [ConicPyImp.cpp](src/Mod/Part/App/ConicPyImp.cpp) |
| Interference kernel | Shared clash and clearance primitives used by the Assembly scan | [InterferenceDetection.cpp](src/Mod/Part/App/InterferenceDetection.cpp) |
| Placement regression tests | Shape/Placement absorption, Deactivated placement, and FlatFace origin repros | [TopoShapeTest.py](src/Mod/Part/parttests/TopoShapeTest.py) |

### GUI and windowing

| Addition | What it does | Source |
| --- | --- | --- |
| Drag-to-detach document tabs | Pull a document tab out of the MDI area into its own top-level window | [MainWindow.cpp](src/Gui/MainWindow.cpp) |
| Per-document window layouts | Floating view geometry is saved and restored per `.FCStd`, clamped to the available screens and pruned over time | [WindowLayout.cpp](src/Gui/WindowLayout.cpp) |
| Detached-view navigation | Guarded redraw and null-camera handling so orbit and zoom stay live in detached 3D views | [View3DInventorViewer.cpp](src/Gui/View3DInventorViewer.cpp) |
| Spreadsheet windows | Spreadsheets take part in the same detach and restore flow as 3D views | [SpreadsheetView.h](src/Mod/Spreadsheet/Gui/SpreadsheetView.h) |
| Presentation split | `PersonalViewContext` and `SharedPresentationCoordinator` separate per-user view state from shared document state | [SharedPresentationCoordinator.cpp](src/Gui/SharedPresentationCoordinator.cpp) |
| Recovery hardening | Fixes a Zip stream leak that falsely marked `.FCStd` files corrupted, and exports the recovery validators | [DocumentRecovery.cpp](src/Gui/DocumentRecovery.cpp) |

### App core — document collaboration

| Addition | What it does | Source |
| --- | --- | --- |
| Collaboration service | `DocumentCollaborationService` coordinates external (agent) edits against a live document | [DocumentCollaborationService.cpp](src/App/DocumentCollaborationService.cpp) |
| Edit sessions | Pointer-free advisory session metadata, plus `PreparedEdit` and `PreparedEditExecutor` for staged mutations | [EditSession.h](src/App/EditSession.h) |
| Commit coordinator | Serialises and classifies mutations (`MutationClassification`, `MutationKind`) before they reach the document | [DocumentCommitCoordinator.cpp](src/App/DocumentCommitCoordinator.cpp) |
| Revision index | Stable per-document revision tracking used for diffing and fencing | [DocumentRevisionIndex.cpp](src/App/DocumentRevisionIndex.cpp) |
| Collaborative operations | Typed operation registry with Part boolean and set-property implementations | [CollaborativeOperationRegistry.cpp](src/App/CollaborativeOperationRegistry.cpp) |
| Recovery snapshots | `RecoverySnapshot` and `App.advanceDocumentCollaborationEpoch()` preserve recovery state while cancelling in-flight work | [RecoverySnapshot.cpp](src/App/RecoverySnapshot.cpp) |

### Tooling

| Addition | What it does | Source |
| --- | --- | --- |
| `freecad-git` | Deterministic `Model.FCStd.git.json` review sidecars — defensive archive validation, semantic `Document.xml` parsing, schema-checked atomic writes, and a CLI (`export`, `check`, `validate`, `diagnostics`) | [tools/freecad_git/](tools/freecad_git/) |
| GitSidecar addon | FreeCAD workbench that regenerates a sidecar after a successful save | [freecad_addon/GitSidecar](tools/freecad_git/freecad_addon/GitSidecar) |
| `freecad-mcp` | MCP server submodule for driving FreeCAD from an agent — see its own [README](tools/mcp/freecad-mcp/README.md) | [tools/mcp/freecad-mcp](tools/mcp/freecad-mcp) |
| Build and launch scripts | `build_freecad.py`, `start_freecad.py`, and Windows release launchers | [build_freecad.py](build_freecad.py), [start_freecad.py](start_freecad.py) |

### CI and packaging

| Addition | What it does | Source |
| --- | --- | --- |
| Woodpecker pipeline | Configure, build, lint, unit, integration, e2e and Linux release stages with persistent ccache | [.woodpecker/ci.yml](.woodpecker/ci.yml) |
| CI image builds | Kaniko-built dependency and MCP images published to a dedicated registry | [ci/build-images.sh](ci/build-images.sh), [.woodpecker/build-images.yml](.woodpecker/build-images.yml) |
| Bundled Coin3D and Pivy | `src/3rdParty/coin` and `src/3rdParty/pivy` submodules, so CI no longer depends on distro packages | [.gitmodules](.gitmodules) |
| Sidecar and MCP workflows | GitHub Actions for the `freecad-git` and `freecad-mcp` suites | [freecad-sidecars.yml](.github/workflows/freecad-sidecars.yml), [mcp-tests.yml](.github/workflows/mcp-tests.yml) |
| Ubuntu package image | Reproducible Ubuntu build image with the full dependency set | [package/ubuntu/Dockerfile](package/ubuntu/Dockerfile) |


Overview
--------

* **Freedom to build what you want**  FreeCAD is an open-source parametric 3D 
modeler made primarily to design real-life objects of any size. 
Parametric modeling allows you to easily modify your design by going back into 
your model history to change its parameters. 

* **Create 3D from 2D and back** FreeCAD lets you sketch geometry-constrained
 2D shapes and use them as a base to build other objects.
 It contains many components to adjust dimensions or extract design details from 
 3D models to create high quality production-ready drawings.

* **Designed for your needs** FreeCAD is designed to fit a wide range of uses
including product design, mechanical engineering and architecture,
whether you are a hobbyist, programmer, experienced CAD user, student or teacher.

* **Cross platform** FreeCAD runs on Windows, macOS and Linux operating systems.

* **Underlying technology**
    * **OpenCASCADE** A powerful geometry kernel, the most important component of FreeCAD
    * **Coin3D library** Open Inventor-compliant 3D scene representation model
    * **Python** FreeCAD offers a broad Python API
    * **Qt** Graphical user interface built with Qt


Installing
----------

Precompiled packages for stable releases are available for Windows, macOS and Linux on the
[latest releases page](https://github.com/FreeCAD/FreeCAD/releases/latest).

On most Linux distributions, FreeCAD is also directly installable from the 
software center application.

For weekly development releases visit the [releases page](https://github.com/FreeCAD/FreeCAD/releases/).

Other options are described on the [wiki Download page](https://wiki.freecad.org/Download).

Compiling
---------

See the [Developers Handbook – Getting Started](https://freecad.github.io/DevelopersHandbook/gettingstarted/)
for build instructions.


Reporting Issues
---------

To report an issue please:

- Consider posting to the [Forum](https://forum.freecad.org), [Discord](https://discord.com/invite/w2cTKGzccC) channel, or [Reddit](https://www.reddit.com/r/FreeCAD) to verify the issue; 
- Search the existing [issues](https://github.com/FreeCAD/FreeCAD/issues) for potential duplicates; 
- Use the most updated stable or [development versions](https://github.com/FreeCAD/FreeCAD/releases/) of FreeCAD; 
- Post version info from `Help > About FreeCAD > Copy to clipboard`; 
- Restart FreeCAD in safe mode `Help > Restart in safe mode` and try to reproduce the issue again. If the issue is resolved it can be fixed by deleting the FreeCAD config files.
- Start recording a macro `Macro > Macro recording...` and repeat all steps. Stop recording after the issue occurs and upload the saved macro or copy the macro code in the issue; 
- Post a Step-By-Step explanation on how to recreate the issue; 
- Upload an example file (FCStd as ZIP file) to demonstrate the problem; 

For more details see:

- [Bug Tracker](https://github.com/FreeCAD/FreeCAD/issues)
- [Reporting Issues and Requesting Features](https://github.com/FreeCAD/FreeCAD/issues/new/choose)
- [Contributing](https://github.com/FreeCAD/FreeCAD/blob/main/CONTRIBUTING.md)
- [Help Forum](https://forum.freecad.org/viewforum.php?f=3)

> [!NOTE]
The [FPA](https://fpa.freecad.org) offers developers the opportunity
to apply for a grant to work on projects of their choosing. Check
[jobs and funding](https://blog.freecad.org/jobs/) to know more.


Usage & Getting Help
--------------------

The FreeCAD wiki contains documentation on 
general FreeCAD usage, Python scripting, and development.
View these pages for more information:

- [Getting started](https://wiki.freecad.org/Getting_started)
- [Features list](https://wiki.freecad.org/Feature_list)
- [Frequent questions](https://wiki.freecad.org/FAQ/en)
- [Workbenches](https://wiki.freecad.org/Workbenches)
- [Scripting](https://wiki.freecad.org/Power_users_hub)
- [Developers Handbook](https://freecad.github.io/DevelopersHandbook/)

The [FreeCAD forum](https://forum.freecad.org) is a great place
to find help and solve specific problems when learning to use FreeCAD.

---

<p>This project receives generous infrastructure support from
  <a href="https://www.digitalocean.com/">
    <img src="https://opensource.nyc3.cdn.digitaloceanspaces.com/attribution/assets/SVG/DO_Logo_horizontal_blue.svg" width="91px">
  </a> and <a href="https://www.kipro-pcb.com/">KiCad Services Corp.</a>
</p>
