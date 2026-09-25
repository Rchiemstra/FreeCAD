# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from Base.Metadata import export
from Base.PyObjectBase import PyObjectBase


@export(Constructor=False, Delete=True)
class DocumentCommandHandle(PyObjectBase):
    """Non-blocking observation handle for one admitted document command."""

    def id(self) -> int:
        """Return the pointer-free document command identifier."""
        ...

    def status(self) -> dict:
        """Return a read-only command snapshot without advancing execution."""
        ...

    def progress(self) -> float:
        """Return command progress in the inclusive [0, 1] range."""
        ...

    def done(self) -> bool:
        """Report whether the admitted command reached a terminal state."""
        ...

    def cancel(self, reason: str = "command cancelled by caller", /) -> bool:
        """Request cooperative cancellation of the admitted command."""
        ...
