# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: 2026 FreeCAD contributors
# SPDX-FileNotice: Part of the FreeCAD project.

################################################################################
#                                                                              #
#   FreeCAD is free software: you can redistribute it and/or modify            #
#   it under the terms of the GNU Lesser General Public License as             #
#   published by the Free Software Foundation, either version 2.1              #
#   of the License, or (at your option) any later version.                     #
#                                                                              #
#   FreeCAD is distributed in the hope that it will be useful,                 #
#   but WITHOUT ANY WARRANTY; without even the implied warranty                #
#   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                    #
#   See the GNU Lesser General Public License for more details.                #
#                                                                              #
#   You should have received a copy of the GNU Lesser General Public           #
#   License along with FreeCAD. If not, see https://www.gnu.org/licenses       #
#                                                                              #
################################################################################

import Arch
import Draft
import FreeCAD
from Test.GuiRecompute import recompute_document, close_document

from bimtests.TestArchBaseGui import TestArchBaseGui


class TestArchStairsGui(TestArchBaseGui):

    def _assert_visibility(self, stairs, expected):
        self.assertEqual(stairs.ViewObject.Visibility, expected)
        for railing in (stairs.RailingLeft, stairs.RailingRight):
            self.assertIsNotNone(railing)
            self.assertEqual(railing.ViewObject.Visibility, expected)

    def test_stairs_railings_follow_parent_visibility(self):
        stairs = Arch.makeStairs(length=3500, width=800, height=2500, steps=14)
        recompute_document(self.document)

        self.assertIsNotNone(stairs.RailingLeft)
        self.assertIsNotNone(stairs.RailingRight)
        self._assert_visibility(stairs, True)

        stairs.ViewObject.Visibility = False
        self.pump_gui_events()
        recompute_document(self.document)
        self._assert_visibility(stairs, False)

        stairs.ViewObject.Visibility = True
        self.pump_gui_events()
        recompute_document(self.document)
        self._assert_visibility(stairs, True)

        level = Arch.makeBuildingPart()
        level.Group = [stairs]
        recompute_document(self.document)

        level.ViewObject.Visibility = False
        self.pump_gui_events()
        recompute_document(self.document)
        self._assert_visibility(stairs, False)

        level.ViewObject.Visibility = True
        self.pump_gui_events()
        recompute_document(self.document)
        self._assert_visibility(stairs, True)

    def test_stairs_multi_segment_railing_follow_parent_visibility(self):
        wire1 = Draft.make_wire([FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(1000, 0, 0)])
        wire2 = Draft.make_wire([FreeCAD.Vector(1000, 0, 0), FreeCAD.Vector(1000, 1000, 0)])
        stairs = Arch.makeStairs(baseobj=[wire1, wire2], width=800, height=2500, steps=14)
        recompute_document(self.document)

        Arch.makeRailing([stairs] + stairs.Additions)
        recompute_document(self.document)

        segment = stairs.Additions[-1]
        segment.RailingLeft.ViewObject.Visibility = True
        segment.RailingRight.ViewObject.Visibility = True

        stairs.ViewObject.Visibility = False
        self.pump_gui_events()
        recompute_document(self.document)

        self.assertFalse(segment.RailingLeft.ViewObject.Visibility)
        self.assertFalse(segment.RailingRight.ViewObject.Visibility)

        stairs.ViewObject.Visibility = True
        self.pump_gui_events()
        recompute_document(self.document)

        self.assertTrue(segment.RailingLeft.ViewObject.Visibility)
        self.assertTrue(segment.RailingRight.ViewObject.Visibility)
