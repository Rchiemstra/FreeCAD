# SPDX-License-Identifier: LGPL-2.1-or-later
"""Keep reference repair on the audited nonblocking GUI execution path."""

import unittest
from pathlib import Path

from gui_blocking_live_model import rules, scanner

ROOT = Path(__file__).resolve().parents[2]
HELPER = "src/Mod/Sketcher/SketchReferenceRepair.py"
GUI = "src/Mod/Sketcher/SketchReferenceRepairGui.py"
PICK = "src/Mod/Sketcher/SketchReferenceReassignGui.py"


class SketchReferenceRepairArchitecture(unittest.TestCase):
    def test_helper_is_audited(self):
        self.assertIn(HELPER, rules.EXTRA_GUI_FILES)
        self.assertNotIn(HELPER, rules.REVIEWED_TRANSITIVE_IMPORT_EXCLUSIONS)
        self.assertIn(PICK, rules.EXTRA_GUI_FILES)

    def test_no_direct_recompute(self):
        for name in (HELPER, GUI, PICK):
            source = (ROOT / name).read_text()
            findings = scanner.scan_source(source, ".py", name)
            self.assertFalse([f for f in findings if f.category == "direct-recompute"])

    def test_async_mutation_and_admitted_completion(self):
        source = (ROOT / GUI).read_text()
        self.assertIn("commitCompatibilityMutationAsync", source)
        self.assertIn("tryRunPresentationCallback", source)
        self.assertNotIn("processEvents(", source)
