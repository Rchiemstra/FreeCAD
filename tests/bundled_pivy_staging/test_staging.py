#!/usr/bin/env python3
"""Exercise the FreeCAD staging helper without compiling FreeCAD or Pivy."""
import pathlib
import subprocess
import sys
import tempfile

fixture = pathlib.Path(__file__).resolve().parent
helper = fixture.parents[1] / "cMake" / "FreeCAD_Helpers"

def run(*args):
    subprocess.run(args, check=True)

with tempfile.TemporaryDirectory(prefix="pivy-stage-") as temp:
    build = pathlib.Path(temp)
    run("cmake", "-S", str(fixture), "-B", str(build), "-G", "Ninja Multi-Config",
        "-DCMAKE_CONFIGURATION_TYPES=Debug;Release", f"-DHELPER_DIR={helper}")
    def build_config(config):
        run("cmake", "--build", str(build), "--config", config, "--target", "pivy")
    def check(config):
        package = build / config / "Mod" / "pivy"
        assert (package / "_coin.fixture").read_text() == f"{config.lower()} extension\n"
        assert (package / "coin.py").read_text() == "# generated wrapper\n"
        assert (package / "__init__.py").is_file()
        assert (package / "gui" / "__init__.py").is_file()
        return package
    build_config("Debug")
    debug = check("Debug")
    (debug / "obsolete.py").write_text("old")
    build_config("Release")
    check("Release")
    check("Debug")
    source = build / "input" / "pivy" / "__init__.py"
    source.write_text("# updated package\n")
    build_config("Debug")
    assert (debug / "__init__.py").read_text() == "# updated package\n"
    assert not (debug / "obsolete.py").exists()
    assert (check("Release") / "__init__.py").read_text() == "# package\n"
    print("Pivy staging fixture passed")
