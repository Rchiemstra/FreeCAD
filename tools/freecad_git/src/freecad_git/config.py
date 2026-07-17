"""Configuration loading and validation for .freecad-git.toml."""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .errors import InvalidConfigError, IOError as FreecadGitIOError

if sys.version_info >= (3, 11):
    import tomllib
else:
    import tomli as tomllib  # type: ignore[no-redef]

KNOWN_TOP_LEVEL_KEYS = frozenset(
    {
        "include",
        "exclude",
        "fixture_exclude",
        "profile",
        "property_include",
        "property_exclude",
        "external_reference_policy",
        "archive",
        "xml",
        "collections",
        "diagnostics",
        "sidecar",
        "freecad_cmd",
        "skip_directories",
    }
)

KNOWN_ARCHIVE_KEYS = frozenset(
    {
        "max_entries",
        "max_compressed_bytes",
        "max_uncompressed_bytes",
        "max_xml_bytes",
        "max_compression_ratio",
    }
)

KNOWN_XML_KEYS = frozenset(
    {
        "max_depth",
        "max_attributes",
        "max_text_bytes",
        "max_properties_per_object",
    }
)

KNOWN_COLLECTION_KEYS = frozenset(
    {
        "max_list_length",
        "max_string_length",
        "max_objects",
        "max_dependencies",
    }
)

KNOWN_DIAGNOSTIC_KEYS = frozenset({"max_runtime_seconds", "freecad_cmd"})
KNOWN_SIDECAR_KEYS = frozenset({"size_warning_bytes", "size_limit_bytes"})

_DEFAULT_SKIP_DIRS = frozenset(
    {
        ".git",
        ".svn",
        ".hg",
        "build",
        "build-debug",
        "build-release",
        "cmake-build-debug",
        "cmake-build-release",
        "node_modules",
        ".venv",
        ".tox",
        "__pycache__",
        "bin",
        "lib",
    }
)


@dataclass(frozen=True)
class ArchiveLimits:
    max_entries: int = 10_000
    max_compressed_bytes: int = 256 * 1024 * 1024
    max_uncompressed_bytes: int = 512 * 1024 * 1024
    max_xml_bytes: int = 64 * 1024 * 1024
    max_compression_ratio: float = 100.0


@dataclass(frozen=True)
class XmlLimits:
    max_depth: int = 256
    max_attributes: int = 1024
    max_text_bytes: int = 16 * 1024 * 1024
    max_properties_per_object: int = 10_000


@dataclass(frozen=True)
class CollectionLimits:
    max_list_length: int = 100_000
    max_string_length: int = 1_000_000
    max_objects: int = 50_000
    max_dependencies: int = 500_000


@dataclass(frozen=True)
class DiagnosticConfig:
    max_runtime_seconds: int = 120
    freecad_cmd: str | None = None


@dataclass(frozen=True)
class SidecarConfig:
    size_warning_bytes: int = 500_000
    size_limit_bytes: int | None = None


@dataclass
class Config:
    """Resolved freecad-git configuration."""

    config_path: Path | None = None
    repo_root: Path = field(default_factory=Path.cwd)
    include: list[str] = field(default_factory=list)
    exclude: list[str] = field(
        default_factory=lambda: [
            "**/tests/**",
            "**/fixtures/**",
            "src/**",
            "data/examples/**",
            "build/**",
            "build-*/**",
            "cmake-build*/**",
            ".git/**",
            "src/3rdParty/**",
            "src/Mod/**/data/**",
            "src/Mod/**/Test/**",
            "src/Mod/AddonManager/**",
        ]
    )
    fixture_exclude: list[str] = field(default_factory=lambda: ["**/tests/**", "**/fixtures/**"])
    skip_directories: frozenset[str] = field(default_factory=lambda: set(_DEFAULT_SKIP_DIRS))
    profile: str = "semantic"
    property_include: dict[str, list[str]] = field(default_factory=dict)
    property_exclude: dict[str, list[str]] = field(default_factory=dict)
    external_reference_policy: str = "redact"
    archive: ArchiveLimits = field(default_factory=ArchiveLimits)
    xml: XmlLimits = field(default_factory=XmlLimits)
    collections: CollectionLimits = field(default_factory=CollectionLimits)
    diagnostics: DiagnosticConfig = field(default_factory=DiagnosticConfig)
    sidecar: SidecarConfig = field(default_factory=SidecarConfig)
    freecad_cmd: str | None = None


