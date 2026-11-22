# SPDX-License-Identifier: LGPL-2.1-or-later

"""Assembly GUI integration tests that need the GUI and a real model tree,
collected by the GUI integration runner."""

import sys
import FreeCAD as App

from AssemblyTests.TestComponentPattern import TestComponentPatternGui
from AssemblyTests.TestExtractComponent import TestExtractComponentGui
from AssemblyTests.TestCommandCreateView import TestCommandCreateView
from AssemblyTests.TestCommandInsertLink import TestCommandInsertLink
from AssemblyTests.TestCore import TestCoreGui
from AssemblyTests.TestOverlayIconsAndOrigin import TestOverlayIconsAndOriginGui
from AssemblyTests.TestPreferencesImport import TestPreferencesImportGui
from AssemblyTests.TestReviewNotes import TestReviewNotesVisibilityGui
from AssemblyTests.TestSimulationExport import TestSimulationExport
from AssemblyTests.TestJointEditorSwitch import TestJointEditorSwitch
from AssemblyTests.TestMotionInterferenceGui import TestMotionInterferenceGui, TestDragFeedbackGui
from AssemblyTests.TestReviewNotes import TestReviewNotesGui
from AssemblyTests.TestReviewNotesIssues import TestReviewNotesGuiIssues

# Report-view redirection hides the command-line summary from CI. This entry
# point is also importable in the interactive test workbench.
if App.ConfigGet("TestCase").startswith("TestAssemblyGui"):
    sys.stdout = sys.__stdout__

__all__ = [
    "TestComponentPatternGui",
    "TestDragFeedbackGui",
    "TestExtractComponentGui",
    "TestCommandCreateView",
    "TestCommandInsertLink",
    "TestCoreGui",
    "TestOverlayIconsAndOriginGui",
    "TestPreferencesImportGui",
    "TestReviewNotesVisibilityGui",
    "TestSimulationExport",
    "TestJointEditorSwitch",
    "TestMotionInterferenceGui",
    "TestReviewNotesGui",
    "TestReviewNotesGuiIssues",
]
