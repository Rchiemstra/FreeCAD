"""Deterministic JSON serialization."""

from __future__ import annotations

import hashlib
import json
import math
from typing import Any

from .errors import InvalidSchemaError


def _sort_dict_keys(obj: dict[str, Any]) -> dict[str, Any]:
    return {k: _canonicalize(v) for k, v in sorted(obj.items())}


def _canonicalize(obj: Any) -> Any:
    if isinstance(obj, dict):
        return _sort_dict_keys(obj)
    if isinstance(obj, list):
        return [_canonicalize(v) for v in obj]
    if isinstance(obj, float):
        if math.isnan(obj) or math.isinf(obj):
            raise InvalidSchemaError("cannot serialize non-finite float")
        from .normalize import canonical_decimal

        return canonical_decimal(obj)
    return obj


def semantic_hash(model_dict: dict[str, Any]) -> str:
    """Compute SHA-256 over canonical semantic content before digest field."""
    # Hash everything except source.semantic_sha256
    payload = {
        "document": model_dict.get("document", {}),
        "objects": model_dict.get("objects", {}),
        "dependencies": model_dict.get("dependencies", []),
        "external_references": model_dict.get("external_references", []),
    }
    canonical = _canonicalize(payload)
    data = json.dumps(canonical, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(data.encode("utf-8")).hexdigest()


def serialize_deterministic(data: dict[str, Any]) -> bytes:
    """Serialize to deterministic UTF-8 JSON bytes with LF newlines."""
    canonical = _canonicalize(data)

    # Sort objects by internal name (already keyed, but ensure)
    if "objects" in canonical and isinstance(canonical["objects"], dict):
        canonical["objects"] = {
            k: canonical["objects"][k] for k in sorted(canonical["objects"].keys())
        }

    # Sort dependencies
    if "dependencies" in canonical:
        deps = [tuple(d) for d in canonical["dependencies"]]
        canonical["dependencies"] = [list(d) for d in sorted(set(deps))]

    # Sort external references
    if "external_references" in canonical:
        ext = canonical["external_references"]

        def ext_key(e: dict[str, Any]) -> tuple:
            return (
                e.get("path", ""),
                e.get("document", ""),
                e.get("object", ""),
                e.get("subelement", ""),
                e.get("reference_type", ""),
            )

        canonical["external_references"] = sorted(ext, key=ext_key)

    text = json.dumps(canonical, indent=2, ensure_ascii=False, sort_keys=True)
    # Ensure LF only
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    if not text.endswith("\n"):
        text += "\n"
    return text.encode("utf-8")
