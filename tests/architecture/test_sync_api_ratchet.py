# SPDX-License-Identifier: LGPL-2.1-or-later
"""Ratchet test: first-party GUI must not gain sync document API calls.

The allowlist is keyed by call site, not by file. A site is the repository
path plus the whitespace-normalised source evidence of the call. Identical
calls in one file share a site and are counted. Line numbers are not part of
the identity, so a moved call does not fail and a new call does, including a
new call inside a file that already has an allowlisted site.

The test fails when a discovered site is missing, when its count is higher
than the allowlist, or when an allowlist entry is stale (missing, or a lower
count than recorded). Coin3D, property, and feature ``touch()``, painter and
image saves, feature-level ``obj.recompute()``, ``Gui::Document`` save/undo/redo,
and the async ingress implementation are excluded by the scanner and must not
appear here.

Exclusions (never scanned):
 - ``src/App/**`` — the compatibility implementation of the sync API itself.
 - ``src/Mod/Test/**`` — test-infrastructure workbench; not production GUI.
 - Python binding sources ``DocumentPy.cpp`` and ``DocumentPy.xml``.
"""

from __future__ import annotations

import os
import re
import sys
import unittest
from pathlib import Path

_ARCH_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_ARCH_DIR))

from gui_blocking_live_model import rules, scanner  # noqa: E402

REPOSITORY_ROOT = Path(os.environ.get("FREECAD_SOURCE_ROOT", _ARCH_DIR.parents[1])).resolve()

# ---------------------------------------------------------------------------
# Allowlist
# ---------------------------------------------------------------------------
# Key: ``relative/path||normalised evidence``.
# Value: ``(count, reason)``. ``count`` is how many times that exact call
# text occurs in the file. Drive the list to empty by migrating sites.
# ---------------------------------------------------------------------------

