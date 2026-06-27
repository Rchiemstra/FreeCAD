"""
Load and verify reproducibility pins from config/runtime-versions.lock.yaml.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

_LOCK_REL = Path("config") / "runtime-versions.lock.yaml"


def repo_root(start: Optional[Path] = None) -> Path:
    return (start or Path.cwd()).resolve()


def lock_path(root: Optional[Path] = None) -> Path:
    return repo_root(root) / _LOCK_REL


def load_runtime_lock(root: Optional[Path] = None) -> Dict[str, Any]:
    path = lock_path(root)
    if not path.is_file():
        raise FileNotFoundError(f"Runtime lock missing: {path}")
    import yaml  # type: ignore

    data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    if not isinstance(data, dict):
        raise ValueError(f"{path} must be a YAML mapping")
    return data


def fcstd_spec(lock: Optional[Dict[str, Any]] = None, *, root: Optional[Path] = None) -> Dict[str, Any]:
    data = lock if lock is not None else load_runtime_lock(root)
    spec = data.get("robot_source") or {}
    if not isinstance(spec, dict):
        raise ValueError("robot_source must be a mapping in runtime lock")
    return spec


def fcstd_path(root: Optional[Path] = None, lock: Optional[Dict[str, Any]] = None) -> Path:
    spec = fcstd_spec(lock, root=root)
    rel = spec.get("fcstd_path") or "robots/arm_2dof.FCStd"
    return repo_root(root) / str(rel)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_fcstd(
    root: Optional[Path] = None,
    *,
    lock: Optional[Dict[str, Any]] = None,
) -> tuple[bool, str]:
    """
    Return ``(ok, message)`` comparing ``robots/arm_2dof.FCStd`` to the lock file.
    """
    base = repo_root(root)
    spec = fcstd_spec(lock, root=base)
    path = fcstd_path(base, lock=lock)
    expected = str(spec.get("fcstd_sha256", "")).lower().strip()
    if not expected:
        return False, "fcstd_sha256 missing in runtime lock"

    if not path.is_file():
        url = str(spec.get("ci_artifact_url") or "").strip()
        hint = (
            f"Fetch with: ROBOTS_ARM_2DOF_FCSTD_URL=<url> bash e2e/fetch_robot_source.sh"
            if url
            else "Commit robots/arm_2dof.FCStd or set ROBOTS_ARM_2DOF_FCSTD_URL"
        )
        return False, f"Robot source missing: {path} ({hint})"

    digest = sha256_file(path)
    if digest != expected:
        return False, (
            f"SHA-256 mismatch for {path}: got {digest}, expected {expected}"
        )

    expected_bytes = spec.get("fcstd_bytes")
    if expected_bytes is not None:
        size = path.stat().st_size
        if int(expected_bytes) != size:
            return False, f"Size mismatch for {path}: got {size} bytes, expected {expected_bytes}"

    return True, f"OK {path.name} sha256={digest[:16]}..."


def version_check_strict() -> bool:
    import os

    return os.environ.get("E2E_VERSION_STRICT", "1").strip().lower() in (
        "1",
        "true",
        "yes",
    )


def read_builtin_base_image_ref() -> Optional[str]:
    path = Path("/etc/e2e-base-image.ref")
    if path.is_file():
        return path.read_text(encoding="utf-8").strip()
    return None


def query_apt_versions(packages: List[str]) -> Dict[str, str]:
    import subprocess

    if not packages:
        return {}
    try:
        proc = subprocess.run(
            ["dpkg-query", "-W", "-f=${Package}=${Version}\n", *packages],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except Exception:
        return {}
    out: Dict[str, str] = {}
    for line in (proc.stdout or "").splitlines():
        if "=" in line:
            name, ver = line.split("=", 1)
            out[name.strip()] = ver.strip()
    return out


def locked_apt_versions(lock: Optional[Dict[str, Any]] = None) -> Dict[str, str]:
    data = lock if lock is not None else load_runtime_lock()
    apt = (data.get("docker_e2e") or {}).get("apt_versions") or {}
    if not isinstance(apt, dict):
        return {}
    return {str(k): str(v) for k, v in apt.items()}


def compare_observed_to_lock(
    observed: Dict[str, Any],
    lock: Optional[Dict[str, Any]] = None,
) -> Tuple[List[str], List[str]]:
    """
    Return ``(warnings, errors)`` comparing an E2E observation dict to the lock file.
    """
    data = lock if lock is not None else load_runtime_lock()
    warnings: List[str] = []
    errors: List[str] = []
    docker = data.get("docker_e2e") or {}
    pypi = data.get("pypi") or {}

    expected_digest = str(docker.get("base_image_digest") or "").strip()
    observed_ref = observed.get("docker_base_image_ref")
    builtin_ref = observed.get("docker_base_image_builtin")
    if expected_digest:
        for label, ref in (
            ("observed", observed_ref),
            ("builtin", builtin_ref),
        ):
            if ref and expected_digest not in str(ref):
                errors.append(
                    f"base image {label} {ref!r} does not include lock digest {expected_digest}"
                )

    apt_observed = observed.get("apt_versions") or {}
    if not isinstance(apt_observed, dict):
        apt_observed = {}
    for pkg, want in locked_apt_versions(data).items():
        got = apt_observed.get(pkg)
        if not got:
            errors.append(f"apt package {pkg} not reported (expected {want})")
        elif got != want:
            errors.append(f"apt {pkg}={got} != lock {want}")

    if observed.get("gazebo_sim") and docker.get("gazebo_sim_version"):
        if observed["gazebo_sim"] != docker["gazebo_sim_version"]:
            warnings.append(
                f"gazebo_sim CLI {observed['gazebo_sim']} != lock {docker['gazebo_sim_version']}"
            )

    if observed.get("freecad") and docker.get("freecad_version"):
        if not str(observed["freecad"]).startswith(str(docker["freecad_version"])):
            warnings.append(
                f"freecad CLI {observed['freecad']} != lock prefix {docker['freecad_version']}"
            )

    rc = observed.get("robotcad_commit")
    expected_rc = (docker.get("robotcad") or {}).get("commit")
    if rc and expected_rc and rc != expected_rc:
        errors.append(f"robotcad commit {rc[:12]} != lock {expected_rc[:12]}")

    for pkg, lock_key in (("mcp", "mcp"), ("pydantic", "pydantic"), ("pyyaml", "pyyaml")):
        got = (observed.get("mcp_venv") or {}).get(pkg)
        want = pypi.get(lock_key)
        if got and want and got != want:
            errors.append(f"pypi {pkg} {got} != lock {want}")

    rs = observed.get("robot_source") or {}
    spec = data.get("robot_source") or {}
    if rs.get("sha256") and spec.get("fcstd_sha256"):
        if str(rs["sha256"]).lower() != str(spec["fcstd_sha256"]).lower():
            errors.append("robot FCStd sha256 != lock")
    if rs.get("ok") is False:
        errors.append(f"robot source: {rs.get('verify', 'verify failed')}")

    return warnings, errors
