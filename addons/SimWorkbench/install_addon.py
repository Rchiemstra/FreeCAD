"""
install_addon.py — Copy SimWorkbench to FreeCAD's Mod directory.

Run from the repo root:
    python addons/SimWorkbench/install_addon.py

Requires FreeCAD v1.x to be installed. The script locates the Mod directory
automatically from APPDATA (Windows) or ~/.local/share (Linux/macOS).
"""
from __future__ import annotations

import os
import sys
import shutil
import platform
from pathlib import Path


def find_freecad_mod_dir() -> Path:
    system = platform.system()
    if system == "Windows":
        appdata = os.environ.get("APPDATA", "")
        candidates = [
            Path(appdata) / "FreeCAD" / "v1-2" / "Mod",
            Path(appdata) / "FreeCAD" / "Mod",
        ]
    else:
        home = Path.home()
        candidates = [
            home / ".local" / "share" / "FreeCAD" / "Mod",
            home / ".FreeCAD" / "Mod",
        ]
    for c in candidates:
        if c.parent.exists():
            c.mkdir(parents=True, exist_ok=True)
            return c
    raise RuntimeError(
        "Could not find FreeCAD Mod directory. "
        "Please install manually: copy addons/SimWorkbench/ to "
        "%APPDATA%\\FreeCAD\\v1-2\\Mod\\SimWorkbench\\"
    )


def install() -> None:
    src = Path(__file__).parent.resolve()
    repo_root = src.parent.parent.resolve()
    mod_dir = find_freecad_mod_dir()
    dst = mod_dir / "SimWorkbench"

    if dst.exists():
        print(f"Removing existing {dst} …")
        shutil.rmtree(dst)

    print(f"Copying {src} → {dst} …")
    shutil.copytree(src, dst)
    (dst / "repo_root.txt").write_text(str(repo_root), encoding="utf-8")
    print("Done. Restart FreeCAD and switch to 'Simulation Workbench'.")


if __name__ == "__main__":
    install()
