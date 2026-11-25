#!/usr/bin/env python3
"""Verify SET_BIN_DIR target locations in configured fixture build directories."""
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
for config in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel"):
    for multi in (False, True):
        build = root / ("multi" if multi else "single-" + config)
        paths = dict(line.split("=", 1) for line in
                     (build / f"locations-{config}.txt").read_text().splitlines())
        prefix = config if multi and sys.platform.startswith("linux") else ""
        suffix = config if multi and not sys.platform.startswith("linux") and config not in ("Debug", "Release") else ""
        for target, directory in (("exe", "bin"), ("core", "lib"),
                                  ("module", "Mod/Example"),
                                  ("template", "Mod/_TEMPLATE_")):
            assert Path(paths[target]) == build / prefix / directory / suffix, (config, target, paths[target])
print("FreeCADMultiConfigPaths: all target locations match")
