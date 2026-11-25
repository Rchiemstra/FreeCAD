# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from Base.Metadata import export, typing_only
from Part.ArcOfConic import ArcOfConic
from typing import Final

@export(
    Father="ArcOfConicPy",
    Name="ArcOfHyperbolaPy",
    PythonName="Part.ArcOfHyperbola",
    Twin="GeomArcOfHyperbola",
    TwinPointer="GeomArcOfHyperbola",
    Include="Mod/Part/App/Geometry.h",
    Namespace="Part",
    FatherInclude="Mod/Part/App/ArcOfConicPy.h",
    FatherNamespace="Part",
    Constructor=True,
)
class ArcOfHyperbola(ArcOfConic):
    """
    Describes a portion of an hyperbola
    """

    MajorRadius: float = 0.0
    """The major radius of the hyperbola."""

    MinorRadius: float = 0.0
    """The minor radius of the hyperbola."""

    Hyperbola: Final[object] = None
    """The internal hyperbola representation"""

    @typing_only
    def __init__(self, hyperbola: "Part.Hyperbola", u1: float, u2: float, sense: bool = ..., /) -> None: ...
