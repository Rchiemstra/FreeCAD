"""Main export pipeline from FCStd to sidecar JSON."""

from __future__ import annotations

import hashlib
import os
import tempfile
import uuid
from pathlib import Path
from typing import Any

from . import __schema__, __version__
from .archive import SafeArchive, validate_and_open
from .config import Config
from .document_xml import parse_document_xml
from .errors import (
    FreecadGitError,
    InvalidSchemaError,
    IOError,
    MalformedSidecarError,
    MissingSidecarError,
    StaleSidecarError,
    UnsupportedDocumentError,
)
from .model import SemanticModel
from .properties import parse_object_properties
from .schema_validate import validate_sidecar_bytes, validate_sidecar_dict
from .serialize import semantic_hash, serialize_deterministic


def _build_document_info(parser: Any, source_filename: str) -> dict[str, Any]:
    attrs = parser.document_attrs
    info: dict[str, Any] = {
        "name": attrs.get("name") or attrs.get("label") or "Unnamed",
        "label": attrs.get("label", attrs.get("name") or "Unnamed"),
    }
    if "ProgramVersion" in attrs:
        info["freecad_version"] = attrs["ProgramVersion"]
    if "FileVersion" in attrs:
        info["file_version"] = attrs["FileVersion"]
    if "SchemaVersion" in attrs:
        info["schema_version"] = attrs["SchemaVersion"]
    return info


def _extract_dependencies(objects: dict[str, dict[str, Any]]) -> list[list[str]]:
    """Build dependency edges from local links. Direction: source -> target."""
    deps: set[tuple[str, str]] = set()
    for obj_name, obj_data in objects.items():
        local_links = obj_data.get("local_links", [])
        for target in local_links:
            if target and target != obj_name:
                deps.add((obj_name, target))
        membership = obj_data.get("membership", {})
        for key in ("body", "group", "origin", "tip"):
            val = membership.get(key)
            if isinstance(val, str) and val:
                deps.add((obj_name, val))
            elif isinstance(val, list):
                for v in val:
                    deps.add((obj_name, v))
    return [list(d) for d in sorted(deps)]


