# SPDX-License-Identifier: LGPL-2.1-or-later
from SketcherTests.TestConstraintPreselectionGui import SketcherGuiTestCases
from SketcherTests.TestDistanceLabelExtensionGui import TestDistanceLabelExtensionGui
from SketcherTests.TestConstraintCommandsGui import TestConstraintCommandsGui
from SketcherTests.TestCoincidentCommandGui import TestCoincidentCommandGui
from SketcherTests.TestOnViewParameterGui import TestOnViewParameterGui
from SketcherTests.TestPlacementUpdate import TestSketchPlacementUpdate
from SketcherTests.TestExternalFacePreselection import TestExternalFacePreselection
from SketcherTests.TestSketcherOffsetGui import TestSketcherOffsetGui
from SketcherTests.TestVisualLayerListGui import TestVisualLayerListGui
from SketcherTests.TestSketchExpressionDiagnosticsGui import TestSketchExpressionDiagnosticsGui, TestExpressionClipboardWorkflow
from SketcherTests.TestViewSectionGui import TestViewSectionGui

from SketcherTests.TestSketchReferenceRepairGui import TestSketchReferenceRepairGui
from SketcherTests.TestSketchEditCleanGui import TestSketchEditCleanGui

# Use the module so that code checkers don't complain (flake8)
(
    True
    if SketcherGuiTestCases
    and TestDistanceLabelExtensionGui
    and TestConstraintCommandsGui
    and TestCoincidentCommandGui
    and TestSketchPlacementUpdate
    and TestOnViewParameterGui
    and TestExternalFacePreselection
    and TestSketcherOffsetGui
    and TestVisualLayerListGui
    and TestSketchExpressionDiagnosticsGui
    and TestExpressionClipboardWorkflow
    and TestViewSectionGui
    and TestSketchReferenceRepairGui
    and TestSketchEditCleanGui
    else False
)
