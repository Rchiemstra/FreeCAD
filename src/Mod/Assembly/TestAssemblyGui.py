# SPDX-License-Identifier: LGPL-2.1-or-later

"""Assembly GUI integration tests, collected by the GUI integration runner."""

import sys
import FreeCAD as App

from AssemblyTests.TestJointEditorSwitch import TestJointEditorSwitch
from AssemblyTests.TestMotionInterferenceGui import TestMotionInterferenceGui, TestDragFeedbackGui

# Report-view redirection hides the command-line summary from CI. This entry
# point is also importable in the interactive test workbench.
if App.ConfigGet("TestCase").startswith("TestAssemblyGui"):
    sys.stdout = sys.__stdout__

True if TestJointEditorSwitch and TestMotionInterferenceGui and TestDragFeedbackGui else False
