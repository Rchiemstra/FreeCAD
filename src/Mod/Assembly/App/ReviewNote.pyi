# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from Base.Metadata import export, constmethod

from App.DocumentObject import DocumentObject

@export(
    Include="Mod/Assembly/App/ReviewNote.h",
    Namespace="Assembly",
    FatherInclude="App/DocumentObjectPy.h",
    FatherNamespace="App",
)
class ReviewNote(DocumentObject):
    """
    Assembly review note anchored to a component or joint.

    Author: The FreeCAD Project Association AISBL
    License: LGPL-2.1-or-later
    """

    def refreshBasePosition(self) -> None:
        """Recompute BasePosition from Target and LocalAnchor."""
        ...

    @constmethod
    def isAttachmentBroken(self) -> bool:
        """True when the target is missing or invalid."""
        ...

    @constmethod
    def getOwnerPart(self) -> object:
        """Return the owning App::Part (or AssemblyObject) for this note."""
        ...

    @constmethod
    def getAssembly(self) -> object:
        """Return the owning AssemblyObject when the owner is an assembly."""
        ...

    @constmethod
    def getGroup(self) -> object:
        """Return the ReviewNoteGroup containing this note."""
        ...
