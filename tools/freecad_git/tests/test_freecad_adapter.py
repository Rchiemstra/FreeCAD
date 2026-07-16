"""FreeCAD adapter tests."""

from __future__ import annotations

import pytest

from freecad_git.freecad_adapter import (
    GitSidecarSaveObserver,
    _is_eligible_target,
    suppress_git_sidecar,
)


class TestFreecadAdapter:
    def test_eligible_fcstd(self):
        assert _is_eligible_target("/path/to/Model.FCStd")
        assert _is_eligible_target("C:\\Models\\part.fcstd")

    def test_ineligible_backup(self):
        assert not _is_eligible_target("/path/to/Model.FCStd1")
        assert not _is_eligible_target("/path/to/Model.bak")

    def test_ineligible_recovery(self):
        assert not _is_eligible_target("/tmp/fc_recovery_files/doc.FCStd")

    def test_suppress_context(self):
        from freecad_git.freecad_adapter import _is_suppressed, _suppress_depth

        assert not _is_suppressed()
        with suppress_git_sidecar():
            assert _is_suppressed()
        assert not _is_suppressed()

    def test_observer_skips_when_no_preference(self):
        observer = GitSidecarSaveObserver()
        # Should not raise when FreeCAD not available
        observer.slotFinishSaveDocument(None, "/tmp/test.FCStd")
