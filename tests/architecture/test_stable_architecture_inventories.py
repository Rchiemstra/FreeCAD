# SPDX-License-Identifier: LGPL-2.1-or-later
"""Snapshot identities survive source movement without hiding finding drift."""

from collections import Counter
import json
from pathlib import Path
import shutil
import sys

import pytest

ARCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = ARCH_DIR.parents[1]
sys.path.insert(0, str(ARCH_DIR))

import generate_view_provider_updatedata_inventory as providers
from gui_blocking_live_model import rules, scanner
from test_gui_blocking_live_model_inventory import entry_violations, report_violations
import test_view_provider_updatedata_inventory as provider_gate
import view_provider_updatedata_classifications as classifications


def _serialized_snapshots(root: Path) -> tuple[bytes, bytes]:
    inventory = json.dumps(scanner.build_payload(root), indent=2) + "\n"
    provider_table = providers._render(providers._collect_rows(root))
    return inventory.encode("utf-8"), provider_table.encode("utf-8")


def test_copied_sources_with_shifted_lines_have_byte_identical_snapshots(tmp_path):
    paths = (
        "src/Mod/Part/Gui/ViewProviderExt.cpp",
        "src/Mod/Draft/draftviewproviders/view_base.py",
        "src/Gui/Application.h",  # Includes reviewed exclusions.
    )
    for relative in paths:
        target = tmp_path / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO_ROOT / relative, target)

    findings_before = scanner.scan(tmp_path)
    providers_before = providers._collect_rows(tmp_path)
    assert {Path(f.path).suffix for f in findings_before} >= {".cpp", ".py"}
    assert {Path(row[0]).suffix for row in providers_before} >= {".cpp", ".py"}
    curated_before = scanner.apply_exclusions(findings_before, scanner.load_exclusions())
    assert len(curated_before) < len(findings_before)
    before = _serialized_snapshots(tmp_path)

    for relative in paths:
        target = tmp_path / relative
        comment = "# shifted source" if target.suffix == ".py" else "// shifted source"
        target.write_text("\n" + comment + "\n\n" + target.read_text(encoding="utf-8"),
                          encoding="utf-8")

    findings_after = scanner.scan(tmp_path)
    assert [f.line for f in findings_after] == [f.line + 3 for f in findings_before]
    assert [row[2] for row in providers._collect_rows(tmp_path)] == [
        row[2] + 3 for row in providers_before
    ]
    assert _serialized_snapshots(tmp_path) == before


