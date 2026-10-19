# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import Any, List, Tuple, TypeAlias

from Base.Metadata import export
from App.DocumentObject import DocumentObject
from Gui.ViewProvider import ViewProvider

SoTransformDragger: TypeAlias = Any

@export(Include="Mod/Assembly/Gui/ViewProviderAssembly.h", Namespace="AssemblyGui")
class ViewProviderAssembly(ViewProvider):
    """
    This is the ViewProviderAssembly class

    Author: Ondsel (development@ondsel.com)
    License: LGPL-2.1-or-later
    """

    def isInEditMode(self) -> Any:
        """
        Return true if the assembly object is currently in edit mode.
        """
        ...

    def getDragger(self) -> SoTransformDragger:
        """Return the assembly dragger coin object."""
        ...

    def isolateComponents(
        self,
        components: List[DocumentObject] | Tuple[DocumentObject, ...],
        mode: int,
        selection: bool = False,
        /,
    ) -> None:
        """
        Temporarily isolates a given set of components in the 3D view.
        Other components are faded or hidden based on the specified mode.

        Args:
            components (List[DocumentObject] | Tuple[DocumentObject, ...]):
                A list or tuple of DocumentObjects to isolate.
            mode (int): An integer specifying the isolation mode:
                - 0: Transparent
                - 1: Wireframe
                - 2: Hidden
            selection (bool): Use scene-only user isolation. Selected objects retain
                their prior state. Joint isolation and transactions cannot clear it;
                call clearIsolate(True) to exit. Defaults to legacy joint isolation.
        """
        ...

    def clearIsolate(self, selection: bool = False, /) -> None:
        """
        Restores component state. Pass True to also clear user isolation.
        The default clears only legacy joint isolation, preserving user isolation.
        """
        ...
    EnableMovement: bool
    """Enable moving the parts by clicking and dragging."""

    MoveOnlyPreselected: bool
    """If enabled, only the preselected object will move."""

    MoveInCommand: bool
    """If enabled, each move will be wrapped in a command."""

    DraggerVisibility: bool
    """Show or hide the assembly dragger."""

    DraggerPlacement: Any
    """Placement of the assembly dragger object."""