def _extract_external_references(objects: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    refs: list[dict[str, Any]] = []
    seen: set[tuple] = set()
    for obj_name, obj_data in objects.items():
        for ext in obj_data.get("external_links", []):
            entry = {
                "source_object": obj_name,
                "path": ext.get("path", ""),
                "relative": ext.get("relative", False),
                "absolute": ext.get("absolute", False),
                "document": ext.get("document", ""),
                "object": ext.get("object", ""),
                "subelement": ext.get("subelement", ""),
                "reference_type": "external_link",
            }
            key = (
                obj_name,
                entry.get("path", ""),
                entry.get("document", ""),
                entry.get("object", ""),
                entry.get("subelement", ""),
            )
            if key not in seen:
                seen.add(key)
                refs.append(entry)
    return sorted(refs, key=lambda e: (e.get("source_object", ""), e.get("path", ""), e.get("object", "")))


def build_semantic_model(
    archive: SafeArchive,
    config: Config,
    source_filename: str,
) -> SemanticModel:
    """Build canonical semantic model from validated archive."""
    if archive.document_xml is None:
        raise UnsupportedDocumentError("Document.xml not available")

    parser = parse_document_xml(
        archive.document_xml,
        config.xml,
        config.collections,
    )

    model = SemanticModel(source_filename=source_filename)
    model.document = _build_document_info(parser, source_filename)

    for obj_name in sorted(parser.object_types.keys()):
        obj_type = parser.object_types[obj_name]
        obj_data_raw = parser.object_data.get(obj_name, {})
        properties = obj_data_raw.get("properties", [])

        type_exclude = frozenset(config.property_exclude.get(obj_type, []))
        type_include = frozenset(config.property_include.get(obj_type, []))

        parsed = parse_object_properties(
            properties,
            obj_type,
            config.collections,
            config.external_reference_policy,
            type_exclude,
            type_include,
        )

        obj_entry: dict[str, Any] = {"type": obj_type}
        if "label" in parsed:
            obj_entry["label"] = parsed.pop("label")
        else:
            obj_entry["label"] = obj_name
        obj_entry.update(parsed)
        model.objects[obj_name] = obj_entry

    model.dependencies = _extract_dependencies(model.objects)
    if len(model.dependencies) > config.collections.max_dependencies:
        raise UnsupportedDocumentError("dependency count exceeds limit")

    model.external_references = _extract_external_references(model.objects)

    return model


def export_to_dict(
    fcstd_path: Path,
    config: Config | None = None,
) -> dict[str, Any]:
    """Export FCStd to sidecar dictionary."""
    config = config or Config()
    fcstd_path = fcstd_path.resolve()
    archive = validate_and_open(fcstd_path, config.archive)
    model = build_semantic_model(archive, config, fcstd_path.name)

    # Build without hash first
    partial = model.to_sidecar_dict("freecad-git", __version__, config.profile, "")
    digest = semantic_hash(partial)
    return model.to_sidecar_dict("freecad-git", __version__, config.profile, digest)


def export_to_bytes(
    fcstd_path: Path,
    config: Config | None = None,
) -> bytes:
    """Export FCStd to deterministic JSON bytes."""
    data = export_to_dict(fcstd_path, config)
    validate_sidecar_dict(data)
    result = serialize_deterministic(data)

    cfg = config or Config()
    if cfg.sidecar.size_limit_bytes and len(result) > cfg.sidecar.size_limit_bytes:
        raise UnsupportedDocumentError(
            f"sidecar exceeds size limit: {len(result)} > {cfg.sidecar.size_limit_bytes}"
        )
    return result


def sidecar_path_for(fcstd_path: Path) -> Path:
    """Return expected sidecar path for an FCStd file."""
    return fcstd_path.parent / f"{fcstd_path.name}.git.json"


def write_sidecar_atomic(fcstd_path: Path, data: bytes) -> bool:
    """Write sidecar atomically. Returns True if written, False if unchanged."""
    output_path = sidecar_path_for(fcstd_path)
    if output_path.exists():
        existing = output_path.read_bytes()
        if existing == data:
            return False

    tmp_name = f"{output_path.name}.tmp-{uuid.uuid4().hex}"
    tmp_path = output_path.parent / tmp_name
    try:
        tmp_path.write_bytes(data)
        # Validate written file
        validate_sidecar_bytes(data)
        os.replace(tmp_path, output_path)
    except Exception:
        if tmp_path.exists():
            tmp_path.unlink()
        raise
    return True


def export_file(
    fcstd_path: Path,
    config: Config | None = None,
    stdout: bool = False,
) -> bytes:
    """Export a single FCStd file. Returns generated bytes."""
    data = export_to_bytes(fcstd_path, config)
    if not stdout:
        write_sidecar_atomic(fcstd_path, data)
    return data


def check_file(fcstd_path: Path, config: Config | None = None) -> None:
    """Verify sidecar matches expected output."""
    fcstd_path = fcstd_path.resolve()
    sidecar = sidecar_path_for(fcstd_path)

    expected = export_to_bytes(fcstd_path, config)

    if not sidecar.exists():
        raise MissingSidecarError(f"Missing sidecar: {sidecar}")

    try:
        actual = sidecar.read_bytes()
    except OSError as exc:
        raise IOError(f"cannot read sidecar: {exc}") from exc

    if actual != expected:
        raise StaleSidecarError(
            f"Stale sidecar: {sidecar}\n\n"
            f"Run:\n"
            f"  freecad-git export {fcstd_path}\n"
            f"  git add {fcstd_path} {sidecar}"
        )

    validate_sidecar_bytes(actual)


def check_all(config: Config) -> list[str]:
    """Check all discovered FCStd files. Returns list of error messages."""
    errors: list[str] = []
    files = __import__("freecad_git.config", fromlist=["discover_fcstd_files"]).discover_fcstd_files(
        config, config.repo_root
    )
    for path in files:
        try:
            check_file(path, config)
        except FreecadGitError as exc:
            errors.append(exc.message)
    return errors
