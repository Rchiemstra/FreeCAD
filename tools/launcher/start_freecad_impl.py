#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
Start FreeCAD and bring up the MCP RPC server (FreeCADMCP addon).

Dev builds on Windows need Qt and other DLLs from the pixi/conda environment.
This script adds those directories to PATH before launching FreeCAD.

Examples:

  python start_freecad.py
  python start_freecad.py model.FCStd
  python start_freecad.py --freecad FreeCAD/build/release/bin/FreeCAD.exe
  python start_freecad.py --no-wait-for-mcp
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


MCP_RPC_PORT = 9875
MCP_SETTINGS_FILENAME = "freecad_mcp_settings.json"
MCP_ADDON_DIRNAME = "FreeCADMCP"
PIXI_EXTERNAL_MODS = ("freecad.robotcad",)


def _is_freecad_repo(path: Path) -> bool:
    return (path / "src" / "Main").is_dir()


def _repo_root() -> Path:
    for env_var in ("FREECAD_REPO", "FREECAD_ROOT"):
        value = os.environ.get(env_var, "").strip()
        if value:
            return Path(value)

    # This module lives at <repo>/tools/launcher/, so the repository is an
    # ancestor rather than the script's own directory. Walk up and take the
    # first ancestor that actually looks like the FreeCAD source tree.
    script_dir = Path(__file__).resolve().parent
    for candidate in (script_dir, *script_dir.parents):
        if _is_freecad_repo(candidate):
            return candidate

    # Kept for the historical layout, where the launcher sat beside a FreeCAD
    # checkout rather than inside one.
    nested = script_dir / "FreeCAD"
    if _is_freecad_repo(nested):
        return nested

    return script_dir


def _pixi_env_dir() -> Path | None:
    for env_var in ("PIXI_ENV", "CONDA_PREFIX"):
        value = os.environ.get(env_var, "").strip()
        if value:
            return Path(value)

    pixi = _repo_root() / ".pixi" / "envs" / "default"
    if pixi.is_dir():
        return pixi

    return None


def _pixi_exe() -> str | None:
    return shutil.which("pixi") or shutil.which("pixi.exe")


def _is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def _path_for_pixi_run(path: Path, cwd: Path) -> str:
    try:
        return path.resolve().relative_to(cwd.resolve()).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def _freecad_mcp_dir() -> Path:
    return _repo_root() / "tools" / "mcp" / "freecad-mcp"


def _build_resource_mod_dir(freecad_exe: Path) -> Path | None:
    if freecad_exe.parent.name.lower() != "bin":
        return None

    candidate = freecad_exe.parent.parent / "data" / "Mod"
    if candidate.is_dir():
        return candidate

    return None


def _link_or_copy_dir(source: Path, target: Path) -> None:
    if target.exists():
        return

    target.parent.mkdir(parents=True, exist_ok=True)

    try:
        os.symlink(source, target, target_is_directory=True)
        print(f"Linked FreeCAD workbench resources: {target} -> {source}")
        return
    except OSError:
        pass

    if os.name == "nt":
        result = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(target), str(source)],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            print(f"Linked FreeCAD workbench resources: {target} -> {source}")
            return

    shutil.copytree(source, target)
    print(f"Copied FreeCAD workbench resources to: {target}")


def _ensure_pixi_external_mods(freecad_exe: Path) -> None:
    target_mod_dir = _build_resource_mod_dir(freecad_exe)
    pixi_env = _pixi_env_dir()
    if target_mod_dir is None or pixi_env is None:
        return

    source_mod_dir = pixi_env / "Library" / "data" / "Mod"
    if not source_mod_dir.is_dir():
        return

    for mod_name in PIXI_EXTERNAL_MODS:
        source = source_mod_dir / mod_name
        target = target_mod_dir / mod_name
        if source.is_dir() and not target.exists():
            _link_or_copy_dir(source, target)


