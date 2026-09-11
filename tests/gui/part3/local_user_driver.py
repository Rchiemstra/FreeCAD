# SPDX-License-Identifier: LGPL-2.1-or-later
"""Coordinator-side LocalUserDriver client for the Part 3 control channel."""

from __future__ import annotations

import json
import os
import secrets
import shutil
import subprocess
import uuid
from pathlib import Path
from typing import Any
from urllib import error as urllib_error
from urllib import request as urllib_request

from .local_driver.control_channel import (
    ENDPOINT_DIR_ENV,
    ENDPOINT_FILENAME,
    TOKEN_ENV,
    generate_control_token,
)

REPO_ROOT = Path(__file__).resolve().parents[3]
LOCAL_DRIVER_SOURCE = Path(__file__).resolve().parent / "local_driver"
MOD_DIRNAME = "Part3LocalDriver"
MCP_IMPORT_MOD_DIRNAME = "AddonImportPath"
MCP_SETTINGS_FILENAME = "freecad_mcp_settings.json"
MCP_SECRET_FILENAME = "freecad_mcp_auth.secret"


def _is_reparse_point(path: Path) -> bool:
    try:
        stat_result = path.lstat()
    except OSError:
        return False
    if os.name == "nt":
        file_attribute_reparse_point = 0x400
        return bool(
            getattr(stat_result, "st_file_attributes", 0) & file_attribute_reparse_point
        )
    import stat as _stat

    return _stat.S_ISLNK(stat_result.st_mode)


def _link_points_to(path: Path, source: Path) -> bool:
    try:
        return path.resolve() == source.resolve()
    except OSError:
        return False


def _remove_install(path: Path) -> None:
    if _is_reparse_point(path):
        os.rmdir(path)
    elif path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def install_mcp_addon_import_path(profile_root: Path, repo_root: Path | None = None) -> Path:
    """Install an isolated-profile Mod that prepends the nested MCP root to sys.path.

    FreeCAD's embedded interpreter often ignores process PYTHONPATH, and
    ``pixi run`` may not preserve it. The disposable APPDATA profile still
    loads ``Mod/*/Init.py`` before ``FreeCADMCP``, so baking the absolute
    ``tools/mcp/freecad-mcp`` path here makes ``import addon`` succeed without
    a product-addon redesign.
    """

    source_root = repo_root or REPO_ROOT
    mcp_root = (source_root / "tools" / "mcp" / "freecad-mcp").resolve()
    if not (mcp_root / "addon").is_dir():
        raise FileNotFoundError(f"nested MCP addon root missing: {mcp_root}")
    mod_dir = profile_root / "FreeCAD" / "Mod" / MCP_IMPORT_MOD_DIRNAME
    _remove_install(mod_dir)
    mod_dir.mkdir(parents=True, exist_ok=True)
    init_path = mod_dir / "Init.py"
    init_path.write_text(
        "# Generated for an isolated Part 3 profile only. Not a product Mod.\n"
        "import sys as _sys\n"
        f"_MCP_ROOT = {str(mcp_root)!r}\n"
        "if _MCP_ROOT not in _sys.path:\n"
        "    _sys.path.insert(0, _MCP_ROOT)\n",
        encoding="utf-8",
        newline="\n",
    )
    return mod_dir


def install_part3_local_driver(profile_root: Path, repo_root: Path | None = None) -> Path:
    """Runtime-install Mod/Part3LocalDriver into an isolated profile only."""

    source = (repo_root or REPO_ROOT) / "tests" / "gui" / "part3" / "local_driver"
    if not source.is_dir():
        raise FileNotFoundError(f"local driver source missing: {source}")
    mod_dir = profile_root / "FreeCAD" / "Mod" / MOD_DIRNAME
    if _is_reparse_point(mod_dir) and _link_points_to(mod_dir, source):
        return mod_dir
    _remove_install(mod_dir)
    mod_dir.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.symlink(source, mod_dir, target_is_directory=True)
        return mod_dir
    except OSError:
        pass
    if os.name == "nt":
        result = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(mod_dir), str(source)],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode == 0:
            return mod_dir
    shutil.copytree(source, mod_dir)
    return mod_dir


