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
        """Return a read-only recompute snapshot without advancing execution."""
        ...

    def progress(self) -> float:
        """Return recompute progress in the inclusive [0, 1] range."""
        ...

    def done(self) -> bool:
        """Report whether the recompute reached a terminal state."""
        ...

    def poll(self) -> bool:
        """Report whether the recompute reached a terminal state without advancing execution."""
        ...

    def cancel(self, reason: str = "recompute cancelled by caller", /) -> bool:
        """Request cooperative cancellation, escalating through the process backend if needed."""
        ...

    def wait(self, timeout: float = 360.0, /) -> dict:
        """Wait up to timeout seconds off the GUI thread and return the current status."""
        ...