def _default_freecad() -> str | None:
    for env_var in ("FREECAD", "FC_FREECAD", "FREECAD_EXE"):
        value = os.environ.get(env_var, "").strip()
        if value:
            return value

    root = _repo_root()
    build_dir = root / "build"
    candidates = [
        build_dir / "release" / "bin" / "FreeCAD.exe",
        build_dir / "release" / "bin" / "FreeCAD",
        build_dir / "debug" / "bin" / "FreeCAD.exe",
        build_dir / "debug" / "bin" / "FreeCAD",
        build_dir / "bin" / "FreeCAD.exe",
        build_dir / "bin" / "FreeCAD",
        *sorted(build_dir.glob("*/bin/FreeCAD.exe")),
        *sorted(build_dir.glob("*/bin/FreeCAD")),
    ]

    pixi = _pixi_env_dir()
    if pixi is not None:
        candidates.append(pixi / "Library" / "bin" / "FreeCAD.exe")
        candidates.append(pixi / "Library" / "bin" / "FreeCAD")

    candidate = _newest_existing_file(candidates)
    if candidate is not None:
        return str(candidate)

    return shutil.which("FreeCAD") or shutil.which("FreeCAD.exe")


def _newest_existing_file(candidates: list[Path]) -> Path | None:
    existing: list[Path] = []
    seen: set[str] = set()

    for candidate in candidates:
        if sys.platform == "win32" and candidate.suffix.lower() != ".exe":
            continue
        if not candidate.is_file():
            continue
        try:
            key = str(candidate.resolve()).lower()
        except OSError:
            key = str(candidate.absolute()).lower()
        if key in seen:
            continue
        seen.add(key)
        existing.append(candidate)

    if not existing:
        return None

    def sort_key(path: Path) -> tuple[int, int, str]:
        try:
            mtime = path.stat().st_mtime_ns
        except OSError:
            mtime = 0
        exe_preference = 1 if path.suffix.lower() == ".exe" else 0
        return (mtime, exe_preference, str(path).lower())

    return max(existing, key=sort_key)


def _freecad_user_data_base() -> Path:
    if sys.platform == "win32":
        appdata = os.environ.get("APPDATA", "").strip()
        if appdata:
            return Path(appdata) / "FreeCAD"
        return Path.home() / "AppData" / "Roaming" / "FreeCAD"

    xdg_data = os.environ.get("XDG_DATA_HOME", "").strip()
    if xdg_data:
        return Path(xdg_data) / "FreeCAD"

    return Path.home() / ".local" / "share" / "FreeCAD"


def _freecad_user_data_dirs() -> list[Path]:
    base = _freecad_user_data_base()
    dirs = [base]
    if base.is_dir():
        for child in sorted(base.glob("v*")):
            if child.is_dir():
                dirs.append(child)
    return dirs


def _runtime_path_entries(freecad_exe: Path) -> list[str]:
    entries: list[str] = []
    seen: set[str] = set()

    def add(path: Path) -> None:
        if not path.is_dir():
            return
        key = str(path.resolve()).lower()
        if key in seen:
            return
        seen.add(key)
        entries.append(str(path))

    add(freecad_exe.parent)

    if freecad_exe.parent.name.lower() == "bin":
        mod_dir = freecad_exe.parent.parent / "Mod"
        if mod_dir.is_dir():
            for child in sorted(mod_dir.iterdir()):
                if child.is_dir() and any(child.glob("*.pyd")):
                    add(child)

    pixi = _pixi_env_dir()
    if pixi is not None:
        add(pixi)
        add(pixi / "Library" / "bin")
        add(pixi / "Library" / "lib")
        add(pixi / "Library" / "lib" / "qt6" / "bin")
        add(pixi / "Library" / "lib" / "qt6" / "plugins")

    for part in os.environ.get("PATH", "").split(os.pathsep):
        part = part.strip()
        if part:
            add(Path(part))

    return entries


