# SPDX-License-Identifier: LGPL-2.1-or-later
"""FreeCAD InitGui entry for isolated-profile Part3LocalDriver."""

from __future__ import annotations

import os as _os
import sys as _sys
from pathlib import Path as _Path

from PySide import QtCore

try:
    _addon_dir = _os.path.dirname(_os.path.abspath(__file__))
except NameError:
    import inspect as _inspect

    _addon_dir = _os.path.dirname(
        _os.path.abspath(_inspect.getfile(_inspect.currentframe()))
    )
if _addon_dir not in _sys.path:
    _sys.path.insert(0, _addon_dir)

_RUNTIME_STATE: dict[str, object] = {}


def _install(
    _os_module=_os,
    _qt_core=QtCore,
    _Path=_Path,
    _state=_RUNTIME_STATE,
):
    token_hex = _os_module.environ.get("PART3_LOCAL_CONTROL_TOKEN", "").strip()
    endpoint_dir = _os_module.environ.get("PART3_CONTROL_ENDPOINT_DIR", "").strip()
    if not token_hex or not endpoint_dir:
        FreeCAD.Console.PrintWarning(  # noqa: F821
            "[Part3LocalDriver] missing control env; driver not started\n"
        )
        return
    try:
        token = bytes.fromhex(token_hex)
    except ValueError:
        FreeCAD.Console.PrintWarning(  # noqa: F821
            "[Part3LocalDriver] invalid control token encoding\n"
        )
        return
    if len(token) != 32:
        FreeCAD.Console.PrintWarning(  # noqa: F821
            "[Part3LocalDriver] control token must be 32 bytes\n"
        )
        return
    if _state.get("installed"):
        return

    from control_channel import start_control_channel
    from driver import LocalUserDriverCore

    driver = LocalUserDriverCore()
    channel = start_control_channel(
        token,
        _Path(endpoint_dir),
        driver.submit_from_thread,
    )
    _state["installed"] = True
    _state["driver"] = driver
    _state["channel"] = channel
    FreeCAD.Console.PrintMessage(  # noqa: F821
        f"[Part3LocalDriver] control channel ready at {channel.endpoint_path}\n"
    )


QtCore.QTimer.singleShot(0, _install)
