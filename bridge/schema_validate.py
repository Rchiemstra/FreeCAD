"""
JSON Schema validation for project.yaml, scenario YAML, and result.yaml.

Schemas live in config/schemas/*.schema.yaml (draft-07, YAML syntax).
Requires PyYAML; uses jsonschema when installed (see requirements-bridge.txt).
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, List, Optional

_REPO = Path(__file__).resolve().parent.parent
_SCHEMA_DIR = _REPO / "config" / "schemas"


class SchemaValidationError(ValueError):
    """Raised when instance data fails schema validation."""


def schema_path(name: str) -> Path:
    return _SCHEMA_DIR / f"{name}.schema.yaml"


def load_schema(name: str) -> dict[str, Any]:
    import yaml  # type: ignore

    path = schema_path(name)
    if not path.is_file():
        raise FileNotFoundError(f"Schema not found: {path}")
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{path} must be a mapping")
    return data


def validate_instance(
    data: Any,
    schema_name: str,
    *,
    instance_label: str = "document",
) -> None:
    """
    Validate ``data`` against ``config/schemas/<schema_name>.schema.yaml``.

    Raises :class:`SchemaValidationError` on failure.
    """
    errors = validate_instance_errors(data, schema_name, instance_label=instance_label)
    if errors:
        raise SchemaValidationError(
            f"{instance_label} failed {schema_name} schema validation:\n"
            + "\n".join(f"  - {e}" for e in errors)
        )


def validate_instance_errors(
    data: Any,
    schema_name: str,
    *,
    instance_label: str = "document",
) -> List[str]:
    try:
        import jsonschema  # type: ignore
    except ImportError:
        return []

    schema = load_schema(schema_name)
    validator = jsonschema.Draft7Validator(schema)
    errors: List[str] = []
    for err in sorted(validator.iter_errors(data), key=lambda e: list(e.path)):
        loc = ".".join(str(p) for p in err.path) or "(root)"
        errors.append(f"{loc}: {err.message}")
    return errors