SYNC_API_SITES: dict[str, tuple[int, str]] = {
    "src/Gui/DocumentRecovery.cpp||App::GetApplication().closeDocument(docs[i]->getName());": (
        1,
        "intentional sync close in crash-recovery code; no document-lock or completion callback available",
    ),
    "src/Mod/Assembly/CommandInsertNewPart.py||doc.recompute()": (
        1,
        "doc.recompute() after linking a new part into the assembly; the assembly reads the result immediately",
    ),
    "src/Mod/BIM/ArchCommands.py||document.recompute()": (
        1,
        "document.recompute() in BIM utility helpers; another agent owns all BIM source",
    ),
    "src/Mod/BIM/ArchCommands.py||return document.recompute(objects)": (
        1,
        "document.recompute() in BIM utility helpers; another agent owns all BIM source",
    ),
    "src/Mod/BIM/ArchSectionPlane.py||FreeCAD.ActiveDocument.recompute()": (
        2,
        "FreeCAD.ActiveDocument.recompute() in ArchSectionPlane; protected file (another agent owns)",
    ),
    "src/Mod/BIM/bimcommands/BimLibrary.py||FreeCAD.ActiveDocument.save()": (
        1,
        "FreeCAD.ActiveDocument.save() writes a library preview document; async save path requires refactor",
    ),
    "src/Mod/BIM/bimcommands/BimLibrary.py||FreeCAD.ActiveDocument.saveAs(FCfilename)": (
        1,
        "FreeCAD.ActiveDocument.saveAs() stores a library part; async save path requires refactor",
    ),
    "src/Mod/BIM/bimcommands/BimLibrary.py||FreeCAD.closeDocument(self.previewDocName)": (
        3,
        "FreeCAD.closeDocument() on a temporary preview document; async close path requires refactor",
    ),
    "src/Mod/BIM/bimcommands/BimProjectManager.py||FreeCAD.closeDocument(tname)": (
        1,
        "FreeCAD.closeDocument() on a temporary template document; async close path requires refactor",
    ),
    "src/Mod/BIM/importers/exportIFC.py||FreeCAD.ActiveDocument.recompute()": (
        1,
        "FreeCAD.ActiveDocument.recompute() in IFC export; another agent owns BIM/importers",
    ),
    "src/Mod/BIM/importers/importIFC.py||doc.recompute()": (
        12,
        "doc.recompute() in IFC import; another agent owns BIM/importers",
    ),
    "src/Mod/BIM/importers/importIFCmulticore.py||FreeCAD.ActiveDocument.recompute()": (
        1,
        "FreeCAD.ActiveDocument.recompute() in IFC multicore import; another agent owns BIM/importers",
    ),
    "src/Mod/BIM/nativeifc/ifc_commands.py||doc.recompute()": (
        1,
        "doc.recompute() in IFC GUI commands; another agent owns BIM/nativeifc",
    ),
    "src/Mod/BIM/nativeifc/ifc_commands.py||document.recompute()": (
        1,
        "document.recompute() in IFC GUI commands; another agent owns BIM/nativeifc",
    ),
    "src/Mod/BIM/nativeifc/ifc_import.py||document.recompute()": (
        1,
        "document.recompute() in IFC import handler; another agent owns BIM/nativeifc",
    ),
    "src/Mod/BIM/nativeifc/ifc_observer.py||doc.recompute()": (
        1,
        "doc.recompute() after converting an object to IFC; another agent owns BIM/nativeifc",
    ),
    "src/Mod/BIM/nativeifc/ifc_status.py||doc.recompute()": (
        4,
        "doc.recompute() in IFC status update; another agent owns BIM/nativeifc",
    ),
    "src/Mod/BIM/nativeifc/ifc_tools.py||doc.recompute()": (
        1,
        "doc.recompute() in IFC tools; another agent owns BIM/nativeifc",
    ),
    "src/Mod/BIM/nativeifc/ifc_tree.py||obj.Document.recompute()": (
        1,
        "obj.Document.recompute() in IFC tree; another agent owns BIM/nativeifc",
    ),
    "src/Mod/BIM/nativeifc/ifc_viewproviders.py||obj.Document.recompute()": (
        1,
        "obj.Document.recompute() in IFC view providers; another agent owns BIM/nativeifc",
    ),
    "src/Mod/BIM/nativeifc/ifc_viewproviders.py||self.Object.Document.recompute()": (
        10,
        "obj.Document.recompute() in IFC view providers; another agent owns BIM/nativeifc",
    ),
    "src/Mod/BIM/nativeifc/ifc_viewproviders.py||vobj.Object.Document.recompute()": (
        1,
        "obj.Document.recompute() in IFC view providers; another agent owns BIM/nativeifc",
    ),
    "src/Mod/CAM/Path/Main/Job.py||obj.Document.recompute() # necessary to create the clone shape": (
        1,
        "obj.Document.recompute() required for clone shape creation; result read immediately after",
    ),
    "src/Mod/CAM/Path/Op/Base.py||obj.Document.recompute()": (
        1,
        "obj.Document.recompute() after replacing a CAM base-link property; the restored operation reads the recomputed shape",
    ),
    "src/Mod/CAM/Path/Tool/shape/doc.py||FreeCAD.closeDocument(self._doc.Name)": (
        1,
        "FreeCAD.closeDocument() on a transient shape-doc helper; no completion callback path available",
    ),
    "src/Mod/CAM/Path/Tool/shape/util.py||FreeCAD.closeDocument(doc.Name)": (
        1,
        "FreeCAD.closeDocument() closes a temporary thumbnail document; no completion callback path available",
    ),
    "src/Mod/Draft/DraftGui.py||FreeCAD.ActiveDocument.recompute()": (
        3,
        "FreeCAD.ActiveDocument.recompute() after Draft face-list edits; the panel reads the recomputed shape",
    ),
    "src/Mod/Draft/draftfunctions/svg.py||App.closeDocument(hidden_doc.Name)": (
        2,
        "App.closeDocument() closes a hidden SVG parse document; async close path requires refactor",
    ),
    "src/Mod/Draft/draftviewproviders/view_layer.py||doc.recompute()": (
        1,
        "doc.recompute() before commitTransaction (ordering constraint)",
    ),
    "src/Mod/Draft/draftviewproviders/view_wire.py||doc.recompute()": (
        1,
        "doc.recompute() before doc.commitTransaction(); result must be committed in same transaction",
    ),
    "src/Mod/Draft/importSVG.py||FreeCAD.closeDocument(hidden_doc.Name)": (
        1,
        "FreeCAD.closeDocument() closes a hidden parse document; async close path requires refactor",
    ),
    "src/Mod/Fem/feminout/importCcxFrdResults.py||doc.recompute()": (
        1,
        "doc.recompute() after FEM FRD result import; the GUI reads the imported result immediately",
    ),
    "src/Mod/Fem/femsolver/elmer/equations/equation.py||doc.Document.recompute()": (
        1,
        "doc.Document.recompute() in equation setup; equation parameters read from result immediately",
    ),
    "src/Mod/Fem/femsolver/run.py||obj.Document.recompute()": (
        1,
        "obj.Document.recompute() after FEM solve; callers read result state immediately; async breaks workflow",
    ),
    "src/Mod/Fem/femtest/Gui/test_open.py||FreeCAD.closeDocument(self.document.Name)": (
        2,
        "FreeCAD.closeDocument() in FEM test teardown; test infrastructure, not production code",
    ),
    "src/Mod/Fem/femtest/Gui/test_open.py||FreeCAD.closeDocument(self.document.Name) # close the empty document from setUp first": (
        1,
        "FreeCAD.closeDocument() in FEM test teardown; test infrastructure, not production code",
    ),
    "src/Mod/Fem/femtest/Gui/test_open.py||self.document.saveAs(file_path)": (
        1,
        "self.document.saveAs() in FEM open/save test; test infrastructure, not production code",
    ),
    "src/Mod/Fem/femtools/ccxtools.py||self.analysis.Document.recompute()": (
        1,
        "self.analysis.Document.recompute() after CalculiX solve; result extraction reads shapes after",
    ),
    "src/Mod/OpenSCAD/importCSG.py||FreeCAD.ActiveDocument.recompute()": (
        1,
        "doc.recompute() after CSG geometry import; import/export module; caller reads geometry immediately",
    ),
    "src/Mod/OpenSCAD/importCSG.py||FreeCAD.closeDocument(docSVG.Name)": (
        1,
        "FreeCAD.closeDocument() closes a temporary SVG parse document during CSG import; async close path requires refactor",
    ),
    "src/Mod/OpenSCAD/importCSG.py||newobj.Document.recompute()": (
        1,
        "doc.recompute() after CSG geometry import; import/export module; caller reads geometry immediately",
    ),
    "src/Mod/OpenSCAD/replaceobj.py||parent.Document.recompute()": (
        1,
        "parent.Document.recompute() after replacing a child object; the caller reads the recomputed parent immediately",
    ),
    "src/Mod/Part/BasicShapes/ViewProviderShapes.py||document.recompute()": (
        2,
        "document.recompute() after a shape-task transaction; the dialog finishes on the recomputed shape",
    ),
}

