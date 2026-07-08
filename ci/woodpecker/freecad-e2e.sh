#!/bin/sh
set -e

xvfb-run -a -s "-screen 0 1024x768x24" python3 .github/scripts/run_gui_tests.py build/debug
