#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Generate and validate the mandatory photo-inspection test catalog."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path


EXPECTED_COUNTS = {
    "GEO": 16,
    "SHT": 16,
    "CAL": 16,
    "IMG": 20,
    "CMP": 18,
    "LIF": 20,
    "SEC": 14,
    "MCP": 18,
    "PKG": 14,
    "PHY": 26,
}
PHASES = {
    "GEO": 1,
    "SHT": 2,
    "CAL": 3,
    "IMG": 4,
    "CMP": 5,
    "LIF": 4,
    "SEC": 0,
    "MCP": 6,
    "PKG": 0,
    "PHY": 6,
}
LANES = {
    "GEO": ["opencv-off", "opencv-floor", "opencv-current"],
    "SHT": ["opencv-floor", "opencv-current", "vector-parser"],
    "CAL": ["opencv-floor", "opencv-current"],
    "IMG": ["opencv-floor", "opencv-current", "synthetic"],
    "CMP": ["opencv-off", "synthetic"],
    "LIF": ["gui-fake-engine"],
    "SEC": ["sanitizer", "fuzz", "resource-limits"],
    "MCP": ["mcp-contract"],
    "PKG": ["installed-artifact"],
    "PHY": ["physical-lab"],
}
ROW = re.compile(
    r"^\| (PI-(?P<category>[A-Z]{3})-(?P<number>[0-9]{3})) "
    r"\| (?P<stimulus>.*?) \| (?P<oracle>.*?) \|\s*$"
)


def generate(plan: Path, destination: Path) -> None:
    records: list[dict[str, object]] = []
    for line in plan.read_text(encoding="utf-8").splitlines():
        match = ROW.match(line)
        if not match:
            continue
        category = match.group("category")
        kind = "physical" if category == "PHY" else "automated"
        records.append(
            {
                "id": match.group(1),
                "category": category,
                "phase": PHASES[category],
                "kind": kind,
                "mandatory": True,
                "stimulus": match.group("stimulus").strip(),
                "oracle": match.group("oracle").strip(),
                "requirements": [f"photo-inspection/{category.lower()}"],
                "lanes": LANES[category],
                "timeoutSeconds": 86400 if kind == "physical" else 120,
                "status": (
                    "physical-evidence-required" if kind == "physical" else "planned"
                ),
                "implementedBy": [],
            }
        )

    payload = {
        "schemaVersion": "1.0",
        "catalogVersion": "photo-inspection-plan-1",
        "tests": records,
    }
    destination.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def validate(manifest: Path) -> list[str]:
    errors: list[str] = []
    try:
        payload = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        return [f"cannot parse manifest: {error}"]

    require(isinstance(payload, dict), "root must be an object", errors)
    if not isinstance(payload, dict):
        return errors
    require(payload.get("schemaVersion") == "1.0", "schemaVersion must be 1.0", errors)
    require(
        isinstance(payload.get("catalogVersion"), str)
        and bool(payload.get("catalogVersion")),
        "catalogVersion must be a non-empty string",
        errors,
    )
    tests = payload.get("tests")
    require(isinstance(tests, list), "tests must be an array", errors)
    if not isinstance(tests, list):
        return errors
    require(len(tests) == 178, f"expected 178 tests, found {len(tests)}", errors)

    ids: list[str] = []
    categories: Counter[str] = Counter()
    required_fields = {
        "id",
        "category",
        "phase",
        "kind",
        "mandatory",
        "stimulus",
        "oracle",
        "requirements",
        "lanes",
        "timeoutSeconds",
        "status",
        "implementedBy",
    }
    allowed_status = {"planned", "implemented", "physical-evidence-required"}
    for index, record in enumerate(tests):
        prefix = f"tests[{index}]"
        if not isinstance(record, dict):
            errors.append(f"{prefix} must be an object")
            continue
        require(
            set(record) == required_fields,
            f"{prefix} fields differ from schema",
            errors,
        )
        test_id = record.get("id")
        require(
            isinstance(test_id, str)
            and re.fullmatch(r"PI-[A-Z]{3}-[0-9]{3}", test_id) is not None,
            f"{prefix}.id is invalid",
            errors,
        )
        if isinstance(test_id, str):
            ids.append(test_id)
        category = record.get("category")
        require(category in EXPECTED_COUNTS, f"{prefix}.category is invalid", errors)
        if isinstance(category, str):
            categories[category] += 1
            require(
                record.get("phase") == PHASES.get(category),
                f"{prefix}.phase does not match category policy",
                errors,
            )
        kind = record.get("kind")
        require(kind in {"automated", "physical"}, f"{prefix}.kind is invalid", errors)
        require(record.get("mandatory") is True, f"{prefix} is not mandatory", errors)
        require(
            isinstance(record.get("stimulus"), str) and bool(record.get("stimulus")),
            f"{prefix}.stimulus is empty",
            errors,
        )
        require(
            isinstance(record.get("oracle"), str) and bool(record.get("oracle")),
            f"{prefix}.oracle is empty",
            errors,
        )
        for field in ("requirements", "lanes"):
            value = record.get(field)
            require(
                isinstance(value, list)
                and bool(value)
                and all(isinstance(item, str) and item for item in value),
                f"{prefix}.{field} must contain strings",
                errors,
            )
        require(
            isinstance(record.get("timeoutSeconds"), int)
            and 1 <= record["timeoutSeconds"] <= 172800,
            f"{prefix}.timeoutSeconds is invalid",
            errors,
        )
        status = record.get("status")
        require(status in allowed_status, f"{prefix}.status is invalid", errors)
        implemented_by = record.get("implementedBy")
        require(
            isinstance(implemented_by, list)
            and all(isinstance(item, str) and item for item in implemented_by),
            f"{prefix}.implementedBy must be an array of names",
            errors,
        )
        if status == "implemented":
            require(bool(implemented_by), f"{prefix} is implemented without a test name", errors)
        if kind == "physical":
            require(
                status == "physical-evidence-required",
                f"{prefix} physical test must require evidence",
                errors,
            )

    require(len(ids) == len(set(ids)), "test IDs are not unique", errors)
    require(dict(categories) == EXPECTED_COUNTS, f"category counts are {dict(categories)}", errors)
    for category, count in EXPECTED_COUNTS.items():
        expected = {f"PI-{category}-{number:03d}" for number in range(1, count + 1)}
        actual = {test_id for test_id in ids if test_id.startswith(f"PI-{category}-")}
        require(actual == expected, f"{category} IDs contain gaps or extras", errors)
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).with_name("TestManifest.json"),
    )
    parser.add_argument("--generate-from-plan", type=Path)
    arguments = parser.parse_args()

    if arguments.generate_from_plan:
        generate(arguments.generate_from_plan, arguments.manifest)
    errors = validate(arguments.manifest)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("PHOTO_INSPECTION_TEST_MANIFEST_OK count=178")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
