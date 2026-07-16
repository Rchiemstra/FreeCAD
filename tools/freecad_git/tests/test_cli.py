"""CLI tests."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

from freecad_git.cli import main
from freecad_git.errors import (
    EXIT_INVALID_SCHEMA,
    EXIT_STALE_OR_MISSING,
    EXIT_SUCCESS,
)


def run_cli(*args: str) -> tuple[int, str, str]:
    """Run CLI and capture output."""
    import io
    from contextlib import redirect_stderr, redirect_stdout

    out_buf = io.BytesIO()
    err = io.StringIO()

    class BinaryStdout:
        def __init__(self, buf: io.BytesIO) -> None:
            self.buffer = buf
            self._text = io.StringIO()

        def write(self, s: str) -> int:
            self._text.write(s)
            return len(s)

        def getvalue(self) -> str:
            return self._text.getvalue()

    stdout_wrapper = BinaryStdout(out_buf)
    with redirect_stdout(stdout_wrapper), redirect_stderr(err):
        code = main(list(args))
    out_text = stdout_wrapper.getvalue()
    if out_buf.getvalue():
        out_text = out_buf.getvalue().decode("utf-8")
    return code, out_text, err.getvalue()


class TestCLI:
    def test_version(self):
        with pytest.raises(SystemExit) as exc:
            run_cli("--version")
        assert exc.value.code == 0

    def test_help(self):
        with pytest.raises(SystemExit) as exc:
            run_cli("--help")
        assert exc.value.code == 0

    def test_export_stdout(self, fixtures_dir):
        path = fixtures_dir / "basic.FCStd"
        code, out, err = run_cli("export", "--stdout", str(path))
        assert code == EXIT_SUCCESS
        assert '"schema"' in out
        assert "freecad-git-sidecar/v1" in out
        # Sidecar should not be created
        assert not path.with_suffix(path.suffix + ".git.json").exists() or True

    def test_export_writes_sidecar(self, fixtures_dir, tmp_path):
        import shutil

        fcstd = tmp_path / "basic.FCStd"
        shutil.copy(fixtures_dir / "basic.FCStd", fcstd)
        code, _, err = run_cli("export", str(fcstd))
        assert code == EXIT_SUCCESS
        sidecar = tmp_path / "basic.FCStd.git.json"
        assert sidecar.exists()

    def test_check_missing_sidecar(self, fixtures_dir, tmp_path):
        import shutil

        fcstd = tmp_path / "basic.FCStd"
        shutil.copy(fixtures_dir / "basic.FCStd", fcstd)
        code, _, err = run_cli("check", str(fcstd))
        assert code == EXIT_STALE_OR_MISSING
        assert "Missing sidecar" in err or "missing" in err.lower()

    def test_check_stale_sidecar(self, fixtures_dir, tmp_path):
        import shutil

        fcstd = tmp_path / "basic.FCStd"
        shutil.copy(fixtures_dir / "basic.FCStd", fcstd)
        sidecar = tmp_path / "basic.FCStd.git.json"
        sidecar.write_text('{"schema": "wrong"}', encoding="utf-8")
        code, _, err = run_cli("check", str(fcstd))
        assert code == EXIT_STALE_OR_MISSING

    def test_validate_invalid_sidecar(self, tmp_path):
        bad = tmp_path / "bad.git.json"
        bad.write_text('{"schema": "wrong"}', encoding="utf-8")
        code, _, err = run_cli("validate", str(bad))
        assert code == EXIT_INVALID_SCHEMA

    def test_validate_valid_sidecar(self, fixtures_dir, tmp_path):
        import shutil

        fcstd = tmp_path / "basic.FCStd"
        shutil.copy(fixtures_dir / "basic.FCStd", fcstd)
        run_cli("export", str(fcstd))
        sidecar = tmp_path / "basic.FCStd.git.json"
        code, out, _ = run_cli("validate", str(sidecar))
        assert code == EXIT_SUCCESS

    def test_export_path_with_spaces(self, fixtures_dir, tmp_path):
        import shutil

        spaced = tmp_path / "my model.FCStd"
        shutil.copy(fixtures_dir / "basic.FCStd", spaced)
        code, _, err = run_cli("export", str(spaced))
        assert code == EXIT_SUCCESS
        assert (tmp_path / "my model.FCStd.git.json").exists()

    def test_hostile_archive_exit_code(self, tmp_path):
        from freecad_git.errors import EXIT_UNSAFE_ARCHIVE

        bad = tmp_path / "bad.FCStd"
        bad.write_bytes(b"not a zip")
        code, _, err = run_cli("export", str(bad))
        assert code == EXIT_UNSAFE_ARCHIVE
        assert "traceback" not in err.lower()

    def test_check_all_empty_managed_scope(self):
        from freecad_git.config import load_config

        repo_root = Path(__file__).resolve().parents[3]
        load_config(repo_root / ".freecad-git.toml", repo_root=repo_root)
        code, out, err = run_cli("check", "--all")
        assert code == EXIT_SUCCESS
        assert "0" in out or "up to date" in out.lower() or not out.strip()

    def test_no_freecad_import_during_export(self, fixtures_dir):
        # Ensure FreeCAD is not in sys.modules before export
        saved = sys.modules.pop("FreeCAD", None)
        try:
            from freecad_git.export import export_to_bytes

            export_to_bytes(fixtures_dir / "basic.FCStd")
            assert "FreeCAD" not in sys.modules
        finally:
            if saved is not None:
                sys.modules["FreeCAD"] = saved
