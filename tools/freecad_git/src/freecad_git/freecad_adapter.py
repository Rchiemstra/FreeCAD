"""Opt-in FreeCAD post-save adapter for Git sidecar generation."""

from __future__ import annotations

import subprocess
import sys
import threading
from pathlib import Path

# Module-level re-entrancy guard
_active_exports: set[str] = set()
_lock = threading.Lock()

# Snapshot/recovery suppression context
_suppress_depth = 0
_suppress_lock = threading.Lock()


class suppress_git_sidecar:
    """Context manager to suppress sidecar generation during internal saves."""

    def __enter__(self):
        global _suppress_depth
        with _suppress_lock:
            _suppress_depth += 1
        return self

    def __exit__(self, *args):
        global _suppress_depth
        with _suppress_lock:
            _suppress_depth -= 1


def _is_suppressed() -> bool:
    return _suppress_depth > 0


def _is_eligible_target(filename: str) -> bool:
    """Check if save target should generate a sidecar."""
    path = Path(filename)
    name_lower = path.name.lower()

    if not name_lower.endswith(".fcstd"):
        return False

    # Exclude backup/recovery/temporary patterns
    exclude_patterns = (
        ".fcstd1",
        ".fcstd2",
        ".bak",
        ".tmp",
        ".recovery",
        "~",
    )
    for pattern in exclude_patterns:
        if pattern in name_lower:
            return False

    # Exclude recovery directories
    parts = {p.lower() for p in path.parts}
    if parts & {"fc_recovery_files", "recovery", "autosave", "snapshots", "snapshot"}:
        return False

    return True


def _invoke_exporter(filename: str) -> None:
    """Invoke the standalone freecad-git exporter."""
    canonical = str(Path(filename).resolve())
    with _lock:
        if canonical in _active_exports:
            return
        _active_exports.add(canonical)

    try:
        result = subprocess.run(
            [sys.executable, "-m", "freecad_git.cli", "export", filename],
            capture_output=True,
            text=True,
            timeout=300,
        )
        if result.returncode != 0:
            _report_warning(filename, result.stderr or result.stdout or "export failed")
    except Exception as exc:
        _report_warning(filename, str(exc))
    finally:
        with _lock:
            _active_exports.discard(canonical)


def _report_warning(filename: str, message: str) -> None:
    """Report non-fatal sidecar warning."""
    try:
        import FreeCAD

        FreeCAD.Console.PrintWarning(
            f"Git sidecar generation failed for {filename}: {message}\n"
        )
    except ImportError:
        print(f"WARNING: Git sidecar generation failed for {filename}: {message}", file=sys.stderr)


def export_sidecar_from_fcstd(source_path: str, output_path: str | None = None) -> None:
    """Public API to export sidecar from finalized FCStd path."""
    _invoke_exporter(source_path)


class GitSidecarSaveObserver:
    """FreeCAD document observer for post-save sidecar generation."""

    def slotFinishSaveDocument(self, document, filename):
        try:
            import FreeCAD

            param = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/GitSidecar")
            if not param.GetBool("GenerateGitSidecarAfterSave", False):
                return
        except (ImportError, AttributeError):
            return

        if _is_suppressed():
            return

        if not _is_eligible_target(filename):
            return

        _invoke_exporter(filename)


def register():
    """Register the save observer with FreeCAD."""
    try:
        import FreeCAD

        if not hasattr(FreeCAD, "_git_sidecar_observer"):
            observer = GitSidecarSaveObserver()
            FreeCAD.addDocumentObserver(observer)
            FreeCAD._git_sidecar_observer = observer
    except ImportError:
        pass
