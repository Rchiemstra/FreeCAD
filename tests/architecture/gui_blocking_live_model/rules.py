# SPDX-License-Identifier: LGPL-2.1-or-later
"""Search rules for the GUI blocking and live-model ingress inventory.

Each category below is a pair of deliberately narrow regular expressions -- one
applied to C++ source and one to Python source -- both run *after* comments and
string/char/raw literals have been masked out (see :mod:`scanner`). Keeping the
patterns narrow is what makes the inventory reproducible and keeps the
false-positive surface small.

Scope
-----
The inventory covers production GUI source only:

* ``src/Gui`` (the GUI framework),
* every ``Gui`` directory under ``src/Mod`` (workbench GUI code), including the
  nested Python GUI directories such as ``src/Mod/CAM/Path/*/Gui``,
* every top-level production workbench ``InitGui.py`` (the workbench entry
  point is GUI code even when the rest of its Python package mixes model and
  presentation helpers), and
* an explicit, reviewed set of Python GUI packages/files that are not named
  ``Gui``.

Both C++ (``.cpp``/``.h``/``.hpp``) and Python (``.py``) GUI source are
scanned. The module-aware additions are intentionally explicit and follow the
Python modules loaded by production workbench entry points: command/task/view
provider packages, GUI utility modules, and GUI preference pages. This keeps
App-layer helpers, tests, and import/export implementations out of scope while
making each inclusion reviewable and deterministic.

The ``update-data-provider`` rule uses its C++ signature pattern and a
provider-aware Python AST classifier for ``ViewProvider.updateData`` callbacks;
the Python side intentionally has no regex pattern because generic Qt model
``updateData(topLeft, bottomRight)`` methods are not providers. ``blocking-invokes``
has both a C++ pattern (``Qt::BlockingQueuedConnection``) and a Python pattern
(the PySide ``BlockingQueuedConnection`` enum); no Python GUI site uses it
today, but the rule is recorded so any future use is inventoried.

Dispositions
------------
Every finding is stamped with a migration disposition:

* ``migrate``    -- must move off the current mechanism to a nonblocking or
                    committed-state/value-only route before the nonblocking
                    cutover can complete.
* ``investigate``-- needs per-site triage; the mechanism is suspect but some
                    uses are legitimate (for example a shutdown-time join, or a
                    one-shot ``processEvents`` flush).
* ``accepted``   -- legitimate and expected to remain; recorded so reviewers
                    can see it was considered, not missed.

Defaults are assigned per category below; a finding can be re-classified in the
report after triage. The defaults encode the *baseline* migration posture.
"""

from __future__ import annotations

from dataclasses import dataclass

#: C++ source suffixes.
CPP_SUFFIXES: frozenset[str] = frozenset({".cpp", ".h", ".hpp"})

#: Python source suffixes.
PY_SUFFIXES: frozenset[str] = frozenset({".py"})

#: All source suffixes the scanner reads.
SOURCE_SUFFIXES: frozenset[str] = CPP_SUFFIXES | PY_SUFFIXES

#: Directories excluded from the scan (template code is not production GUI).
EXCLUDED_DIR_NAMES: frozenset[str] = frozenset({"_TEMPLATE_"})

#: Workbenches whose GUI directory is test/infrastructure code rather than
#: production GUI. ``src/Mod/Test/Gui`` hosts the unit-test workbench, not a
#: shipped workbench surface.
EXCLUDED_WORKBENCHES: frozenset[str] = frozenset({"TemplatePyMod", "Test"})

#: Individual test-harness files excluded from the scan. These are compiled into
#: the GUI library (or importable) but are test infrastructure, not a shipped
#: GUI surface; the rationale mirrors ``EXCLUDED_WORKBENCHES``.
EXCLUDED_FILES: frozenset[str] = frozenset(
    {
        # GUI test-command harness (FCCmdTest*/CmdTestProgress*/BarThread) whose
        # QWaitCondition/QThreadPool waits simulate user activity for the test
        # framework rather than block a production GUI code path.
        "src/Gui/CommandTest.cpp",
        # GUI test support module (QtGui test helpers); not production GUI.
        "src/Gui/FreeCADGuiTest.py",
    }
)

