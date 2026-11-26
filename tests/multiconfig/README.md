# Linux multi-config build-tree checks

From the repository root, `./tests/multiconfig/run_paths.sh <fresh-build-directory>`
configures the real `SET_BIN_DIR` macro with Ninja and Ninja Multi-Config and
checks paths for four configurations. This is a configuration fixture, not a
FreeCAD build or loader qualification.

With bundled Pivy and Part enabled, build `FreeCADMainCmd` and `Part` for each
configuration, then run `ctest -C <config> -R '^FreeCADCmd_multiconfig_part_pivy_imports$'`.
The test starts that configuration's actual executable with loader and Python
path overrides removed, imports and exercises Part and Pivy, verifies their
origins, and checks the shared resource directory. Run again after building
other configurations. Also test installed, single-config, and embedded modes.
A passing fixture or a registered test does not establish native execution.
