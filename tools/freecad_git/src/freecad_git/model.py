"""Canonical semantic model structures."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class SemanticModel:
    """In-memory canonical semantic representation of a FreeCAD document."""

    document: dict[str, Any] = field(default_factory=dict)
    objects: dict[str, dict[str, Any]] = field(default_factory=dict)
    dependencies: list[list[str]] = field(default_factory=list)
    external_references: list[dict[str, Any]] = field(default_factory=list)
    source_filename: str = ""

    def to_sidecar_dict(
        self,
        generator_name: str,
        generator_version: str,
        profile: str,
        semantic_sha256: str,
    ) -> dict[str, Any]:
        return {
            "schema": "freecad-git-sidecar/v1",
            "generator": {
                "name": generator_name,
                "version": generator_version,
                "profile": profile,
            },
            "source": {
                "filename": self.source_filename,
                "semantic_sha256": semantic_sha256,
            },
            "document": self.document,
            "objects": self.objects,
            "dependencies": self.dependencies,
            "external_references": self.external_references,
        }