#: Additional GUI directories for the module-aware workbenches whose GUI Python
#: is not organised under a directory literally named ``Gui``. Each entry is an
#: explicit, reviewed GUI-only sub-package: its contents are commands, task
#: panels, or view providers (never App-layer object/geometry code), so scanning
#: them does not pull App Python into a GUI inventory.
EXTRA_GUI_DIRS: tuple[str, ...] = (
    # Draft GUI commands (GuiCommand* tool classes).
    "src/Mod/Draft/draftguitools",
    # Draft Qt task panels (TaskPanel* dialogs).
    "src/Mod/Draft/drafttaskpanels",
    # Draft view providers (ViewProvider* presentation classes).
    "src/Mod/Draft/draftviewproviders",
    # BIM GUI commands (Bim* command classes).
    "src/Mod/BIM/bimcommands",
    # CAM's Python-only GUI helper package.
    "src/Mod/CAM/PathPythonGui",
    # CAM tool-library and tool-bit UI packages reached through loaded commands.
    "src/Mod/CAM/Path/Tool/library/ui",
    "src/Mod/CAM/Path/Tool/toolbit/ui",
    "src/Mod/CAM/Path/Tool/assets/ui",
    "src/Mod/CAM/Path/Tool/docobject/ui",
    "src/Mod/CAM/Path/Tool/shape/ui",
    # CAM machine/MTConnect task dialogs reached from machine commands.
    "src/Mod/CAM/Machine/ui",
    # FEM GUI packages listed in src/Mod/Fem/CMakeLists.txt.
    "src/Mod/Fem/femcommands",
    "src/Mod/Fem/femguiobjects",
    "src/Mod/Fem/femguiutils",
    "src/Mod/Fem/fempreferencepages",
    "src/Mod/Fem/femtaskpanels",
    "src/Mod/Fem/femviewprovider",
    # TechDraw's Python command and task-panel package loaded by InitGui.py.
    "src/Mod/TechDraw/TechDrawTools",
    # Points command package used by the Points GUI extension.
    "src/Mod/Points/pointscommands",
    # Optional PartDesign shaft-wizard task-dialog package.
    "src/Mod/PartDesign/WizardShaft",
)

