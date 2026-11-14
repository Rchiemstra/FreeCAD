# SPDX-License-Identifier: LGPL-2.1-or-later

"""Assembly GUI integration tests that need the GUI and a real model tree,
collected by the GUI integration runner."""

import sys
import FreeCAD as App

from AssemblyTests.TestExtractComponent import TestExtractComponentGui
from AssemblyTests.TestJointEditorSwitch import TestJointEditorSwitch
from AssemblyTests.TestMotionInterferenceGui import TestMotionInterferenceGui, TestDragFeedbackGui

# Report-view redirection hides the command-line summary from CI. This entry
# point is also importable in the interactive test workbench.
if App.ConfigGet("TestCase").startswith("TestAssemblyGui"):
    sys.stdout = sys.__stdout__

__all__ = [
    "TestDragFeedbackGui",
    "TestExtractComponentGui",
    "TestJointEditorSwitch",
    "TestMotionInterferenceGui",
]