@pytest.mark.parametrize("copies", [1, 3])
def test_adding_or_removing_repeated_findings_changes_both_snapshots(tmp_path, monkeypatch, copies):
    target = tmp_path / "src/Gui/ViewProviderProbe.cpp"
    target.parent.mkdir(parents=True)
    provider = "void ViewProviderProbe::updateData(const App::Property* prop) { doc->recompute(); }\n"
    target.write_text(provider * 2, encoding="utf-8")
    findings = scanner.build_payload(tmp_path)["findings"]
    for category in ("update-data-provider", "direct-recompute"):
        assert [f["occurrence"] for f in findings if f["category"] == category] == [1, 2]
    before = _serialized_snapshots(tmp_path)
    committed = tmp_path / "providers.md"
    committed.write_bytes(before[1])
    monkeypatch.setattr(provider_gate, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(provider_gate, "INVENTORY_PATH", committed)
    provider_gate.test_every_production_provider_is_inventoried()
    target.write_text(provider * copies, encoding="utf-8")
    after = _serialized_snapshots(tmp_path)
    assert all(left != right for left, right in zip(before, after))
    rows = providers._render(providers._collect_rows(tmp_path)).splitlines()
    assert sum("`ViewProviderProbe`" in row for row in rows) == copies
    with pytest.raises(AssertionError, match="updateData provider inventory drift"):
        provider_gate.test_every_production_provider_is_inventoried()


def test_excluding_one_repeated_finding_keeps_other_occurrences_stable(tmp_path, monkeypatch):
    target = tmp_path / "src/Gui/Probe.cpp"
    target.parent.mkdir(parents=True)
    source = "doc->recompute();\n" * 3
    target.write_text(source, encoding="utf-8")
    finding = next(f for f in scanner.scan(tmp_path)
                   if f.category == "direct-recompute" and f.occurrence == 2)
    exclusion = {key: value for key, value in finding.to_dict().items()
                 if key in {"path", "category", "evidence", "occurrence"}}
    exclusion["reason"] = "Reviewed test exclusion"
    monkeypatch.setattr(scanner, "load_exclusions", lambda: [exclusion])
    before = _serialized_snapshots(tmp_path)
    assert [f["occurrence"] for f in json.loads(before[0])["findings"]
            if f["category"] == "direct-recompute"] == [1, 3]
    target.write_text("\n// shift all calls\n" + source, encoding="utf-8")
    assert _serialized_snapshots(tmp_path) == before


@pytest.mark.parametrize("occurrence", [0, -1, True, "1", None, 3])
def test_validator_rejects_invalid_or_missing_occurrence(tmp_path, occurrence):
    target = tmp_path / "src/Gui/Probe.cpp"
    target.parent.mkdir(parents=True)
    target.write_text("doc->recompute();\n" * 2, encoding="utf-8")
    entry = scanner.scan(tmp_path)[0].to_dict()
    categories = frozenset(rules.CATEGORY_BY_KEY)
    dispositions = frozenset(rules.DISPOSITIONS)
    assert not entry_violations(entry, tmp_path, categories, dispositions)
    entry["occurrence"] = occurrence
    assert entry_violations(entry, tmp_path, categories, dispositions)


@pytest.mark.parametrize("calls", [
    ["first->recompute();\n", "first->recompute();\n", "second->recompute();\n"],
    ["doc->recompute(); // a\\b\n", "doc->recompute(); // a/b\n"],
])
def test_stable_order_does_not_depend_on_source_order(tmp_path, calls):
    target = tmp_path / "src/Gui/Probe.cpp"
    target.parent.mkdir(parents=True)
    target.write_text("".join(calls), encoding="utf-8")
    before = _serialized_snapshots(tmp_path)
    target.write_text("".join(reversed(calls)), encoding="utf-8")
    assert _serialized_snapshots(tmp_path) == before


def test_summary_reports_current_counts_without_writing_snapshots(tmp_path, capsys):
    target = tmp_path / "src/Gui/Probe.cpp"
    target.parent.mkdir(parents=True)
    target.write_text("doc->recompute();\n", encoding="utf-8")
    package = ARCH_DIR / "gui_blocking_live_model"
    committed = {p: p.read_bytes() for p in (package / "inventory.json", package / "REPORT.md")}
    assert scanner.main(["--repo-root", str(tmp_path), "--summary"]) == 0
    summary = capsys.readouterr().out
    payload = scanner.build_payload(tmp_path)
    assert f"Total findings: {len(payload['findings'])}\n" in summary
    for field, label in (("category", "Categories"), ("subsystem", "Subsystems")):
        counts = Counter(f[field] for f in payload["findings"])
        assert f"{label}:\n" in summary
        for name, count in counts.items():
            assert f"  {name}: {count}\n" in summary
    assert "  blocking-invokes: 0\n" in summary
    assert f"  C++: {len(payload['findings'])}\n  Python: 0\n" in summary
    assert f"Excluded: {payload['excluded_count']}\n" in summary
    assert {p: p.read_bytes() for p in committed} == committed


@pytest.mark.parametrize("count", [
    "4,380 findings across nine categories",
    "The snapshot spans 1,997 C++ and 2,383 Python findings.",
    "Owning subsystems: `Gui` (387).",
    "Excluded: 6",
])
def test_report_rejects_derived_counts(count):
    report = (ARCH_DIR / "gui_blocking_live_model/REPORT.md").read_text(encoding="utf-8")
    assert not report_violations(report)
    assert report_violations(report + "\n" + count + "\n")


@pytest.mark.parametrize("mutation", ["missing", "duplicate", "disposition", "description", "count"])
def test_report_checks_category_table(mutation):
    report = (ARCH_DIR / "gui_blocking_live_model/REPORT.md").read_text(encoding="utf-8")
    row = next(line for line in report.splitlines() if line.startswith("| `thread-waits`"))
    replacement = {
        "missing": "",
        "duplicate": row + "\n" + row,
        "disposition": row.replace("investigate", "accepted"),
        "description": "| `thread-waits` | investigate | |",
        "count": row.replace("| investigate |", "| 54 | investigate |"),
    }[mutation]
    assert report_violations(report.replace(row, replacement))


def test_classification_override_uses_occurrence_after_source_lines_shift(tmp_path, monkeypatch):
    target = tmp_path / "src/Gui/ViewProviderProbe.cpp"
    target.parent.mkdir(parents=True)
    provider = "void ViewProviderProbe::updateData(const App::Property* prop) {}\n"
    target.write_text(provider * 2, encoding="utf-8")
    sidecar = tmp_path / "classifications.json"
    sidecar.write_text(json.dumps({"overrides": [{
        "file": "src/Gui/ViewProviderProbe.cpp", "symbol": "ViewProviderProbe",
        "occurrence": 2, "classification": "adapted",
    }]}), encoding="utf-8")
    monkeypatch.setattr(classifications, "CLASSIFICATIONS_PATH", sidecar)
    before = _serialized_snapshots(tmp_path)
    assert before[1].count(b"| unsupported |") == 1
    assert before[1].count(b"| adapted |") == 1
    target.write_text("\n// shift providers\n" + provider * 2, encoding="utf-8")
    assert _serialized_snapshots(tmp_path) == before
