# SPDX-License-Identifier: LGPL-2.1-or-later
"""Assembly GUI tests that need the GUI and a real model tree: extract-component
regressions and the joint editor."""

from AssemblyTests.TestExtractComponent import TestExtractComponentGui
from AssemblyTests.TestJointEditorSwitch import TestJointEditorSwitch

__all__ = ["TestExtractComponentGui", "TestJointEditorSwitch"]