def _launch_env(freecad_exe: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join(_runtime_path_entries(freecad_exe))
    mcp_root = str(_freecad_mcp_dir())
    existing_pythonpath = env.get("PYTHONPATH", "")
    pythonpath_parts = [mcp_root]
    if existing_pythonpath:
        pythonpath_parts.extend(
            part
            for part in existing_pythonpath.split(os.pathsep)
            if part and part != mcp_root
        )
    env["PYTHONPATH"] = os.pathsep.join(pythonpath_parts)

    pixi = _pixi_env_dir()
    if pixi is not None:
        env.setdefault("CONDA_PREFIX", str(pixi))
        env.update(_qt_plugin_env(pixi))

    return env


def _qt_plugin_env(pixi_env: Path) -> dict[str, str]:
    plugin_root = pixi_env / "Library" / "lib" / "qt6" / "plugins"
    return {
        "QT_PLUGIN_PATH": str(plugin_root),
        "QT_QPA_PLATFORM_PLUGIN_PATH": str(plugin_root / "platforms"),
    }


def _pixi_for_freecad(freecad_exe: Path) -> str | None:
    pixi = _pixi_exe()
    if pixi is None:
        return None

    root = _repo_root()
    if not (root / "pixi.toml").is_file():
        return None

    if _is_relative_to(freecad_exe, root / "build"):
        return pixi

    pixi_env = _pixi_env_dir()
    if pixi_env is not None and _is_relative_to(freecad_exe, pixi_env):
        return pixi

    return None


def _launch_details(
    freecad_exe: Path,
    freecad_args: list[str],
) -> tuple[list[str], str, dict[str, str]]:
    pixi = _pixi_for_freecad(freecad_exe)
    if pixi is not None:
        cwd = _repo_root()
        env = _launch_env(freecad_exe)
        return (
            [pixi, "run", "--", _path_for_pixi_run(freecad_exe, cwd), *freecad_args],
            str(cwd),
            env,
        )

    return (
        [str(freecad_exe), *freecad_args],
        str(freecad_exe.parent),
        _launch_env(freecad_exe),
    )


def _is_reparse_point(path: Path) -> bool:
    """True if path is a symlink or (on Windows) a directory junction."""
    try:
        st = path.lstat()
    except OSError:
        return False
    if os.name == "nt":
        FILE_ATTRIBUTE_REPARSE_POINT = 0x400
        return bool(getattr(st, "st_file_attributes", 0) & FILE_ATTRIBUTE_REPARSE_POINT)
    import stat as _stat

    return _stat.S_ISLNK(st.st_mode)


def _link_points_to(path: Path, source: Path) -> bool:
    try:
        return path.resolve() == source.resolve()
    except OSError:
        return False


def _remove_install(path: Path) -> None:
    """Remove an existing install without following a link into its target."""
    if _is_reparse_point(path):
        # Drop the link itself only; never recurse into the source contents.
        os.rmdir(path)
    elif path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def _install_mcp_addon_to(mod_dir: Path, source: Path) -> None:
    # Keep an existing link that already points at the current source. Anything
    # else (stale plain copy, or a link to the wrong place) is replaced so the
    # addon FreeCAD loads always matches the repo source.
    if _is_reparse_point(mod_dir) and _link_points_to(mod_dir, source):
        return

    _remove_install(mod_dir)
    mod_dir.parent.mkdir(parents=True, exist_ok=True)

    try:
        os.symlink(source, mod_dir, target_is_directory=True)
        print(f"Linked MCP addon: {mod_dir} -> {source}")
        return
    except OSError:
        pass

    if os.name == "nt":
        result = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(mod_dir), str(source)],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            print(f"Linked MCP addon (junction): {mod_dir} -> {source}")
            return

    shutil.copytree(source, mod_dir)
    print(f"Copied MCP addon to: {mod_dir}")


def _ensure_mcp_addon_installed() -> None:
    source = _freecad_mcp_dir() / "addon" / MCP_ADDON_DIRNAME
    if not source.is_dir():
        print(f"WARNING: MCP addon source not found: {source}", file=sys.stderr)
        return

    for data_dir in _freecad_user_data_dirs():
        _install_mcp_addon_to(data_dir / "Mod" / MCP_ADDON_DIRNAME, source)


def _ensure_mcp_auto_start() -> None:
    defaults = {
        "remote_enabled": False,
        "allowed_ips": "127.0.0.1",
        "auto_start_rpc": True,
    }

    for data_dir in _freecad_user_data_dirs():
        settings_path = data_dir / MCP_SETTINGS_FILENAME
        settings_path.parent.mkdir(parents=True, exist_ok=True)

        settings = dict(defaults)
        if settings_path.is_file():
            try:
                loaded = json.loads(settings_path.read_text(encoding="utf-8"))
                if isinstance(loaded, dict):
                    settings.update(loaded)
            except (OSError, json.JSONDecodeError) as exc:
                print(f"WARNING: could not read {settings_path}: {exc}", file=sys.stderr)

        settings["auto_start_rpc"] = True
        settings_path.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")


class JsonRpcError(Exception):
    """A JSON-RPC 2.0 error object returned by the server, preserved intact."""

    def __init__(self, code: object, message: object, data: object = None) -> None:
        super().__init__(f"JSON-RPC error {code}: {message}")
        self.code = code
        self.message = message
        self.data = data


class JsonRpcTransportError(Exception):
    """Transport, protocol, or response-shape failure. Never a server error."""


