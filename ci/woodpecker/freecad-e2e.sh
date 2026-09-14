#!/bin/sh
set -e

# Same headless GL contract as freecad-unit-tests.sh Gui_tests_run.
# Without this, FreeCAD -t <Gui> can SIGSEGV on the agent (exit 245 = -11)
# after listing and running modules, which is what pipeline 350/351/352 did.
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"
export PYTHONUNBUFFERED=1

xvfb-run -a -s "-screen 0 1024x768x24" python3 .github/scripts/run_gui_tests.py build/debug
