"""Normalization unit tests."""

from __future__ import annotations

import pytest

from freecad_git.errors import InvalidSchemaError
from freecad_git.normalize import (
    canonical_decimal,
    is_excluded_property,
    normalize_expression,
    normalize_path,
    normalize_quaternion,
    safe_external_path,
)


class TestNormalization:
    def test_canonical_decimal_variants(self):
        assert canonical_decimal(1.0) == "1"
        assert canonical_decimal("0.100") == "0.1"
        assert canonical_decimal(1e-3) in ("0.001", "1e-3")

    def test_reject_nan(self):
        with pytest.raises(InvalidSchemaError):
            canonical_decimal(float("nan"))

    def test_reject_infinity(self):
        with pytest.raises(InvalidSchemaError):
            canonical_decimal(float("inf"))

    def test_expression_newline_normalization(self):
        assert normalize_expression("a\r\nb") == "a\nb"
        assert normalize_expression("a\rb") == "a\nb"

    def test_path_normalization(self):
        assert normalize_path("models\\part.FCStd") == "models/part.FCStd"

    def test_excluded_properties(self):
        assert is_excluded_property("Shape", "Part::PropertyPartShape", frozenset())
        # Visibility is emitted as a dedicated object field, not duplicated here.
        assert is_excluded_property("Visibility", "App::PropertyBool", frozenset())
        assert is_excluded_property("Label", "App::PropertyString", frozenset()) is False

    def test_safe_external_path_relative(self):
        result = safe_external_path("models/part.FCStd", "redact")
        assert result["relative"] is True
        assert result["path"] == "models/part.FCStd"

    def test_safe_external_path_absolute(self):
        result = safe_external_path("C:/secret/model.FCStd", "redact")
        assert result["absolute"] is True
        assert "secret" not in result["path"]

    def test_quaternion_w_positive(self):
        q = normalize_quaternion(0, 0, 0, -0.5)
        assert float(q[3]) > 0