class JsonRpcClient:
    """Strict JSON-RPC 2.0 client for the MCP listener.

    The listener retired XML-RPC: it answers 410 Gone on /RPC2 with
    `Link: </jsonrpc>; rel="successor-version"`. There is deliberately no
    XML-RPC fallback here -- falling back would silently reintroduce a
    transport the server rejects.

    Every response is validated: HTTP status, JSON object shape, protocol
    version, request-ID match, and exactly one of result/error.
    """

    JSON_RPC_PATH = "/jsonrpc"

    def __init__(self, host: str = "localhost", port: int = MCP_RPC_PORT) -> None:
        self.host = host
        self.port = port
        self.url = f"http://{host}:{port}{self.JSON_RPC_PATH}"
        self._next_id = 0

    def call(self, method: str, params: object = None, timeout: float = 30.0) -> object:
        self._next_id += 1
        request_id = self._next_id
        payload = json.dumps({
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
            "params": {} if params is None else params,
        }).encode("utf-8")
        request = urllib.request.Request(
            self.url, data=payload, headers={"Content-Type": "application/json"}
        )

        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                status = getattr(response, "status", response.getcode())
                body = response.read()
        except urllib.error.HTTPError as exc:  # includes the retired 410
            detail = ""
            try:
                detail = exc.read().decode("utf-8", "replace")[:200]
            except Exception:  # noqa: BLE001
                pass
            raise JsonRpcTransportError(
                f"HTTP {exc.code} from {self.url}: {detail}"
            ) from exc
        except urllib.error.URLError as exc:
            raise JsonRpcTransportError(f"cannot reach {self.url}: {exc.reason}") from exc
        except OSError as exc:
            raise JsonRpcTransportError(f"cannot reach {self.url}: {exc}") from exc

        if status != 200:
            raise JsonRpcTransportError(f"HTTP {status} from {self.url}")

        try:
            decoded = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise JsonRpcTransportError(f"malformed JSON from {self.url}: {exc}") from exc

        if not isinstance(decoded, dict):
            raise JsonRpcTransportError(f"response is not a JSON object: {type(decoded).__name__}")
        if decoded.get("jsonrpc") != "2.0":
            raise JsonRpcTransportError(f"unexpected jsonrpc version: {decoded.get('jsonrpc')!r}")
        if decoded.get("id") != request_id:
            raise JsonRpcTransportError(
                f"response id {decoded.get('id')!r} does not match request id {request_id!r}"
            )

        has_result = "result" in decoded
        has_error = "error" in decoded
        if has_result == has_error:
            raise JsonRpcTransportError(
                "response must carry exactly one of 'result' or 'error'"
            )
        if has_error:
            error = decoded["error"]
            if not isinstance(error, dict):
                raise JsonRpcTransportError(f"error member is not an object: {error!r}")
            raise JsonRpcError(error.get("code"), error.get("message"), error.get("data"))
        return decoded["result"]


def _ping_mcp_rpc(host: str = "localhost", port: int = MCP_RPC_PORT) -> bool:
    try:
        return bool(JsonRpcClient(host, port).call("ping", timeout=5.0))
    except (JsonRpcError, JsonRpcTransportError):
        return False


def _mcp_rpc_proxy(host: str = "localhost", port: int = MCP_RPC_PORT):
    client = JsonRpcClient(host, port)
    try:
        if client.call("ping", timeout=5.0):
            return client
    except (JsonRpcError, JsonRpcTransportError):
        return None
    return None


def _mcp_rpc_process_id(proxy) -> int | None:
    """PID of a session this launcher started.

    This runs code in the target process, so it must never be used to identify
    a session we do not own. Callers must have established ownership first.
    """
    if proxy is None:
        return None
    try:
        result = proxy.call(
            "execute_code",
            {"code": "import os\nprint('freecad_process_id=' + str(os.getpid()))"},
            timeout=30.0,
        )
    except (JsonRpcError, JsonRpcTransportError):
        return None

    if not isinstance(result, dict) or not result.get("success"):
        return None

    for line in str(result.get("message", "")).splitlines():
        line = line.strip()
        if line.startswith("Output:"):
            line = line[len("Output:"):].strip()
        if not line.startswith("freecad_process_id="):
            continue
        try:
            return int(line.split("=", 1)[1])
        except ValueError:
            return None

    return None


