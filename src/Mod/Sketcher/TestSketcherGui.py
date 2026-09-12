# SPDX-License-Identifier: LGPL-2.1-or-later
from SketcherTests.TestConstraintPreselectionGui import SketcherGuiTestCases
from SketcherTests.TestDistanceLabelExtensionGui import TestDistanceLabelExtensionGui
from SketcherTests.TestConstraintCommandsGui import TestConstraintCommandsGui
from SketcherTests.TestOnViewParameterGui import TestOnViewParameterGui
from SketcherTests.TestPlacementUpdate import TestSketchPlacementUpdate
from SketcherTests.TestExternalFacePreselection import TestExternalFacePreselection
from SketcherTests.TestSketcherOffsetGui import TestSketcherOffsetGui

# Use the module so that code checkers don't complain (flake8)
(
    True
    if SketcherGuiTestCases
    and TestDistanceLabelExtensionGui
    and TestConstraintCommandsGui
    and TestSketchPlacementUpdate
    and TestOnViewParameterGui
    and TestExternalFacePreselection
    and TestSketcherOffsetGui
    else False
)
