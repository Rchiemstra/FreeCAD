# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from Base.Metadata import export

from App.DocumentObjectGroup import DocumentObjectGroup

@export(Include="Mod/Assembly/App/Groups.h", Namespace="Assembly")
class ReviewNoteGroup(DocumentObjectGroup):
    """
    This class is a group subclass for assembly review notes.

    Author: The FreeCAD Project Association AISBL
    License: LGPL-2.1-or-later
    """