#: Additional top-level GUI modules in module-aware workbenches. These files sit
#: beside App-layer modules in the shared top-level package, so they cannot be
#: selected by directory; each is listed explicitly and reviewed as GUI-only.
EXTRA_GUI_FILES: tuple[str, ...] = (
    # Draft's main GUI controller (toolbar, task panel dispatch).
    "src/Mod/Draft/DraftGui.py",
    # Draft GUI command definitions (GuiCommand classes).
    "src/Mod/Draft/DraftTools.py",
    # Draft DXF import dialog (Qt widget).
    "src/Mod/Draft/DxfImportDialog.py",
    # BIM covering task panel / view provider.
    "src/Mod/BIM/ArchCoveringGui.py",
    # Draft GUI utility layer (selection/view helpers, not Draft geometry).
    "src/Mod/Draft/draftutils/gui_utils.py",
    # Draft GUI startup, grid, and status-bar helpers loaded by InitGui.py.
    "src/Mod/Draft/draftutils/grid_observer.py",
    "src/Mod/Draft/draftutils/init_draft_statusbar.py",
    "src/Mod/Draft/draftutils/init_tools.py",
    # BIM native IFC presentation provider.
    "src/Mod/BIM/nativeifc/ifc_viewproviders.py",
    # BIM native IFC command, observer, and status-bar GUI modules.
    "src/Mod/BIM/nativeifc/ifc_commands.py",
    "src/Mod/BIM/nativeifc/ifc_observer.py",
    "src/Mod/BIM/nativeifc/ifc_status.py",
    # FEM GUI extraction/view helper.
    "src/Mod/Fem/femguiutils/extract_link_view.py",
    # Robot GUI movie export tool.
    "src/Mod/Robot/MovieTool.py",
    # Tux's optional GUI extensions loaded by its production InitGui.py.
    "src/Mod/Tux/NavigationIndicatorGui.py",
    "src/Mod/Tux/PersistentToolbarsGui.py",
    # Assembly's Python GUI commands and workbench helpers.
    "src/Mod/Assembly/AssemblyPreferences.py",
    "src/Mod/Assembly/UtilsAssembly.py",
    "src/Mod/Assembly/CommandCreateAssembly.py",
    "src/Mod/Assembly/CommandCreateBom.py",
    "src/Mod/Assembly/CommandCreateJoint.py",
    "src/Mod/Assembly/CommandCreateSimulation.py",
    "src/Mod/Assembly/CommandCreateSnapshot.py",
    "src/Mod/Assembly/CommandCreateView.py",
    "src/Mod/Assembly/CommandExportASMT.py",
    "src/Mod/Assembly/CommandInsertLink.py",
    "src/Mod/Assembly/CommandInsertNewPart.py",
    "src/Mod/Assembly/CommandReviewNote.py",
    "src/Mod/Assembly/CommandSolveAssembly.py",
    # Joint task and view-provider handlers imported by Assembly commands.
    "src/Mod/Assembly/JointObject.py",
    "src/Mod/Assembly/SoSwitchMarker.py",
    # BIM GUI helpers imported by InitGui.py when the optional modules exist.
    "src/Mod/BIM/BimSelect.py",
    "src/Mod/BIM/BimStatus.py",
    # BIM view-provider modules reached from command and native-IFC paths.
    "src/Mod/BIM/ArchAxis.py",
    "src/Mod/BIM/ArchAxisSystem.py",
    "src/Mod/BIM/Arch.py",
    "src/Mod/BIM/ArchBuilding.py",
    "src/Mod/BIM/ArchBuildingPart.py",
    "src/Mod/BIM/ArchComponent.py",
    "src/Mod/BIM/ArchCurtainWall.py",
    "src/Mod/BIM/ArchEquipment.py",
    "src/Mod/BIM/ArchFence.py",
    "src/Mod/BIM/ArchFloor.py",
    "src/Mod/BIM/ArchFrame.py",
    "src/Mod/BIM/ArchGrid.py",
    "src/Mod/BIM/ArchMaterial.py",
    "src/Mod/BIM/ArchPanel.py",
    "src/Mod/BIM/ArchPipe.py",
    "src/Mod/BIM/ArchPrecast.py",
    "src/Mod/BIM/ArchProfile.py",
    "src/Mod/BIM/ArchProject.py",
    "src/Mod/BIM/ArchReport.py",
    "src/Mod/BIM/ArchRebar.py",
    "src/Mod/BIM/ArchReference.py",
    "src/Mod/BIM/ArchRoof.py",
    "src/Mod/BIM/ArchSchedule.py",
    "src/Mod/BIM/ArchSectionPlane.py",
    "src/Mod/BIM/ArchSite.py",
    "src/Mod/BIM/ArchSpace.py",
    "src/Mod/BIM/ArchStairs.py",
    "src/Mod/BIM/ArchStructure.py",
    "src/Mod/BIM/ArchTruss.py",
    "src/Mod/BIM/ArchWall.py",
    "src/Mod/BIM/ArchWindow.py",
    "src/Mod/BIM/ArchWindowPresets.py",
    "src/Mod/BIM/ArchCutPlane.py",
    "src/Mod/BIM/ArchCovering.py",
    "src/Mod/BIM/ArchNesting.py",
    "src/Mod/BIM/ArchSql.py",
    "src/Mod/BIM/ArchVRM.py",
    "src/Mod/BIM/importers/exportIFC.py",
    "src/Mod/BIM/importers/importDAE.py",
    "src/Mod/BIM/importers/importIFC.py",
    "src/Mod/BIM/importers/importIFCHelper.py",
    "src/Mod/BIM/importers/importIFCmulticore.py",
    "src/Mod/BIM/nativeifc/ifc_import.py",
    "src/Mod/BIM/nativeifc/ifc_objects.py",
    "src/Mod/BIM/nativeifc/ifc_tools.py",
    "src/Mod/BIM/nativeifc/ifc_tree.py",
    "src/Mod/BIM/ArchCommands.py",
    # CAM GUI entry-point helpers loaded outside a directory named Gui.
    "src/Mod/CAM/PathCommands.py",
    "src/Mod/CAM/Path/GuiInit.py",
    "src/Mod/CAM/Path/Tool/assets/ui/preferences.py",
    "src/Mod/CAM/Path/Tool/toolbit/ui/cmd.py",
    "src/Mod/CAM/Path/Tool/library/ui/cmd.py",
    "src/Mod/CAM/Path/Tool/camassets.py",
    "src/Mod/CAM/Path/Tool/migration/migration.py",
    "src/Mod/CAM/Path/Preferences.py",
    "src/Mod/CAM/PathScripts/PathUtilsGui.py",
    "src/Mod/CAM/Path/Tool/Controller.py",
    "src/Mod/CAM/Path/Base/PropertyBag.py",
    "src/Mod/CAM/Path/Base/SetupSheet.py",
    "src/Mod/CAM/Path/Dressup/Array.py",
    "src/Mod/CAM/Path/Dressup/Boundary.py",
    "src/Mod/CAM/Path/Dressup/DogboneII.py",
    "src/Mod/CAM/Path/Dressup/Tags.py",
    "src/Mod/CAM/Path/Dressup/Utils.py",
    "src/Mod/CAM/Path/Main/Job.py",
    "src/Mod/CAM/Path/Main/Sanity/ImageBuilder.py",
    "src/Mod/CAM/Path/Main/Stock.py",
    "src/Mod/CAM/Path/Op/Adaptive.py",
    "src/Mod/CAM/Path/Op/Base.py",
    "src/Mod/CAM/Path/Op/Custom.py",
    "src/Mod/CAM/Path/Op/Deburr.py",
    "src/Mod/CAM/Path/Op/Drilling.py",
    "src/Mod/CAM/Path/Op/Engrave.py",
    "src/Mod/CAM/Path/Op/Flute.py",
    "src/Mod/CAM/Path/Op/Helix.py",
    "src/Mod/CAM/Path/Op/MillFace.py",
    "src/Mod/CAM/Path/Op/MillFacing.py",
    "src/Mod/CAM/Path/Op/PlanarSurface.py",
    "src/Mod/CAM/Path/Op/Pocket.py",
    "src/Mod/CAM/Path/Op/PocketShape.py",
    "src/Mod/CAM/Path/Op/Probe.py",
    "src/Mod/CAM/Path/Op/Profile.py",
    "src/Mod/CAM/Path/Op/RotarySurface.py",
    "src/Mod/CAM/Path/Op/Slot.py",
    "src/Mod/CAM/Path/Op/Surface.py",
    "src/Mod/CAM/Path/Op/Tapping.py",
    "src/Mod/CAM/Path/Op/ThreadMilling.py",
    "src/Mod/CAM/Path/Op/Vcarve.py",
    "src/Mod/CAM/Path/Op/Waterline.py",
    "src/Mod/CAM/Path/Post/Command.py",
    "src/Mod/CAM/Path/Post/Utils.py",
    "src/Mod/CAM/Path/Post/UtilsExport.py",
    "src/Mod/CAM/Path/Tool/shape/doc.py",
    "src/Mod/CAM/PathScripts/PathUtils.py",
    "src/Mod/Draft/draftfunctions/mirror.py",
    "src/Mod/Draft/draftfunctions/cut.py",
    "src/Mod/Draft/draftfunctions/downgrade.py",
    "src/Mod/Draft/draftfunctions/draftify.py",
    "src/Mod/Draft/draftfunctions/extrude.py",
    "src/Mod/Draft/draftfunctions/fuse.py",
    "src/Mod/Draft/draftfunctions/heal.py",
    "src/Mod/Draft/draftfunctions/offset.py",
    "src/Mod/Draft/draftfunctions/upgrade.py",
    "src/Mod/Draft/draftmake/make_clone.py",
    "src/Mod/Draft/draftmake/make_array.py",
    "src/Mod/Draft/draftmake/make_bezcurve.py",
    "src/Mod/Draft/draftmake/make_block.py",
    "src/Mod/Draft/draftmake/make_bspline.py",
    "src/Mod/Draft/draftmake/make_circle.py",
    "src/Mod/Draft/draftmake/make_circulararray.py",
    "src/Mod/Draft/draftmake/make_copy.py",
    "src/Mod/Draft/draftmake/make_dimension.py",
    "src/Mod/Draft/draftmake/make_ellipse.py",
    "src/Mod/Draft/draftmake/make_facebinder.py",
    "src/Mod/Draft/draftmake/make_fillet.py",
    "src/Mod/Draft/draftmake/make_hatch.py",
    "src/Mod/Draft/draftmake/make_label.py",
    "src/Mod/Draft/draftmake/make_layer.py",
    "src/Mod/Draft/draftmake/make_point.py",
    "src/Mod/Draft/draftmake/make_patharray.py",
    "src/Mod/Draft/draftmake/make_pointarray.py",
    "src/Mod/Draft/draftmake/make_polararray.py",
    "src/Mod/Draft/draftmake/make_polygon.py",
    "src/Mod/Draft/draftmake/make_rectangle.py",
    "src/Mod/Draft/draftmake/make_shape2dview.py",
    "src/Mod/Draft/draftmake/make_shapestring.py",
    "src/Mod/Draft/draftmake/make_sketch.py",
    "src/Mod/Draft/draftmake/make_text.py",
    "src/Mod/Draft/draftmake/make_wire.py",
    "src/Mod/Draft/draftmake/make_wpproxy.py",
    "src/Mod/Draft/draftutils/todo.py",
    "src/Mod/Draft/importSVG.py",
    "src/Mod/Fem/femmesh/gmshtools.py",
    "src/Mod/Fem/femmesh/netgentools.py",
    "src/Mod/Fem/femsolver/calculix/calculixtools.py",
    "src/Mod/Fem/femsolver/elmer/elmertools.py",
    "src/Mod/Fem/femsolver/elmer/equations/equation.py",
    "src/Mod/Fem/femsolver/run.py",
    "src/Mod/Fem/femsolver/task.py",
    "src/Mod/Fem/femsolver/z88/z88tools.py",
    "src/Mod/Fem/femtools/ccxtools.py",
    "src/Mod/Fem/femtools/objecttools.py",
    "src/Mod/Fem/feminout/importCcxFrdResults.py",
    "src/Mod/Fem/femresult/resulttools.py",
    "src/Mod/OpenSCAD/importCSG.py",
    "src/Mod/OpenSCAD/OpenSCAD2Dgeom.py",
    "src/Mod/OpenSCAD/replaceobj.py",
    "src/Mod/Part/CompoundTools/CompoundFilter.py",
    # Other production workbench GUI entry-point helpers.
    "src/Mod/Draft/WorkingPlane.py",
    "src/Mod/Help/Help.py",
    "src/Mod/OpenSCAD/OpenSCADCommands.py",
    "src/Mod/OpenSCAD/OpenSCADUtils.py",
    "src/Mod/Part/BasicShapes/CommandShapes.py",
    "src/Mod/Part/CompoundTools/_CommandCompoundFilter.py",
    "src/Mod/Part/CompoundTools/_CommandExplodeCompound.py",
    "src/Mod/PartDesign/InvoluteGearFeature.py",
    "src/Mod/PartDesign/SprocketFeature.py",
    # OpenSCAD's provider module is loaded by OpenSCADCommands.
    "src/Mod/OpenSCAD/OpenSCADFeatures.py",
    # BasicShapes' view-provider helpers are loaded by its command module.
    "src/Mod/Part/BasicShapes/ViewProviderShapes.py",
    "src/Mod/Sketcher/Profiles.py",
    "src/Mod/Start/StartMigrator.py",
)

