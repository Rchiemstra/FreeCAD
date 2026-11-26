# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run the actual command-line application and verify configuration-specific imports."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable, build, config, resources = sys.argv[1:]
    executable = str(Path(executable).resolve())
    build = Path(build).resolve()
    resources = Path(resources)
    if not resources.is_absolute():
        resources = build / resources
    with tempfile.TemporaryDirectory(prefix="freecad-multiconfig-") as tmp:
        root = Path(tmp)
        home = root / "home"
        home.mkdir()
        marker = root / "success"
        macro = root / "imports.py"
        macro.write_text('''import FreeCAD as App
from pathlib import Path
import os
import Part
import pivy, pivy.coin, pivy._coin
root = Path(os.environ["FC_BUILD_ROOT"])
config = os.environ["FC_BUILD_CONFIG"]
assert App.ConfigGet("BuildTreeConfiguration") == config
assert App.ConfigGet("PIVY_SOURCE") == "bundled"
assert Path(App.getHomePath()).resolve() == root
assert Path(App.getResourceDir()).resolve() == Path(os.environ["FC_RESOURCES"])
assert Path(Part.__file__).resolve().parent == root / config / "Mod" / "Part"
for module in (pivy, pivy.coin, pivy._coin):
    assert Path(module.__file__).resolve().parent == root / config / "Mod" / "pivy"
assert pivy.coin.SoDB.getVersion()
assert not Part.makeBox(1, 2, 3).isNull()
Path(os.environ["FC_MARKER"]).write_text("ok")
''', encoding="utf-8")
        env = os.environ.copy()
        for name in ("PYTHONPATH", "PYTHONHOME", "PYTHONOPTIMIZE", "LD_LIBRARY_PATH", "LD_PRELOAD"):
            env.pop(name, None)
        env.update(FREECAD_USER_HOME=str(home), FC_BUILD_ROOT=str(build),
                   FC_BUILD_CONFIG=config, FC_RESOURCES=str(resources.resolve()), FC_MARKER=str(marker))
        result = subprocess.run([executable, str(macro)], cwd=root, env=env,
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace")
        if result.returncode or not marker.is_file() or marker.read_text() != "ok":
            sys.stderr.write(result.stdout)
            raise SystemExit("FreeCADCmd did not complete the Part and Pivy import regression")


if __name__ == "__main__":
    main()
