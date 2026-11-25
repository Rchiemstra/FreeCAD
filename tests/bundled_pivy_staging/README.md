# Pivy staging and cross-configuration generation

The Linux multi-config package for each configuration is staged under
`<build>/<config>/Mod/pivy`. `test_staging.py` is a compiler-free staging
fixture, not a native loader test. `test_stubs.py` exercises the actual Pivy
generator with a simulated extension and a fixture stubgen in Debug, Release,
RelWithDebInfo and MinSizeRel, including installs and ON/OFF/missing cases.
`test_target_order.py` models the actual `FreeCADMainCmd -> pivy` dependency
and runs ALL, the named target and install in sequence for each configuration,
then repeats after other configurations. It uses pinned checked-in typing files,
a wrong-origin Python package and stale generated markers to check selection.
Linux multi-config installs overlay the selected configuration’s generated stubs.
Other platforms and single-config builds use the upstream rules.

The pinned upstream Pivy generates the SWIG C++ and Python wrapper at shared
paths from configuration-specific build rules; its `pivy` target also copies
sources to the shared directory. Concurrent configuration builds can race in
those rules. They must be serialized, including initial SWIG generation, until
an upstream Pivy change creates the wrapper and shared sources once with a
cross-configuration-safe dependency or serialization scheme. Stub generation is staged per configuration here. Isolated package destinations
do not solve the separate shared SWIG dependency. This is a source-level race
assessment; no concurrent native SWIG failure was observed in this fixture.