# Direct local imports from production InitGui.py files that were reviewed and
# deliberately kept out of the GUI inventory. These are package initializers
# or App/model utility modules; GUI submodules imported from them are listed in
# EXTRA_GUI_DIRS/EXTRA_GUI_FILES above. Keeping this manifest explicit prevents
# a new local InitGui import from silently disappearing from the audit.
REVIEWED_INITGUI_IMPORT_EXCLUSIONS: dict[str, str] = {
    "src/Mod/BIM/nativeifc/__init__.py": "dependency-availability helper; GUI nativeifc modules are listed explicitly",
    "src/Mod/CAM/Path/__init__.py": "CAM App package; its loaded GUI subpackages are already in Gui scope",
    "src/Mod/CAM/PathScripts/__init__.py": "CAM App compatibility package; no GUI handlers",
    "src/Mod/Draft/draftutils/__init__.py": "utility package initializer; GUI utility modules are listed explicitly",
    "src/Mod/Draft/draftutils/params.py": "Draft parameter storage helper with no GUI handlers",
    "src/Mod/Draft/draftutils/utils.py": "Draft geometry/model utility with no GUI handlers",
    "src/Mod/CAM/Path/Tool/library/ui/__init__.py": "CAM UI package initializer; command module is listed explicitly",
    "src/Mod/CAM/Path/Tool/toolbit/ui/__init__.py": "CAM UI package initializer; command module is listed explicitly",
    "src/Mod/PartDesign/__init__.py": "PartDesign App package initializer; GUI modules are listed explicitly",
}

