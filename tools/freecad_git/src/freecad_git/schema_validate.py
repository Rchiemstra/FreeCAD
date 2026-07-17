"""JSON Schema validation for sidecars."""

from __future__ import annotations

import json
from importlib import resources
from pathlib import Path
from typing import Any

import jsonschema

from . import __schema__
from .errors import InvalidSchemaError, MalformedSidecarError

_SCHEMA: dict[str, Any] | None = None


def _load_schema() -> dict[str, Any]:
    global _SCHEMA
    if _SCHEMA is not None:
        return _SCHEMA

    # Try package resource first
    try:
        schema_path = resources.files("freecad_git").joinpath("schema/freecad-git-sidecar.schema.json")
        if schema_path.is_file():
            _SCHEMA = json.loads(schema_path.read_text(encoding="utf-8"))
            return _SCHEMA
    except (TypeError, FileNotFoundError, ModuleNotFoundError):
        pass

    # Fallback to development path
    dev_path = Path(__file__).resolve().parents[2] / "schema" / "freecad-git-sidecar.schema.json"
    if dev_path.is_file():
        _SCHEMA = json.loads(dev_path.read_text(encoding="utf-8"))
        return _SCHEMA

    raise InvalidSchemaError("bundled schema not found")


def validate_sidecar_dict(data: dict[str, Any]) -> None:
    """Validate a sidecar dictionary against the bundled schema."""
    schema = _load_schema()

    schema_id = data.get("schema")
    if schema_id != __schema__:
        raise InvalidSchemaError(
            f"unsupported schema identifier: {schema_id!r}, expected {__schema__!r}"
        )

    try:
        jsonschema.validate(instance=data, schema=schema)
    except jsonschema.ValidationError as exc:
        raise MalformedSidecarError(f"schema validation failed: {exc.message}") from exc


def validate_sidecar_bytes(data: bytes) -> dict[str, Any]:
    """Validate sidecar bytes and return parsed dict."""
    try:
        parsed = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise MalformedSidecarError(f"invalid JSON: {exc}") from exc

    if not isinstance(parsed, dict):
        raise MalformedSidecarError("sidecar root must be an object")

    validate_sidecar_dict(parsed)
    return parsed


def validate_sidecar_file(path: Path) -> dict[str, Any]:
    """Validate a sidecar file on disk."""
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise InvalidSchemaError(f"cannot read sidecar: {exc}") from exc
    return validate_sidecar_bytes(data)