_BINDING_EXCLUSIONS: frozenset[str] = frozenset({"src/App", "src/Mod/Test"})
_SYNC_API_CATEGORY = rules.CATEGORY_BY_KEY.get("sync-document-api")


def _normalize_evidence(evidence: str) -> str:
    return re.sub(r"\s+", " ", evidence).strip()


def site_key(path: str, evidence: str) -> str:
    """Stable identity of one sync-document call, independent of line number."""
    return f"{path.replace(chr(92), '/')}||{_normalize_evidence(evidence)}"


def _byte_sort_key(value: str) -> bytes:
    return value.encode("utf-8")


def site_differences(
    discovered: dict[str, int],
    allowlist: dict[str, tuple[int, str]],
) -> tuple[list[str], list[str]]:
    """Return ``(regressions, stale)`` for a site count map.

    A regression is a new site, or a higher count of an existing site, including
    inside a file that already has an allowlisted call. A stale entry is an
    allowlisted site that is gone or whose count dropped.
    """
    regressions: list[str] = []
    stale: list[str] = []
    for key in sorted(discovered, key=_byte_sort_key):
        count = discovered[key]
        allowed = allowlist.get(key)
        if allowed is None:
            regressions.append(f"{key}  (found {count}, not allowlisted)")
        elif count > allowed[0]:
            regressions.append(f"{key}  (found {count}, allowlisted {allowed[0]})")
    for key in sorted(allowlist, key=_byte_sort_key):
        count, reason = allowlist[key]
        found = discovered.get(key, 0)
        if found < count:
            stale.append(f"{key}  (allowlisted {count}, found {found}; was: {reason})")
    return regressions, stale


