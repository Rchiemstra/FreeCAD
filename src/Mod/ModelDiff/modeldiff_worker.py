# SPDX-License-Identifier: LGPL-2.1-or-later
"""Private FreeCADCmd entry point; never imports GUI code or live GUI inputs."""

import json
import os

import modeldiff


def main():
    directory = os.environ["FREECAD_MODELDIFF_JOB"]
    try:
        with open(os.path.join(directory, "request.json"), encoding="utf-8") as stream:
            request = json.load(stream)
        result = modeldiff.compare_files(
            request["local"], request["remote"], _pair=request["pair"]
        )
        modeldiff._write_result(result, directory)
    except Exception as exc:  # noqa: BLE001 - send native/Python errors to the GUI
        with open(
            os.path.join(directory, "error.txt"), "w", encoding="utf-8"
        ) as stream:
            stream.write(str(exc))


main()
