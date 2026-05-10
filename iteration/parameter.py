"""
iteration/parameter.py — FreeCAD parameter get/set via XML-RPC.

Exposes two functions that an LLM agent uses to read and write design
parameters in a running FreeCAD document:

    get_parameter(doc_name, param_name)   -> float | str
    set_parameter(doc_name, param_name, value)  -> None

Design-to-sim is the only supported direction in v1.  Writes to CAD
properties are intentionally one-directional; simulation results never
flow back into the FreeCAD document automatically.

Supported parameter types
--------------------------
FreeCAD parameters are read/written through the `execute_code` RPC
call on the freecad-mcp XML-RPC server (localhost:9875).  Two
mechanisms are supported:

1. **Spreadsheet cell** — most parametric models use a master spreadsheet.
   A parameter named ``"link1_length"`` maps to a cell alias of the same
   name in any spreadsheet in the document.

2. **Object property** — ``"MyObject.Length"`` (dot notation) directly
   accesses ``doc.getObject("MyObject").Length``.

If the FreeCAD server is not running, both functions raise ``ConnectionError``
with a clear message instead of hanging.
"""
from __future__ import annotations

import logging
from typing import Any

log = logging.getLogger(__name__)

# Default XML-RPC endpoint (matches FreeCADMCP addon defaults)
_FREECAD_RPC_URL = "http://localhost:9875"


# ---------------------------------------------------------------------------
# get_parameter
# ---------------------------------------------------------------------------

def get_parameter(
    param_name: str,
    doc_name: str = "",
    rpc_url: str = _FREECAD_RPC_URL,
) -> Any:
    """
    Read the value of a design parameter from the active FreeCAD document.

    Parameters
    ----------
    param_name : str
        Parameter name — either a spreadsheet alias (e.g. ``"link1_length"``)
        or dot notation (e.g. ``"Box.Length"``).
    doc_name : str
        Target document name. If empty, uses ``FreeCAD.ActiveDocument``.
    rpc_url : str
        XML-RPC URL of the freecad-mcp addon server.

    Returns
    -------
    float | str | bool
        The parameter value as returned by FreeCAD.

    Raises
    ------
    ConnectionError
        If the FreeCAD RPC server is unreachable.
    ValueError
        If the parameter cannot be found in the document.
    """
    code = _build_get_code(param_name, doc_name)
    result = _rpc_execute(code, rpc_url)
    return result


def _build_get_code(param_name: str, doc_name: str) -> str:
    doc_expr = f'FreeCAD.getDocument("{doc_name}")' if doc_name else "FreeCAD.ActiveDocument"

    if "." in param_name:
        obj_name, prop_name = param_name.split(".", 1)
        return (
            f'doc = {doc_expr}\n'
            f'obj = doc.getObject("{obj_name}")\n'
            f'if obj is None: raise ValueError("Object {obj_name!r} not found")\n'
            f'result = getattr(obj, "{prop_name}", None)\n'
            f'if result is None: raise ValueError("Property {prop_name!r} not found on {obj_name!r}")\n'
            f'print(repr(result))'
        )
    else:
        # Spreadsheet alias lookup
        return (
            f'doc = {doc_expr}\n'
            f'result = None\n'
            f'for obj in doc.Objects:\n'
            f'    if obj.TypeId == "Spreadsheet::Sheet":\n'
            f'        try:\n'
            f'            result = obj.get("{param_name}")\n'
            f'            break\n'
            f'        except Exception:\n'
            f'            pass\n'
            f'if result is None:\n'
            f'    raise ValueError("Parameter {param_name!r} not found in any spreadsheet")\n'
            f'print(repr(result))'
        )


# ---------------------------------------------------------------------------
# set_parameter
# ---------------------------------------------------------------------------

def set_parameter(
    param_name: str,
    value: Any,
    doc_name: str = "",
    rpc_url: str = _FREECAD_RPC_URL,
    recompute: bool = True,
) -> None:
    """
    Write a design parameter in the active FreeCAD document.

    The document is recomputed after the change if ``recompute=True``
    (default).  All changes are reflected in the FreeCAD 3D view.

    Parameters
    ----------
    param_name : str
        Parameter name (spreadsheet alias or ``"Object.Property"``).
    value : float | str | bool
        New value.  For spreadsheet aliases the value is set as a quantity;
        for object properties it is set directly.
    doc_name : str
        Target document name. If empty, uses FreeCAD.ActiveDocument.
    rpc_url : str
        XML-RPC URL of the freecad-mcp addon server.
    recompute : bool
        If True, call ``doc.recompute()`` after setting the value.

    Raises
    ------
    ConnectionError
        If the FreeCAD RPC server is unreachable.
    ValueError
        If the parameter cannot be found or the value is invalid.
    """
    code = _build_set_code(param_name, value, doc_name, recompute)
    _rpc_execute(code, rpc_url)
    log.info("[parameter] set %s = %r", param_name, value)


def _build_set_code(
    param_name: str,
    value: Any,
    doc_name: str,
    recompute: bool,
) -> str:
    doc_expr  = f'FreeCAD.getDocument("{doc_name}")' if doc_name else "FreeCAD.ActiveDocument"
    recompute_line = "doc.recompute()" if recompute else ""
    value_repr = repr(value)

    if "." in param_name:
        obj_name, prop_name = param_name.split(".", 1)
        return (
            f'doc = {doc_expr}\n'
            f'obj = doc.getObject("{obj_name}")\n'
            f'if obj is None: raise ValueError("Object {obj_name!r} not found")\n'
            f'setattr(obj, "{prop_name}", {value_repr})\n'
            + recompute_line
        )
    else:
        return (
            f'doc = {doc_expr}\n'
            f'found = False\n'
            f'for obj in doc.Objects:\n'
            f'    if obj.TypeId == "Spreadsheet::Sheet":\n'
            f'        try:\n'
            f'            obj.set("{param_name}", str({value_repr}))\n'
            f'            found = True\n'
            f'            break\n'
            f'        except Exception:\n'
            f'            pass\n'
            f'if not found:\n'
            f'    raise ValueError("Parameter {param_name!r} not found in any spreadsheet")\n'
            + recompute_line
        )


# ---------------------------------------------------------------------------
# RPC helper
# ---------------------------------------------------------------------------

def _rpc_execute(code: str, rpc_url: str) -> Any:
    """Call the freecad-mcp execute_code RPC endpoint."""
    try:
        import xmlrpc.client
        proxy = xmlrpc.client.ServerProxy(rpc_url, allow_none=True)
        result = proxy.execute_code(code)
        return result
    except ConnectionRefusedError as exc:
        raise ConnectionError(
            f"FreeCAD RPC server not reachable at {rpc_url}. "
            "Start FreeCAD and enable 'Auto-Start RPC Server' in the MCP addon."
        ) from exc
    except Exception as exc:
        # Surface the actual FreeCAD error cleanly
        if "ValueError" in str(exc) or "AttributeError" in str(exc):
            raise ValueError(str(exc)) from exc
        raise
