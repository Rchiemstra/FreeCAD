# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from Base.Metadata import export
from Base.PyObjectBase import PyObjectBase


@export(Constructor=False, Delete=True)
class RecomputeHandle(PyObjectBase):
    """Observation and cancellation handle returned by Document.recomputeAsync()."""

    def id(self) -> int:
        """Return the pointer-free document recompute identifier."""
        ...

    def status(self) -> dict:
        """Advance ready work once and return a copy of the current status."""
        ...

    def progress(self) -> float:
        """Advance ready work once and return progress in the inclusive [0, 1] range."""
        ...

    def done(self) -> bool:
        """Advance ready work once and report whether the recompute is terminal."""
        ...

    def cancel(self, reason: str = "recompute cancelled by caller", /) -> bool:
        """Request cooperative cancellation, escalating through the process backend if needed."""
        ...

    def wait(self, timeout: float = 360.0, /) -> dict:
        """Responsively wait up to timeout seconds and return the current status."""
        ...