def _port_is_occupied(host: str = "localhost", port: int = MCP_RPC_PORT) -> bool:
    """True if anything accepts a TCP connection on the endpoint."""
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as sock:
        sock.settimeout(1.0)
        return sock.connect_ex((host, port)) == 0


def _resolve_freecad_file_arg(arg: str) -> Path | None:
    if arg.startswith("-"):
        return None

    path = Path(arg).expanduser()
    if not path.is_absolute():
        path = Path.cwd() / path

    try:
        path = path.resolve()
    except OSError:
        return None

    if path.is_file():
        return path
    return None


def _open_files_in_mcp(proxy, freecad_args: list[str]) -> bool:
    if not freecad_args:
        return True

    paths: list[Path] = []
    for arg in freecad_args:
        path = _resolve_freecad_file_arg(arg)
        if path is None:
            return False
        paths.append(path)

    code = f"""
import os
import FreeCAD

try:
    import FreeCADGui
except Exception:
    FreeCADGui = None

_paths = {json.dumps([str(path) for path in paths])}
_opened = []

def _existing_doc_for_path(_path):
    _target = os.path.normcase(os.path.abspath(_path))
    for _doc in FreeCAD.listDocuments().values():
        _filename = getattr(_doc, "FileName", "")
        if _filename and os.path.normcase(os.path.abspath(_filename)) == _target:
            return _doc
    return None

for _path in _paths:
    _doc = _existing_doc_for_path(_path)
    if _doc is None:
        _doc = FreeCAD.openDocument(_path)
    if _doc is not None:
        FreeCAD.setActiveDocument(_doc.Name)
        _opened.append(_doc.Name)

if FreeCADGui is not None:
    try:
        FreeCADGui.getMainWindow().raise_()
    except Exception:
        pass

print("opened_documents=" + repr(_opened))
"""
    result = proxy.execute_code(code)
    if not result.get("success"):
        print(
            f"ERROR: existing MCP server could not open file(s): {result.get('error')}",
            file=sys.stderr,
        )
        return False
    message = result.get("message", "")
    if message:
        print(message.strip())
    return True


# Automatic session reuse is disabled.
#
# While the readiness probe still spoke the retired XML-RPC transport it could
# never succeed, so this path was dead and the risk was theoretical. Migrating
# to JSON-RPC makes it live again, which is only safe if the launcher can first
# prove the listening session is the one it means to drive.
#
# That proof needs a READ-ONLY method reporting the executable, the profile or
# instance identity, the endpoint, and a live PID. The surface offers no such
# method: `ping` returns only true, and `handshake_v2` is an authenticated
# lease operation requiring credentials and a payload. The only way to identify
# a session would be to run `execute_code` inside it -- executing code in a
# process of unknown provenance purely to discover what it is, which is exactly
# backwards.
#
# So reuse stays off until a read-only identity method exists. An occupied port
# is refused instead, and never attached to.
REUSE_DISABLED_REASON = (
    "no read-only identity method is available to prove which session is listening"
)


def _reuse_existing_mcp_if_possible(
    freecad_args: list[str],
    force_new: bool,
) -> bool:
    del freecad_args, force_new
    return False


def _wait_for_mcp_rpc(
    timeout_s: float,
    host: str = "localhost",
    port: int = MCP_RPC_PORT,
    process: subprocess.Popen[bytes] | None = None,
    previous_mcp_pid: int | None = None,
) -> str:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if process is None:
            if _ping_mcp_rpc(host, port):
                return "ready"
        else:
            proxy = _mcp_rpc_proxy(host, port)
            if proxy is not None:
                rpc_pid = _mcp_rpc_process_id(proxy)
                if rpc_pid is None:
                    if previous_mcp_pid is None:
                        return "ready"
                elif previous_mcp_pid is None or rpc_pid != previous_mcp_pid:
                    return "ready"
        if process is not None and process.poll() is not None:
            return "exited"
        time.sleep(0.5)
    return "timeout"


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--freecad",
        default=_default_freecad(),
        help=(
            "Path to the FreeCAD executable "
            "(default: pixi install, build/*/bin/FreeCAD, or PATH)"
        ),
    )
    parser.add_argument(
        "--wait",
        action="store_true",
        help="Wait for FreeCAD to exit before this script returns",
    )
    parser.add_argument(
        "--no-wait-for-mcp",
        action="store_true",
        help="Do not wait for the MCP RPC server to respond after launching FreeCAD",
    )
    parser.add_argument(
        "--force-new",
        action="store_true",
        help="Start a new FreeCAD process even if the MCP RPC server is already running",
    )
    parser.add_argument(
        "--mcp-timeout",
        type=float,
        default=120.0,
        help="Seconds to wait for the MCP RPC server (default: 120)",
    )
    parser.add_argument(
        "args",
        nargs=argparse.REMAINDER,
        help="Arguments passed to FreeCAD (e.g. a .FCStd file)",
    )
    return parser.parse_args(argv)