def read_endpoint(endpoint_dir: Path) -> dict[str, Any]:
    endpoint_path = endpoint_dir / ENDPOINT_FILENAME
    payload = json.loads(endpoint_path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("endpoint.json must contain an object")
    if not payload.get("ready"):
        raise ValueError("endpoint.json is not ready")
    return payload


class LocalUserDriver:
    """Authenticated client for the in-process Part 3 control channel."""

    def __init__(self, token: str, endpoint: dict[str, Any]) -> None:
        self._token = token
        host = str(endpoint.get("host") or "127.0.0.1")
        port = int(endpoint["port"])
        path = str(endpoint.get("path") or "/action")
        self._url = f"http://{host}:{port}{path}"

    @classmethod
    def from_endpoint_file(cls, token: str, endpoint_dir: Path) -> LocalUserDriver:
        return cls(token, read_endpoint(endpoint_dir))

    def invoke(
        self,
        action: str,
        params: dict[str, Any] | None = None,
        *,
        operation_id: str | None = None,
        timeout: float = 30.0,
    ) -> dict[str, Any]:
        op_id = operation_id or str(uuid.uuid4())
        payload = {
            "token": self._token,
            "operation_id": op_id,
            "action": action,
            "params": params or {},
        }
        request = urllib_request.Request(
            self._url,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib_request.urlopen(request, timeout=timeout) as response:
                body = json.loads(response.read().decode("utf-8"))
        except urllib_error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            try:
                body = json.loads(detail)
            except json.JSONDecodeError as decode_error:
                raise RuntimeError(f"HTTP {exc.code}: {detail}") from decode_error
        if not isinstance(body, dict):
            raise RuntimeError(f"unexpected response type: {type(body).__name__}")
        if body.get("operation_id") != op_id:
            raise RuntimeError(
                f"operation_id mismatch: sent {op_id!r}, got {body.get('operation_id')!r}"
            )
        if not body.get("success"):
            raise RuntimeError(str(body.get("error") or "action failed"))
        return body

    def preflight(self) -> dict[str, Any]:
        response = self.invoke("preflight")
        return response.get("result") or {}


def _provision_disposable_auth_secret(secret_path: Path) -> None:
    """Create or repair the profile auth secret with owner-only POSIX mode.

    Mirrors ``create_profile_secret`` / ``setup_isolated_profile._ensure_auth_secret``
    so disposable profiles match real isolated-profile provisioning.
    """

    secret_path.parent.mkdir(parents=True, exist_ok=True)
    if secret_path.is_file():
        try:
            size = secret_path.stat().st_size
        except OSError:
            size = -1
        if size == 32:
            try:
                os.chmod(secret_path, 0o600)
            except OSError:
                pass
            return
        secret_path.unlink()

    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_BINARY"):
        flags |= os.O_BINARY
    descriptor = os.open(secret_path, flags, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(secrets.token_bytes(32))
            handle.flush()
            os.fsync(handle.fileno())
        if os.name != "nt":
            os.chmod(secret_path, 0o600)
    except Exception:
        try:
            secret_path.unlink()
        except OSError:
            pass
        raise


def ensure_disposable_profile_auth_secret(profile_root: Path) -> Path:
    """Create the 32-byte MCP handshake secret in a disposable isolated profile.

    ``setup_isolated_profile.py`` does this for the persistent isolated instance.
    Disposable WP04 profiles only redirected APPDATA, so handshake waited on a
    file the addon never auto-creates (ensure_profile_secret is not called on
    import).
    """

    fc_data = profile_root / "FreeCAD"
    fc_data.mkdir(parents=True, exist_ok=True)
    secret_path = fc_data / MCP_SECRET_FILENAME
    _provision_disposable_auth_secret(secret_path)
    settings_path = fc_data / MCP_SETTINGS_FILENAME
    settings: dict[str, Any] = {}
    if settings_path.is_file():
        try:
            loaded = json.loads(settings_path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                settings.update(loaded)
        except (OSError, json.JSONDecodeError):
            settings = {}
    settings["auto_start_rpc"] = True
    settings["remote_enabled"] = False
    settings["allowed_ips"] = "127.0.0.1"
    settings["auth_secret_file"] = str(secret_path)
    if not settings.get("profile_instance_id"):
        settings["profile_instance_id"] = str(uuid.uuid4())
    settings_path.write_text(
        json.dumps(settings, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return secret_path


def launch_env_for_isolated_profile(
    profile_root: Path,
    *,
    control_token: str,
    endpoint_dir: Path,
    repo_root: Path | None = None,
) -> dict[str, str]:
    fc_data = profile_root / "FreeCAD"
    config_root = profile_root / "config"
    cache_root = profile_root / "cache"
    fc_data.mkdir(parents=True, exist_ok=True)
    config_root.mkdir(parents=True, exist_ok=True)
    cache_root.mkdir(parents=True, exist_ok=True)
    (fc_data / "temp").mkdir(exist_ok=True)
    endpoint_dir.mkdir(parents=True, exist_ok=True)
    install_part3_local_driver(profile_root, repo_root)
    install_mcp_addon_import_path(profile_root, repo_root)
    ensure_disposable_profile_auth_secret(profile_root)
    env = os.environ.copy()
    env["HOME"] = str(profile_root)
    env["APPDATA"] = str(profile_root)
    env["XDG_CONFIG_HOME"] = str(config_root)
    env["XDG_CACHE_HOME"] = str(cache_root)
    env["XDG_DATA_HOME"] = str(profile_root)
    env["FREECAD_USER_HOME"] = str(fc_data)
    env["FREECAD_USER_DATA"] = str(fc_data)
    env["FREECAD_USER_TEMP"] = str(fc_data / "temp")
    env["FREECAD_REPO"] = str(repo_root or REPO_ROOT)
    nested_root = (repo_root or REPO_ROOT) / "tools" / "mcp" / "freecad-mcp"
    existing_pythonpath = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = (
        str(nested_root)
        + (os.pathsep + existing_pythonpath if existing_pythonpath else "")
    )
    env[TOKEN_ENV] = control_token
    env[ENDPOINT_DIR_ENV] = str(endpoint_dir)
    return env


def wait_for_endpoint(endpoint_dir: Path, timeout_s: float = 120.0) -> dict[str, Any]:
    """Poll endpoint.json without importing sleep into the in-process driver."""

    import time

    deadline = time.monotonic() + timeout_s
    endpoint_path = endpoint_dir / ENDPOINT_FILENAME
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if endpoint_path.is_file():
            try:
                return read_endpoint(endpoint_dir)
            except (OSError, ValueError, json.JSONDecodeError) as exc:
                last_error = exc
        time.sleep(0.05)
    if last_error is not None:
        raise TimeoutError(
            f"endpoint.json not ready within {timeout_s}s: {last_error}"
        ) from last_error
    raise TimeoutError(f"endpoint.json not ready within {timeout_s}s")


__all__ = [
    "ENDPOINT_DIR_ENV",
    "LOCAL_DRIVER_SOURCE",
    "LocalUserDriver",
    "TOKEN_ENV",
    "generate_control_token",
    "ensure_disposable_profile_auth_secret",
    "install_mcp_addon_import_path",
    "install_part3_local_driver",
    "launch_env_for_isolated_profile",
    "read_endpoint",
    "wait_for_endpoint",
]
