# SPDX-License-Identifier: LGPL-2.1-or-later
# /****************************************************************************
#                                                                           *
#    Copyright (c) 2026 The FreeCAD project                                *
#                                                                           *
#    This file is part of FreeCAD.                                          *
#                                                                           *
#    FreeCAD is free software: you can redistribute it and/or modify it     *
#    under the terms of the GNU Lesser General Public License as            *
#    published by the Free Software Foundation, either version 2.1 of the   *
#    License, or (at your option) any later version.                        *
#                                                                           *
#    FreeCAD is distributed in the hope that it will be useful, but         *
#    WITHOUT ANY WARRANTY; without even the implied warranty of             *
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
#    Lesser General Public License for more details.                        *
#                                                                           *
#    You should have received a copy of the GNU Lesser General Public       *
#    License along with FreeCAD. If not, see                                *
#    <https://www.gnu.org/licenses/>.                                       *
#                                                                           *
# ***************************************************************************/

"""Regression tests for Assembly overlay icons and Origin-in-Group handling.

Covers:
- ViewProviderJoint.getOverlayIcons when getAssembly() returns None
- getGroundedParts (via isPartConnected) when Origin is wrongly in Group
- claimChildren Origin dedupe when Origin is also in Group (Gui)
"""

import unittest

import FreeCAD as App
import JointObject


def _msg(text, end="\n"):
    App.Console.PrintMessage(text + end)


class TestOverlayIconsAndOrigin(unittest.TestCase):
    def setUp(self):
        doc_name = self.__class__.__name__
        if App.ActiveDocument:
            if App.ActiveDocument.Name != doc_name:
                App.newDocument(doc_name)
        else:
            App.newDocument(doc_name)
        App.setActiveDocument(doc_name)
        self.doc = App.ActiveDocument

        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.jointgroup = self.assembly.newObject("Assembly::JointGroup", "Joints")

        _msg("  Temporary document '{}'".format(self.doc.Name))

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def _force_origin_into_group(self):
        origin = self.assembly.Origin
        self.assertIsNotNone(origin, "Assembly must have an Origin")
        group = list(self.assembly.Group)
        if origin not in group:
            self.assembly.Group = group + [origin]
        self.assertIn(origin, self.assembly.Group)
        return origin

    def test_getOverlayIcons_no_assembly(self):
        """getOverlayIcons must not AttributeError when getAssembly() returns None."""
        operation = "getOverlayIcons with no assembly"
        _msg("  Test '{}'".format(operation))

        # Joint.__init__ requires an assembly parent; create normally then stub getAssembly.
        joint = self.jointgroup.newObject("App::FeaturePython", "OrphanJoint")
        JointObject.Joint(joint, 0)

        original_get_assembly = joint.Proxy.getAssembly
        joint.Proxy.getAssembly = lambda _joint: None
        try:
            self.assertIsNone(joint.Proxy.getAssembly(joint))

            vp = JointObject.ViewProviderJoint.__new__(JointObject.ViewProviderJoint)
            vp.app_obj = joint

            overlays = vp.getOverlayIcons()
            self.assertEqual(overlays, {}, "'{}' failed".format(operation))
        finally:
            joint.Proxy.getAssembly = original_get_assembly

    def test_isPartConnected_with_origin_in_group(self):
        """Origin in Group must not throw when walking grounded parts."""
        operation = "isPartConnected with Origin in Group"
        _msg("  Test '{}'".format(operation))

        box = self.assembly.newObject("Part::Box", "Box")
        self._force_origin_into_group()

        # Must not raise ExtensionContainer::getExtension / TypeError
        connected = self.assembly.isPartConnected(box)
        self.assertIsInstance(connected, bool, "'{}' failed".format(operation))

    @unittest.skipIf(not App.GuiUp, "GUI tests require FreeCAD GUI mode")
    def test_claimChildren_origin_not_duplicated(self):
        """claimChildren must claim Origin only once when Origin is also in Group."""
        operation = "claimChildren Origin dedupe"
        _msg("  Test '{}'".format(operation))

        origin = self._force_origin_into_group()
        children = self.assembly.ViewObject.claimChildren()

        self.assertEqual(
            children.count(origin),
            1,
            "'{}' failed: Origin claimed {} times".format(operation, children.count(origin)),
        )
        self.assertIs(
            children[0],
            origin,
            "'{}' failed: Origin should be first claimed child".format(operation),
        )