# Local modules reached transitively from GUI commands but intentionally kept
# out of the presentation inventory. Each exclusion is individually reviewed
# as a model/IO/solver helper; GUI-bearing modules are promoted into scope.
REVIEWED_TRANSITIVE_IMPORT_EXCLUSIONS: dict[str, str] = {
    "src/Mod/BIM/importers/exportIFCHelper.py": "BIM IFC helper; GUI caller is inventoried",
    "src/Mod/CAM/Path/Op/SurfaceSupport.py": "CAM geometry helper; GUI caller is inventoried",
    "src/Mod/CAM/Path/Tool/shape/models/base.py": "CAM shape model helper; GUI caller is inventoried",
    "src/Mod/CAM/Path/Tool/toolbit/util.py": "CAM tool model helper; GUI caller is inventoried",
    "src/Mod/Draft/draftfunctions/join.py": "Draft geometry constructor; no GUI handlers",
    "src/Mod/Draft/draftmake/make_arc_3points.py": "Draft model constructor; no GUI handlers",
    "src/Mod/Draft/draftmake/make_orthoarray.py": "Draft model constructor; no GUI handlers",
    "src/Mod/Draft/draftutils/groups.py": "Draft model grouping helper; no GUI handlers",
    "src/Mod/Fem/feminout/importCcxDatResults.py": "FEM result importer; GUI caller is inventoried",
    "src/Mod/Fem/femsolver/mystran/tasks.py": "FEM solver task helper; GUI caller is inventoried",
    "src/Mod/PartDesign/fcgear/fcgear.py": "PartDesign model helper; GUI caller is inventoried",
    "src/Mod/PartDesign/fcsprocket/fcsprocket.py": "PartDesign model helper; GUI caller is inventoried",
}

