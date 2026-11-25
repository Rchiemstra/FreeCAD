#!/usr/bin/env python3
"""Exercise staged generation and install; no native Pivy or compiler required."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

fixture = Path(__file__).resolve().parent
source = fixture.parents[1]
configs = ("Debug", "Release", "RelWithDebInfo", "MinSizeRel")
def run(*args):
    subprocess.run(args, check=True)

with tempfile.TemporaryDirectory(prefix="pivy-stubs-") as tmp:
    root = Path(tmp)
    stubgen = root / "stubgen"
    stubgen.write_text(f'''#!{sys.executable}
import importlib, os, pathlib, sys
mod = importlib.import_module("pivy.coin")
assert pathlib.Path(mod.__file__).is_relative_to(pathlib.Path(os.getcwd()))
assert os.environ["LD_LIBRARY_PATH"].split(":")[0].endswith("coin-lib")
out = pathlib.Path(sys.argv[sys.argv.index("-o") + 1]) / "pivy" / "coin.pyi"
out.write_text("# config " + mod.CONFIG + "\\n")
''')
    stubgen.chmod(0o755)
    for mode, program in (("ON", str(stubgen)), ("OFF", str(stubgen)), ("ON", "")):
        build = root / (mode + ("-found" if program else "-missing"))
        run("cmake", "-S", str(fixture / "stubs_fixture"), "-B", str(build),
            "-G", "Ninja Multi-Config", "-DCMAKE_CONFIGURATION_TYPES=" + ";".join(configs),
            f"-DHELPER_DIR={source / 'cMake/FreeCAD_Helpers'}",
            f"-DPIVY_SOURCE={source / 'src/3rdParty/pivy'}", f"-DPYTHON={sys.executable}",
            f"-DPIVY_GENERATE_PYI={mode}", f"-DSTUBGEN={program}")
        assert f"PIVY_GENERATE_PYI:BOOL={mode}" in (build / "CMakeCache.txt").read_text()
        for config in configs:
            run("cmake", "--build", str(build), "--config", config)
            prefix = build / ("install-" + config)
            run("cmake", "--install", str(build), "--config", config, "--prefix", str(prefix))
            package = prefix / "Mod/pivy"
            if mode == "ON" and program:
                assert (package / "coin.pyi").read_text().find(f"# config {config}") >= 0
                assert (package / "_coin.pyi").is_file()
                assert (package / "py.typed").is_file()
            else:
                assert (package / "coin.pyi").read_bytes() == (source / "src/3rdParty/pivy/pivy/coin.pyi").read_bytes()
                assert (package / "py.typed").exists() == (source / "src/3rdParty/pivy/pivy/py.typed").exists()
    print("Pivy stub fixture passed")