def _start_freecad(
    freecad: Path,
    freecad_args: list[str],
    wait: bool,
) -> subprocess.Popen[bytes] | int:
    launch_args, cwd, env = _launch_details(freecad, freecad_args)
    process = subprocess.Popen(launch_args, cwd=cwd, env=env)
    if wait:
        return process
    return process


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv if argv is not None else sys.argv[1:])

    if not args.freecad:
        print(
            "ERROR: could not find FreeCAD; pass --freecad "
            "FreeCAD/build/<preset>/bin/FreeCAD.exe (or set $FREECAD)",
            file=sys.stderr,
        )
        return 1

    freecad = Path(args.freecad)
    if not freecad.is_file():
        print(f"ERROR: FreeCAD executable not found: {freecad}", file=sys.stderr)
        return 1

    # Refuse an occupied endpoint before touching the profile or starting
    # anything. Starting a second instance would race for the port, and
    # attaching is not permitted because the listener cannot be identified.
    if _port_is_occupied():
        reachable = _ping_mcp_rpc()
        print(
            f"ERROR: localhost:{MCP_RPC_PORT} is already in use by "
            f"{'a live MCP RPC server' if reachable else 'another process'}.\n"
            f"       Refusing to start: {REUSE_DISABLED_REASON}.\n"
            "       Stop the existing session, or free the port, and retry.",
            file=sys.stderr,
        )
        return 1

    _ensure_pixi_external_mods(freecad)
    _ensure_mcp_addon_installed()
    _ensure_mcp_auto_start()

    # The port was proven free above, so any server that answers from here on
    # is the one this launcher started.
    previous_mcp_pid = None

    if args.wait:
        process = _start_freecad(freecad, args.args, wait=True)
        assert isinstance(process, subprocess.Popen)
        if not args.no_wait_for_mcp:
            mcp_status = _wait_for_mcp_rpc(
                args.mcp_timeout,
                process=process,
                previous_mcp_pid=previous_mcp_pid,
            )
            if mcp_status != "ready":
                if process.poll() is None:
                    process.terminate()
                if mcp_status == "exited":
                    print(
                        f"ERROR: FreeCAD exited before MCP RPC server responded "
                        f"(exit code {process.returncode}).",
                        file=sys.stderr,
                    )
                else:
                    print(
                        f"ERROR: MCP RPC server did not respond on localhost:{MCP_RPC_PORT} "
                        f"within {args.mcp_timeout:.0f}s",
                        file=sys.stderr,
                    )
                return process.returncode or 1
        if not args.no_wait_for_mcp:
            print(f"MCP RPC server is ready on localhost:{MCP_RPC_PORT}")
        return process.wait()

    process = _start_freecad(freecad, args.args, wait=False)
    assert isinstance(process, subprocess.Popen)
    print(f"Started FreeCAD: {freecad}")

    if args.no_wait_for_mcp:
        return 0

    print(f"Waiting for MCP RPC server on localhost:{MCP_RPC_PORT}...")
    mcp_status = _wait_for_mcp_rpc(
        args.mcp_timeout,
        process=process,
        previous_mcp_pid=previous_mcp_pid,
    )
    if mcp_status != "ready":
        if mcp_status == "exited":
            print(
                f"ERROR: FreeCAD exited before MCP RPC server responded "
                f"(exit code {process.returncode}).",
                file=sys.stderr,
            )
            return process.returncode or 1
        print(
            f"ERROR: MCP RPC server did not respond within {args.mcp_timeout:.0f}s. "
            "Open the FreeCAD MCP workbench and start the RPC server manually if needed.",
            file=sys.stderr,
        )
        return 1

    print(f"MCP RPC server is ready on localhost:{MCP_RPC_PORT}")
    print(
        "Cursor can connect via FreeCAD/tools/mcp/freecad-mcp "
        "(uv run freecad-mcp in your MCP config)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