# Dynamic imports whose runtime value is a persisted/plugin-selected module
# name. The closure validator expands these deterministic repository-local
# patterns instead of attempting to execute application code.
REVIEWED_DYNAMIC_IMPORT_TARGETS: dict[str, tuple[str, ...]] = {
    "src/Mod/BIM/Arch.py": ("src/Mod/BIM/Arch*.py",),
    "src/Mod/CAM/Path/Op/Gui/Base.py": ("src/Mod/CAM/Path/Op/Gui/*.py",),
}

# Dynamic imports whose targets are deliberately runtime/plugin supplied rather
# than repository-local modules. They are validated as reviewed external
# boundaries instead of being guessed from arbitrary user/configuration data.
REVIEWED_DYNAMIC_IMPORT_EXTERNAL_SOURCES: dict[str, str] = {
    "src/Gui/FreeCADGuiInit.py": "extension workbench name supplied by the installed module registry",
    "src/Mod/BIM/bimcommands/BimPreflight.py": "user Preflight directory or IFC metadata supplies module names",
    "src/Mod/BIM/importers/importIFCHelper.py": "IFC property metadata supplies optional App/GUI module names",
}

# Dynamic callbacks whose candidate package is already conservatively scoped.
REVIEWED_DYNAMIC_IMPORT_SCOPED_PACKAGES: dict[str, str] = {
    "src/Mod/CAM/Path/Base/Gui/IconViewProvider.py": "callback.__module__ resolves within the scoped Path Base GUI package",
    "src/Mod/Draft/draftutils/gui_utils.py": "draftviewproviders package is an explicit GUI scope entry",
}


