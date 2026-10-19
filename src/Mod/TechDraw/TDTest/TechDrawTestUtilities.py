# SPDX-License-Identifier: LGPL-2.1-or-later

import FreeCAD
import os
import time


def waitForHLR(view):
    """Pump Qt events for up to 60 seconds until a new view has HLR geometry."""
    from PySide import QtCore

    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        QtCore.QCoreApplication.processEvents(QtCore.QEventLoop.AllEvents, 50)
        # The HLR busy flags are not exposed to Python. For a new view, edges
        # become available when onHlrFinished installs the projected geometry.
        if "Up-to-date" in view.State and view.getVisibleEdges():
            return
        time.sleep(0.01)
    raise AssertionError(
        f"Timed out after 60 s waiting for HLR of {view.Name}: "
        f"State={view.State}, visible edges={len(view.getVisibleEdges())}"
    )


def createPageWithSVGTemplate(doc=None):
    """Returns a page with an SVGTemplate added on the ActiveDocument"""
    path = os.path.dirname(os.path.abspath(__file__))
    templateFileSpec = path + "/TestTemplate.svg"

    if not doc:
        doc = FreeCAD.ActiveDocument

    page = doc.addObject("TechDraw::DrawPage", "Page")
    doc.addObject("TechDraw::DrawSVGTemplate", "Template")
    doc.Template.Template = templateFileSpec
    doc.Page.Template = doc.Template
    return page