def _validate_keys(section: str, data: dict[str, Any], known: frozenset[str]) -> None:
    for key in data:
        if key not in known:
            raise InvalidConfigError(f"unknown {section} key: {key}")


def _load_section_limits(
    data: dict[str, Any],
    known: frozenset[str],
    defaults: dict[str, Any],
    section: str,
) -> dict[str, Any]:
    if section not in data:
        return dict(defaults)
    section_data = data[section]
    if not isinstance(section_data, dict):
        raise InvalidConfigError(f"{section} must be a table")
    _validate_keys(section, section_data, known)
    result = dict(defaults)
    for key, value in section_data.items():
        if not isinstance(value, (int, float)):
            raise InvalidConfigError(f"{section}.{key} must be a number")
        result[key] = value
    return result


def load_config(config_path: Path | None = None, repo_root: Path | None = None) -> Config:
    """Load configuration from .freecad-git.toml or return defaults."""
    root = (repo_root or Path.cwd()).resolve()
    cfg = Config(repo_root=root)

    if config_path is None:
        candidate = root / ".freecad-git.toml"
        if candidate.is_file():
            config_path = candidate

    if config_path is None:
        return cfg

    if not config_path.is_file():
        raise InvalidConfigError(f"configuration file not found: {config_path}")

    with config_path.open("rb") as fh:
        data = tomllib.load(fh)

    if not isinstance(data, dict):
        raise InvalidConfigError("configuration root must be a table")

    _validate_keys("root", data, KNOWN_TOP_LEVEL_KEYS)

    cfg.config_path = config_path.resolve()

    for key in ("include", "exclude", "fixture_exclude"):
        if key in data:
            value = data[key]
            if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
                raise InvalidConfigError(f"{key} must be a list of strings")
            setattr(cfg, key, list(value))

    if "skip_directories" in data:
        value = data["skip_directories"]
        if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
            raise InvalidConfigError("skip_directories must be a list of strings")
        cfg.skip_directories = frozenset(value)

    if "profile" in data:
        if not isinstance(data["profile"], str):
            raise InvalidConfigError("profile must be a string")
        cfg.profile = data["profile"]

    for key in ("property_include", "property_exclude"):
        if key in data:
            value = data[key]
            if not isinstance(value, dict):
                raise InvalidConfigError(f"{key} must be a table")
            parsed: dict[str, list[str]] = {}
            for obj_type, props in value.items():
                if not isinstance(props, list) or not all(isinstance(p, str) for p in props):
                    raise InvalidConfigError(f"{key}.{obj_type} must be a list of strings")
                parsed[str(obj_type)] = list(props)
            setattr(cfg, key, parsed)

    if "external_reference_policy" in data:
        policy = data["external_reference_policy"]
        if policy not in ("redact", "hash", "reject"):
            raise InvalidConfigError(
                "external_reference_policy must be 'redact', 'hash', or 'reject'"
            )
        cfg.external_reference_policy = policy

    archive_data = _load_section_limits(
        data,
        KNOWN_ARCHIVE_KEYS,
        {
            "max_entries": 10_000,
            "max_compressed_bytes": 256 * 1024 * 1024,
            "max_uncompressed_bytes": 512 * 1024 * 1024,
            "max_xml_bytes": 64 * 1024 * 1024,
            "max_compression_ratio": 100.0,
        },
        "archive",
    )
    cfg.archive = ArchiveLimits(**archive_data)

    xml_data = _load_section_limits(
        data,
        KNOWN_XML_KEYS,
        {
            "max_depth": 256,
            "max_attributes": 1024,
            "max_text_bytes": 16 * 1024 * 1024,
            "max_properties_per_object": 10_000,
        },
        "xml",
    )
    cfg.xml = XmlLimits(**xml_data)

    coll_data = _load_section_limits(
        data,
        KNOWN_COLLECTION_KEYS,
        {
            "max_list_length": 100_000,
            "max_string_length": 1_000_000,
            "max_objects": 50_000,
            "max_dependencies": 500_000,
        },
        "collections",
    )
    cfg.collections = CollectionLimits(**coll_data)

    if "diagnostics" in data:
        diag = data["diagnostics"]
        if not isinstance(diag, dict):
            raise InvalidConfigError("diagnostics must be a table")
        _validate_keys("diagnostics", diag, KNOWN_DIAGNOSTIC_KEYS)
        max_runtime = diag.get("max_runtime_seconds", 120)
        if not isinstance(max_runtime, int):
            raise InvalidConfigError("diagnostics.max_runtime_seconds must be an integer")
        freecad_cmd = diag.get("freecad_cmd")
        if freecad_cmd is not None and not isinstance(freecad_cmd, str):
            raise InvalidConfigError("diagnostics.freecad_cmd must be a string")
        cfg.diagnostics = DiagnosticConfig(
            max_runtime_seconds=max_runtime,
            freecad_cmd=freecad_cmd,
        )

    if "sidecar" in data:
        sidecar = data["sidecar"]
        if not isinstance(sidecar, dict):
            raise InvalidConfigError("sidecar must be a table")
        _validate_keys("sidecar", sidecar, KNOWN_SIDECAR_KEYS)
        warning = sidecar.get("size_warning_bytes", 500_000)
        limit = sidecar.get("size_limit_bytes")
        if not isinstance(warning, int):
            raise InvalidConfigError("sidecar.size_warning_bytes must be an integer")
        if limit is not None and not isinstance(limit, int):
            raise InvalidConfigError("sidecar.size_limit_bytes must be an integer")
        cfg.sidecar = SidecarConfig(size_warning_bytes=warning, size_limit_bytes=limit)

    if "freecad_cmd" in data:
        if not isinstance(data["freecad_cmd"], str):
            raise InvalidConfigError("freecad_cmd must be a string")
        cfg.freecad_cmd = data["freecad_cmd"]

    return cfg