@dataclass(frozen=True)
class Category:
    """A single search rule: name, prose rationale, patterns, and disposition.

    ``cpp_pattern``/``py_pattern`` are compiled against the *masked* source of
    the matching language; ``None`` marks a category that has no expression in
    that language.
    """

    key: str
    title: str
    description: str
    cpp_pattern: str | None
    py_pattern: str | None
    default_disposition: str


# Keep categories in a stable, documented order.
CATEGORIES: tuple[Category, ...] = (
    Category(
        key="thread-waits",
        title="Thread/process blocking waits",
        description=(
            "Synchronous waits for a worker thread, helper process, condition "
            "variable, or future to finish while the GUI thread is blocked. "
            "C++: QProcess/QFuture/QThreadPool waitForFinished, waitForDone, "
            "waitForStarted, waitForBytesWritten, and QLocalSocket waitForConnected; "
            "QThread::wait; pthread_join; "
            "and the instance member wait (thread->wait() and QWaitCondition().wait). "
            "Python: the same QProcess/QThread waitFor* family and instance .wait() "
            "(e.g. subprocess/QThread.wait), plus AST-classified join() calls on "
            "thread/task/process-like receivers. String joins (QString::join, str.join, "
            "os.path.join) and std::thread::detach are explicitly not matches."
        ),
        cpp_pattern=(
            r"\b(?:waitForFinished|waitForDone|waitForStarted|waitForBytesWritten|waitForConnected)[^\S\n]*\(|"
            r"QThread[^\S\n]*::[^\S\n]*wait[^\S\n]*\(|"
            r"pthread_join[^\S\n]*\(|"
            r"(?:->|\.)wait[^\S\n]*\("
        ),
        py_pattern=(
            r"\.(?:waitForFinished|waitForDone|waitForStarted|waitForBytesWritten|waitForConnected)[^\S\n]*\(|"
            r"\.wait[^\S\n]*\("
        ),
        default_disposition="investigate",
    ),
    Category(
        key="blocking-invokes",
        title="Cross-thread blocking invokes",
        description=(
            "Queued cross-thread invocation that blocks the calling (GUI) thread "
            "until the receiver executes. Detected by the Qt::BlockingQueuedConnection "
            "connection type (C++), or the PySide BlockingQueuedConnection enum "
            "(Python), which is the only portable way FreeCAD requests a blocking "
            "queued dispatch. No Python GUI site currently uses it; the rule is "
            "recorded so any future use is inventoried."
        ),
        cpp_pattern=r"Qt[^\S\n]*::[^\S\n]*BlockingQueuedConnection",
        py_pattern=r"\bBlockingQueuedConnection\b",
        default_disposition="migrate",
    ),
    Category(
        key="process-events-polling",
        title="Manual event-loop polling",
        description=(
            "Explicit processEvents() pumping of the Qt event loop, or the Python "
            "FreeCADGui/Gui.updateGui() wrapper that performs the same re-entrant "
            "flush. These sites re-enter the event loop from inside a handler and "
            "are the classic source of GUI-thread reentrancy and long-duration busy "
            "loops. Masked comments and literals are not matches."
        ),
        cpp_pattern=r"\bprocessEvents[^\S\n]*\(",
        py_pattern=(
            r"\bprocessEvents[^\S\n]*\(|" r"(?:FreeCADGui|Gui)[^\S\n]*\.[^\S\n]*updateGui[^\S\n]*\("
        ),
        default_disposition="investigate",
    ),
    Category(
        key="direct-recompute",
        title="Direct synchronous document recompute",
        description=(
            "Direct calls to Document/DocumentObject::recompute from GUI code "
            "(C++ ``.recompute(``/``->recompute(``, Python ``.recompute(``). "
            "These run the model recompute synchronously on the GUI thread and "
            "must move onto the per-document execution lane."
        ),
        cpp_pattern=r"(?:\.|->)recompute[^\S\n]*\(",
        py_pattern=r"\.recompute[^\S\n]*\(",
        default_disposition="migrate",
    ),
    Category(
        key="live-app-dereference",
        title="Live App object dereference",
        description=(
            "Reaching into global application state for a live App::Document or "
            "App::DocumentObject handle and dereferencing it directly, bypassing "
            "the committed presentation state. C++: App::GetApplication() document/"
            "object accessors (getActiveDocument, getDocument, getDocuments, "
            "getDocumentOrActive, getDocumentByPath), including the multi-line "
            "receiver-chain form. Python: App/FreeCAD.ActiveDocument (attribute), "
            "App/FreeCAD.activeDocument() (method), and App/FreeCAD.getDocument(). "
            "Gui.ActiveDocument (the GUI-side document handle) is tracked "
            "separately, as is the model-layer src/App core."
        ),
        cpp_pattern=(
            r"App::GetApplication\s*\(\s*\)\s*\.\s*"
            r"(?:getActiveDocument|getDocuments|getDocumentOrActive|getDocumentByPath|getDocument)"
            r"[^\S\n]*\("
        ),
        py_pattern=(
            r"(?:App|FreeCAD)[^\S\n]*\.[^\S\n]*(?:\+[^\S\n]*)?"
            r"(?:ActiveDocument\b|activeDocument[^\S\n]*\(|getDocument[^\S\n]*\()"
        ),
        default_disposition="investigate",
    ),
    Category(
        key="live-reference-callback",
        title="Live-reference lifecycle callbacks",
        description=(
            "Subscriptions and handlers for the state-change signals that deliver "
            "live object references into GUI code. C++: the document-object "
            "signal/slot identifiers Changed, Change, Touched, Deleted, Delete, "
            "BeforeChange, Recomputed (plus the ChangedView/DeletedView view "
            "variants). Python: addObserver/removeObserver subscriptions (Selection "
            "and Document observers) that register callbacks receiving live "
            "references. Creation/rename/activate lifecycle signals and tree/"
            "selection widget navigation signals are out of scope and tracked "
            "separately. Pure signal member declarations are excluded individually "
            "in exclusions.json."
        ),
        cpp_pattern=(
            r"\b(?:signal|slot)"
            r"(?:Changed|Change|Touched|Deleted|Delete|BeforeChange|Recomputed)"
            r"(?:View)?Object\b"
        ),
        py_pattern=r"\b(?:addObserver|removeObserver)[^\S\n]*\(",
        default_disposition="migrate",
    ),
    Category(
        key="update-data-provider",
        title="ViewProvider updateData providers",
        description=(
            "ViewProvider updateData(const App::Property*) declarations and "
            "overrides -- both the qualified definitions and the inline "
            "declarations/definitions (``void updateData(const App::Property*) "
            "override``). These are the presentation providers that read live "
            "model properties and must be migrated to bounded presentation "
            "adapters that consume committed state. Anchoring on the concrete "
            "signature means base-class delegation calls (``X::updateData(prop)``) "
            "and the unrelated QAbstractItemModel-style PropertyItem::updateData() "
            "are not matched. Python uses the scanner's provider-aware AST classifier "
            "for methods named updateData on ViewProvider classes or reviewed provider "
            "modules; its regex pattern is intentionally None so generic Qt model "
            "callbacks remain excluded."
        ),
        cpp_pattern=r"\bupdateData[^\S\n]*\([^\S\n]*const[^\S\n]+App::Property",
        py_pattern=None,
        default_disposition="migrate",
    ),
)

#: Ordered migration dispositions and their meaning (see module docstring).
DISPOSITIONS: tuple[str, ...] = ("migrate", "investigate", "accepted")

CATEGORY_BY_KEY: dict[str, Category] = {category.key: category for category in CATEGORIES}