def _is_excluded_path(rel_path: str) -> bool:
    for excl in _BINDING_EXCLUSIONS:
        if rel_path.startswith(excl + "/") or rel_path.startswith(excl + "\\"):
            return True
    basename = rel_path.replace("\\", "/").split("/")[-1]
    return basename in {"DocumentPy.cpp", "DocumentPy.xml"}


def _collect_sync_api_sites() -> dict[str, int]:
    if _SYNC_API_CATEGORY is None:
        return {}
    counts: dict[str, int] = {}
    for source_path in scanner.iter_source_files(REPOSITORY_ROOT):
        rel_path = source_path.relative_to(REPOSITORY_ROOT).as_posix()
        if _is_excluded_path(rel_path):
            continue
        try:
            source = source_path.read_text(encoding="utf-8", errors="surrogateescape")
        except OSError:
            continue
        for finding in scanner.scan_source(source, source_path.suffix, rel_path):
            if finding.category != "sync-document-api":
                continue
            key = site_key(finding.path, finding.evidence)
            counts[key] = counts.get(key, 0) + 1
    return counts


def _sync_findings(source: str, suffix: str, path: str) -> list[scanner.Finding]:
    return [
        finding
        for finding in scanner.scan_source(source, suffix, path)
        if finding.category == "sync-document-api"
    ]


class SyncApiSiteIdentityTest(unittest.TestCase):
    """The ratchet compares sites, so a second call in a listed file fails."""

    def test_new_call_inside_allowlisted_file_is_a_regression(self) -> None:
        allowlist = {"src/Mod/Example.py||doc.recompute()": (1, "existing debt")}
        discovered = {
            "src/Mod/Example.py||doc.recompute()": 1,
            "src/Mod/Example.py||doc.save()": 1,
        }
        regressions, stale = site_differences(discovered, allowlist)
        self.assertEqual(stale, [])
        self.assertEqual(len(regressions), 1)
        self.assertIn("doc.save()", regressions[0])

    def test_extra_identical_call_inside_allowlisted_file_is_a_regression(self) -> None:
        allowlist = {"src/Mod/Example.py||doc.recompute()": (1, "existing debt")}
        discovered = {"src/Mod/Example.py||doc.recompute()": 2}
        regressions, stale = site_differences(discovered, allowlist)
        self.assertEqual(stale, [])
        self.assertEqual(len(regressions), 1)
        self.assertIn("found 2, allowlisted 1", regressions[0])

    def test_removed_or_reduced_site_is_stale(self) -> None:
        allowlist = {
            "src/Mod/Example.py||doc.recompute()": (2, "existing debt"),
            "src/Mod/Example.py||doc.save()": (1, "existing debt"),
        }
        discovered = {"src/Mod/Example.py||doc.recompute()": 1}
        regressions, stale = site_differences(discovered, allowlist)
        self.assertEqual(len(regressions), 0)
        self.assertEqual(len(stale), 2)

    def test_equal_counts_pass(self) -> None:
        allowlist = {"src/Mod/Example.py||doc.recompute()": (2, "existing debt")}
        discovered = {"src/Mod/Example.py||doc.recompute()": 2}
        self.assertEqual(site_differences(discovered, allowlist), ([], []))

    def test_line_number_is_not_part_of_the_identity(self) -> None:
        self.assertEqual(
            site_key("src/Mod/Example.py", "  doc.recompute()  "),
            site_key("src/Mod/Example.py", "doc.recompute()"),
        )


