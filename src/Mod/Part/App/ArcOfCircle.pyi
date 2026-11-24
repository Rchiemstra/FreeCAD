# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from Base.Metadata import export, typing_only
from ArcOfConic import ArcOfConic
from typing import Final, overload
from Base.Vector import Vector

@export(
    PythonName="Part.ArcOfCircle",
    Twin="GeomArcOfCircle",
    TwinPointer="GeomArcOfCircle",
    Include="Mod/Part/App/Geometry.h",
    FatherInclude="Mod/Part/App/ArcOfConicPy.h",
    Constructor=True,
)
class ArcOfCircle(ArcOfConic):
    """
    Author: Werner Mayer (wmayer[at]users.sourceforge.net)
    Licence: LGPL
    Describes a portion of a circle
    """

    Radius: float = ...
    """The radius of the circle."""

    Circle: Final[object] = ...
    """The internal circle representation"""

    @typing_only
    @overload
    def __init__(self, circle: "Part.Circle", u1: float, u2: float, sense: bool = ..., /) -> None: ...

    @typing_only
    @overload
    def __init__(self, p1: Vector, p2: Vector, p3: Vector, /) -> None: ...