def _matches_glob(rel: str, glob: str) -> bool:
    import fnmatch

    if fnmatch.fnmatch(rel, glob):
        return True
    if glob.startswith("**/"):
        tail = glob[3:]
        if fnmatch.fnmatch(rel, tail):
            return True
        if fnmatch.fnmatch(Path(rel).name, tail):
            return True
    return False


def _matches_any(rel: str, globs: list[str]) -> bool:
    return any(_matches_glob(rel, g) for g in globs)


def _should_skip_dir(name: str, skip_dirs: frozenset[str]) -> bool:
    low = name.lower()
    if low in skip_dirs:
        return True
    if low.startswith("build") or low.startswith("cmake-build"):
        return True
    return False


def discover_fcstd_files(cfg: Config, root: Path | None = None) -> list[Path]:
    """Discover .FCStd files in the managed scope without unsafe traversal."""
    if not cfg.include:
        return []

    base = (root or cfg.repo_root).resolve()
    patterns = cfg.include
    excludes = list(cfg.exclude) + list(cfg.fixture_exclude)
    skip_dirs = cfg.skip_directories

    found: list[Path] = []

    def onerror(exc: OSError) -> None:
        raise FreecadGitIOError(f"cannot access path during discovery: {exc}") from exc

    for dirpath, dirnames, filenames in os.walk(base, topdown=True, onerror=onerror):
        dirnames[:] = [
            d
            for d in dirnames
            if not _should_skip_dir(d, skip_dirs) and d not in (".git",)
        ]
        for filename in filenames:
            if not filename.lower().endswith(".fcstd"):
                continue
            path = Path(dirpath, filename).resolve()
            try:
                rel = str(path.relative_to(base)).replace("\\", "/")
            except ValueError:
                rel = str(path).replace("\\", "/")
            if not _matches_any(rel, patterns):
                continue
            if _matches_any(rel, excludes):
                continue
            found.append(path)

    return sorted(found)