class SyncApiFilterTest(unittest.TestCase):
    """Scanner exclusions for shapes that are not App::Document sync calls."""

    def _assert_count(self, source: str, suffix: str, path: str, expected: int) -> None:
        findings = _sync_findings(source, suffix, path)
        self.assertEqual(
            len(findings),
            expected,
            [finding.evidence for finding in findings],
        )

    def test_false_positive_shapes_are_not_findings(self) -> None:
        cases = [
            ("void f() { painter->save(); }\n", ".cpp", "src/Gui/Snippet.cpp"),
            ("void f() { image.save(path); }\n", ".cpp", "src/Gui/Snippet.cpp"),
            ("def f(obj):\n    obj.recompute()\n", ".py", "src/Mod/BIM/Arch.py"),
            ("void f() { camera->touch(); }\n", ".cpp", "src/Gui/Snippet.cpp"),
            (
                "def f():\n    Gui.ActiveDocument.saveAs('x')\n",
                ".py",
                "src/Mod/Assembly/CommandInsertLink.py",
            ),
            (
                "def f(selectedPart):\n    selectedPart.ViewObject.Document.saveAs()\n",
                ".py",
                "src/Mod/Assembly/CommandInsertLink.py",
            ),
            (
                "void f() { doc->commitCompatibilityMutationAsync(); }\n",
                ".cpp",
                "src/Gui/Snippet.cpp",
            ),
            ("void f() { textEdit->document()->undo(); }\n", ".cpp", "src/Gui/Snippet.cpp"),
            (
                "void f() {\n    Gui::Document* doc = getGuiDocument();\n    doc->saveAs(name);\n}\n",
                ".cpp",
                "src/Gui/Snippet.cpp",
            ),
        ]
        for source, suffix, path in cases:
            with self.subTest(source=source):
                self._assert_count(source, suffix, path, 0)

    def test_real_document_calls_are_findings(self) -> None:
        cases = [
            (
                "def f():\n    FreeCAD.ActiveDocument.recompute()\n",
                ".py",
                "src/Mod/BIM/ArchSectionPlane.py",
            ),
            (
                "def f(obj):\n    obj.Document.recompute()\n",
                ".py",
                "src/Mod/Fem/femsolver/run.py",
            ),
            (
                "def f():\n    FreeCAD.closeDocument(name)\n",
                ".py",
                "src/Mod/BIM/bimcommands/BimLibrary.py",
            ),
            (
                "void f() { doc->commitCompatibilityMutation(); }\n",
                ".cpp",
                "src/Gui/Snippet.cpp",
            ),
            (
                "void f() {\n    App::Document* doc = getDocument();\n    doc->recompute();\n}\n",
                ".cpp",
                "src/Gui/Snippet.cpp",
            ),
        ]
        for source, suffix, path in cases:
            with self.subTest(source=source):
                self._assert_count(source, suffix, path, 1)

    def test_ingress_implementation_is_excluded_and_other_calls_remain(self) -> None:
        source = (
            "void Document::save() {\n"
            "    App::Document* doc = getDocument();\n"
            "    doc->save();\n"
            "}\n"
            "void other() {\n"
            "    App::Document* doc = getDocument();\n"
            "    doc->recompute();\n"
            "}\n"
            "void Document::executeCompatibilityMutation() {\n"
            "    return adapter.execute(\n"
            "        std::move(declaration),\n"
            "        [this](const Declaration& admitted) {\n"
            "            doc->commitCompatibilityMutation();\n"
            "        });\n"
            "}\n"
        )
        findings = _sync_findings(source, ".cpp", "src/Gui/Document.cpp")
        self.assertEqual([finding.evidence for finding in findings], ["doc->recompute();"])


class SyncApiRatchetTest(unittest.TestCase):
    """Discovered sites must match the allowlist exactly."""

    @classmethod
    def setUpClass(cls) -> None:
        if _SYNC_API_CATEGORY is None:
            raise AssertionError("sync-document-api category not found in rules.py")
        cls.discovered = _collect_sync_api_sites()

    def test_no_new_sync_api_sites(self) -> None:
        regressions, _stale = site_differences(self.discovered, SYNC_API_SITES)
        if regressions:
            self.fail(
                "New sync document API sites (a new call fails even inside an "
                "already-listed file):\n"
                + "\n".join(f"  {line}" for line in regressions)
                + "\n\nMigrate the call to requestDocumentRecompute / "
                "submitDocumentKindCommand / submitDocumentClose / submitDocumentSave, "
                "or add the site to SYNC_API_SITES with its count and a one-line reason."
            )

    def test_no_stale_allowlist_entries(self) -> None:
        _regressions, stale = site_differences(self.discovered, SYNC_API_SITES)
        if stale:
            self.fail(
                "Stale sync-document API allowlist entries. Remove the site or "
                "lower its count in SYNC_API_SITES:\n"
                + "\n".join(f"  {line}" for line in stale)
            )


if __name__ == "__main__":
    unittest.main()
