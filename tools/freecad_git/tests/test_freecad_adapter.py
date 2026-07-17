"""FreeCAD adapter tests."""

from __future__ import annotations

import subprocess
import sys
from unittest.mock import MagicMock, patch

import pytest

from freecad_git.freecad_adapter import (
    GitSidecarSaveObserver,
    _invoke_exporter,
    _is_eligible_target,
    _python_executable,
    suppress_git_sidecar,
)


class TestFreecadAdapter:
    def test_python_executable_uses_prefix_python(self, tmp_path, monkeypatch):
        if sys.platform == "win32":
            python = tmp_path / "python.exe"
        else:
            python = tmp_path / "bin" / "python"
            python.parent.mkdir()
        python.touch()
        monkeypatch.setattr("freecad_git.freecad_adapter.sys.prefix", str(tmp_path))

        assert _python_executable() == str(python)

    @patch("freecad_git.freecad_adapter.subprocess.run")
    def test_exporter_hides_windows_console(self, mock_run, tmp_path):
        mock_run.return_value = MagicMock(returncode=0, stdout="", stderr="")

        _invoke_exporter(str(tmp_path / "Model.FCStd"))

        expected_flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
        assert mock_run.call_args.kwargs["creationflags"] == expected_flags

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
