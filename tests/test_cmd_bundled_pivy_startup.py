# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check FreeCADCmd startup after building only FreeCADMainCmd in a fresh tree.

Run through CTest, or build that target and invoke this script with the executable
and build-tree Mod/pivy directory as arguments. FreeCAD can report Python startup
errors while still exiting successfully, so require an explicit success marker.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable, expected = map(lambda value: str(Path(value).resolve()), sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="freecad-cmd-startup-") as directory:
        root = Path(directory)
        macro = root / "startup.py"
        marker = root / "initialized"
        macro.write_text(
            "import FreeCAD as App\n"
            "from pathlib import Path\n"
            "import pivy, pivy.coin, pivy._coin\n"
            "import os\n"
            "assert App.ConfigGet('PIVY_SOURCE') == 'bundled'\n"
            "assert hasattr(App, '__ModCache__')\n"
            "expected = Path(os.environ['FC_EXPECTED_PIVY']).resolve()\n"
            "assert Path(pivy.__file__).resolve().parent == expected\n"
            "assert Path(pivy.coin.__file__).resolve().parent == expected\n"
            "assert Path(pivy._coin.__file__).resolve().parent == expected\n"
            "assert pivy.coin.SoDB.getVersion()\n"
            "Path(os.environ['FC_STARTUP_MARKER']).write_text('initialized')\n",
            encoding="utf-8",
        )
        env = os.environ.copy()
        env.pop("PYTHONOPTIMIZE", None)
        env.update(FREECAD_USER_HOME=str(root / "home"), FC_EXPECTED_PIVY=expected,
                   FC_STARTUP_MARKER=str(marker))
        (root / "home").mkdir()
        result = subprocess.run([executable, str(macro)], cwd=root, env=env,
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace")
        if result.returncode or not marker.is_file() or marker.read_text() != "initialized":
            sys.stderr.write(result.stdout)
            raise SystemExit("FreeCADCmd did not complete bundled Pivy initialization")


if __name__ == "__main__":
    main()
