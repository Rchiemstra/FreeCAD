#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""CI log error/warning gate with per-module baselines (stdlib only).

Scans FreeCAD test logs (freecad-e2e, freecad-integration-tests, pytest, or flat
logs) for high-signal runtime error/warning lines that tests print without
failing on (Coin3D, Qt, FreeCAD unhandled/GUI runtime errors, VTK, and fatal
crashes).

Volatile tokens (hex pointer addresses, /tmp paths, PIDs, UUIDs, timestamps,
durations, and VTK log preambles) are normalized into deterministic signatures
and compared per test module against a checked-in JSON baseline. Each entry is
either a plain non-negative integer (exact baseline) or a bounded object
``{"max": N, "observed": "...", "reason": "..."}`` for timing-dependent counts:

  * New signature not in the module's baseline (baseline = 0): FAIL (exit 1)
  * Signature count above baseline / max:                      FAIL (exit 1)
  * Plain integer count equal to baseline:                     PASS
  * Plain integer count below baseline:                        PASS with NOTE
  * Bounded object count in 1..max:                            PASS (no NOTE)
  * Bounded object count equal to 0:                           PASS with NOTE
    (entry can be removed)
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import sys

DEFAULT_BASELINE = Path(__file__).with_name("log-error-baseline.json")
UNATTRIBUTED_MODULE = "<unattributed>"

# Module delimitation patterns:
# - freecad-e2e (.github/scripts/run_gui_tests.py) emits a banner per GUI unit.
# - freecad-integration-tests (FreeCADCmd -t 0, unittest verbosity=2) emits
#   either "test_name (Mod.Class.test_name) ... <status>" on one line, or
#   "test_name (Mod.Class.test_name)" followed by the docstring line.
#   This is only a best-effort module bucket, not proof that output between
#   a header and status belongs to that test: native streams can interleave.
#   Never use these text boundaries to exempt test-specific diagnostics.
# - pytest (e.g. freecad-mcp-live-gui) emits module progress lines beginning
#   with "tests/.../test_foo.py ".
E2E_BANNER_RE = re.compile(r"^Running GUI tests for module:\s*(\S+)")
UNITTEST_LINE_RE = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_]*\s+\(([A-Za-z_][A-Za-z0-9_.]*)\)(?:\s+\.\.\.|\s*$)"
)
PYTEST_LINE_RE = re.compile(r"^(tests/[A-Za-z0-9_./-]+\.py)\s")

# VTK's loguru-formatted error/warning lines:
#   2026-11-21 15:44:42.952 ( 967.500s) [        D0354C80]       vtkXMLReader.cxx:306    ERR| ...
VTK_PREAMBLE_RE = re.compile(
    r"\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d+\s+\(\s*\d+(?:\.\d+)?s\)\s+"
    r"\[\s*[0-9A-Fa-f]+\s*\]\s*[^\s|]+:\d+\s+(ERR|WARN)\|\s*"
)

# High-signal runtime diagnostic patterns. Each alternative anchors at the
# start of the diagnostic so leading unittest prefixes ("test_x (...) ... ")
# or unflushed stdout ("...partsQObject::killTimer: ...") are stripped cleanly.
_DIAGNOSTIC_PREFIXES = (
    # Coin3D runtime errors, warnings, and scene-graph read errors.
    r"Coin (?:error|warning|read error)\b",
    # Qt class::method warnings/errors (allow lowercase prefix from unflushed stdout).
    r"(?<![A-Z])Q[A-Z][A-Za-z0-9_]*::[A-Za-z0-9_~]+:\s+",
    # Qt categorized logging (qt.qpa.*, qt.gui.*, qt.widgets.*, qt.glx, ...).
    r"\bqt\.[a-z][a-z0-9_.]*:\s+",
    # Qt class-level warnings without '::' (e.g. QXcbConnection:, QOpenGLContext:).
    r"(?<![A-Z])Q(?:Xcb\w*|OpenGL\w*|Wayland\w*|BackingStore|Painter|Widget|Object"
    r"|Layout|Window|Screen|Thread|Font\w*|Pixmap|Image|Icon"
    r"|CoreApplication|GuiApplication|Application)\s*:\s+",
    # FreeCAD unhandled C++ exceptions escaping into Qt / application event loop.
    r"Unhandled (?:Base::Exception|std::exception|unknown exception) caught in ",
    # FreeCAD GUI ControlSingleton null-document dialog errors.
    r"ControlSingleton::closeDialog:\s+",
    # VTK library errors and warnings (FEM/VTK pipelines).
    VTK_PREAMBLE_RE.pattern,
    # Native / Python fatal errors, assertions, and X11/GL errors.
    r"(?:Fatal Python error:|Segmentation fault|pure virtual method called"
    r"|double free or corruption|free\(\): invalid|malloc\(\):"
    r"|Assertion [`'\"].*?['\"`] failed|libGL error:|X Error of failed request:"
    r"|GLib-[A-Za-z0-9_-]*-(?:CRITICAL|ERROR)\b|Gtk-(?:CRITICAL|ERROR)\b)",
)

DIAGNOSTIC_RE = re.compile(
    r"(?:" + "|".join(_DIAGNOSTIC_PREFIXES) + r")[^\r\n]*"
)

# Volatile token normalizers applied in order.
_HEX_ADDR_RE = re.compile(r"\b0x[0-9a-fA-F]+\b")
_TMP_PATH_RE = re.compile(r"(?:/var)?/tmp/[^\s'\",):;\]]+")
_UUID_RE = re.compile(
    r"\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b"
)
_ISO_TIMESTAMP_RE = re.compile(
    r"\b\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:?\d{2})?\b"
)
_PID_KV_RE = re.compile(r"\b(pid|PID)(\s*[:=]\s*)\d+\b")
_PID_SPACE_RE = re.compile(r"\b(pid|PID)\s+\d+\b")
_DURATION_S_RE = re.compile(r"\b\d+\.\d+\s*s\b")
_DURATION_MS_RE = re.compile(r"\b\d+(?:\.\d+)?\s*ms\b")


def normalize_message(text: str) -> str:
    """Normalize volatile tokens in a matched error/warning message."""
    text = VTK_PREAMBLE_RE.sub(lambda m: f"VTK {m.group(1)}| ", text)
    text = _HEX_ADDR_RE.sub("0x<ADDR>", text)
    text = _TMP_PATH_RE.sub("/tmp/<TMP>", text)
    text = _UUID_RE.sub("<UUID>", text)
    text = _ISO_TIMESTAMP_RE.sub("<TIMESTAMP>", text)
    text = _PID_KV_RE.sub(r"\1\2<PID>", text)
    text = _PID_SPACE_RE.sub(r"\1 <PID>", text)
    text = _DURATION_S_RE.sub("<DURATION>", text)
    text = _DURATION_MS_RE.sub("<DURATION>", text)
    return text.strip()


def extract_signature(line: str) -> str | None:
    """Return a normalized diagnostic signature if ``line`` matches, else None."""
    match = DIAGNOSTIC_RE.search(line)
    if not match:
        return None
    return normalize_message(match.group(0))


def detect_format(lines: list[str]) -> str:
    """Infer log attribution format ('e2e', 'integration', 'pytest', or 'flat')."""
    for line in lines:
        if E2E_BANNER_RE.match(line):
            return "e2e"
    for line in lines:
        if UNITTEST_LINE_RE.match(line):
            return "integration"
    for line in lines:
        if PYTEST_LINE_RE.match(line):
            return "pytest"
    return "flat"


def scan_lines(
    lines: list[str],
    *,
    log_format: str = "auto",
    default_module: str = UNATTRIBUTED_MODULE,
) -> tuple[str, set[str], dict[str, dict[str, int]]]:
    """Scan log lines and return ``(resolved_format, seen_modules, counts)``."""
    resolved = detect_format(lines) if log_format == "auto" else log_format
    if resolved not in ("e2e", "integration", "pytest", "flat"):
        raise ValueError(f"unsupported log format: {resolved!r}")

    current_module = default_module
    seen_modules: set[str] = set()
    counts: dict[str, Counter[str]] = defaultdict(Counter)

    for line in lines:
        if resolved == "e2e":
            banner = E2E_BANNER_RE.match(line)
            if banner:
                current_module = banner.group(1)
                seen_modules.add(current_module)
                continue
        elif resolved == "integration":
            test_hdr = UNITTEST_LINE_RE.match(line)
            if test_hdr:
                current_module = test_hdr.group(1).split(".", 1)[0]
                seen_modules.add(current_module)
        elif resolved == "pytest":
            pytest_match = PYTEST_LINE_RE.match(line)
            if pytest_match:
                current_module = pytest_match.group(1)
                seen_modules.add(current_module)

        signature = extract_signature(line)
        if signature is not None:
            counts[current_module][signature] += 1
            seen_modules.add(current_module)

    observed = {
        module: dict(sorted(sig_counts.items()))
        for module, sig_counts in sorted(counts.items())
    }
    return resolved, seen_modules, observed


def scan_log(
    path: Path,
    *,
    log_format: str = "auto",
    default_module: str = UNATTRIBUTED_MODULE,
) -> tuple[str, set[str], dict[str, dict[str, int]]]:
    """Read and scan a log file."""
    text = path.read_text(encoding="utf-8", errors="replace")
    return scan_lines(
        text.splitlines(),
        log_format=log_format,
        default_module=default_module,
    )


BaselineEntry = int | dict[str, object]
_ALLOWED_ENTRY_KEYS = frozenset({"max", "observed", "reason"})


def _validate_baseline_entry(
    path: Path,
    module: str,
    signature: str,
    raw: object,
) -> BaselineEntry:
    if isinstance(raw, int) and not isinstance(raw, bool):
        if raw < 0:
            raise ValueError(
                f"invalid baseline {path}: invalid count {raw!r} for {module!r} / {signature!r}"
            )
        return raw
    if isinstance(raw, dict):
        unknown = sorted(set(raw) - _ALLOWED_ENTRY_KEYS)
        if unknown:
            raise ValueError(
                f"invalid baseline {path}: unexpected key(s) {unknown!r} "
                f"for {module!r} / {signature!r}"
            )
        if "max" not in raw:
            raise ValueError(
                f"invalid baseline {path}: object entry missing required 'max' "
                f"for {module!r} / {signature!r}"
            )
        max_val = raw["max"]
        if isinstance(max_val, bool) or not isinstance(max_val, int) or max_val < 0:
            raise ValueError(
                f"invalid baseline {path}: invalid 'max' {max_val!r} "
                f"(expected non-negative integer) for {module!r} / {signature!r}"
            )
        normalized: dict[str, object] = {"max": max_val}
        for field in ("observed", "reason"):
            if field in raw:
                value = raw[field]
                if not isinstance(value, str) or not value.strip():
                    raise ValueError(
                        f"invalid baseline {path}: '{field}' must be a non-empty string "
                        f"for {module!r} / {signature!r}"
                    )
                normalized[field] = value
        return normalized
    raise ValueError(
        f"invalid baseline {path}: invalid entry {raw!r} "
        f"(expected non-negative integer or object with 'max') for {module!r} / {signature!r}"
    )


def _unpack_baseline_entry(entry: BaselineEntry) -> tuple[int, bool]:
    """Return ``(limit, is_bounded)`` for a baseline entry."""
    if isinstance(entry, dict):
        max_val = entry.get("max")
        if isinstance(max_val, bool) or not isinstance(max_val, int) or max_val < 0:
            raise ValueError(f"invalid baseline object entry: {entry!r}")
        return max_val, True
    if isinstance(entry, bool) or not isinstance(entry, int) or entry < 0:
        raise ValueError(f"invalid baseline integer entry: {entry!r}")
    return entry, False


def load_baseline(path: Path) -> dict[str, dict[str, BaselineEntry]]:
    """Load and strictly validate a JSON baseline file."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid baseline JSON {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise ValueError(f"invalid baseline {path}: expected top-level object")
    if data.get("schema_version") != 1:
        raise ValueError(
            f"invalid baseline {path}: unsupported schema_version={data.get('schema_version')!r}"
        )
    modules = data.get("modules")
    if not isinstance(modules, dict):
        raise ValueError(f"invalid baseline {path}: 'modules' must be an object")
    validated: dict[str, dict[str, BaselineEntry]] = {}
    for module, entries in modules.items():
        if not isinstance(module, str) or not module:
            raise ValueError(f"invalid baseline {path}: module key must be a non-empty string")
        if not isinstance(entries, dict):
            raise ValueError(f"invalid baseline {path}: module {module!r} must map to an object")
        module_entries: dict[str, BaselineEntry] = {}
        for signature, raw_entry in entries.items():
            if not isinstance(signature, str) or not signature:
                raise ValueError(
                    f"invalid baseline {path}: empty/non-string signature in {module!r}"
                )
            module_entries[signature] = _validate_baseline_entry(
                path, module, signature, raw_entry
            )
        validated[module] = module_entries
    return validated


def evaluate(
    observed: dict[str, dict[str, int]],
    seen_modules: set[str],
    baseline: dict[str, dict[str, BaselineEntry]],
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    """Compare observed counts against baseline and return ``(failures, notes)``.

    * Failures: any ``actual > limit`` (including ``limit == 0`` for new
      signatures).
    * Notes (for modules observed in the current log):
      - Plain integer entry: any ``actual < baseline`` so baselines ratchet
        down toward zero as bugs are fixed.
      - Bounded object entry (``{"max": N, ...}``): only when ``actual == 0``
        (the entry can be removed); counts ``1..max`` emit no note.
    """
    failures: list[dict[str, object]] = []
    notes: list[dict[str, object]] = []

    for module in sorted(observed):
        base_mod = baseline.get(module, {})
        for signature, actual in sorted(observed[module].items()):
            raw_entry = base_mod.get(signature, 0)
            expected, is_bounded = _unpack_baseline_entry(raw_entry)
            if actual > expected:
                failures.append(
                    {
                        "kind": "new" if expected == 0 else "above",
                        "module": module,
                        "signature": signature,
                        "actual": actual,
                        "baseline": expected,
                        "bounded": is_bounded,
                    }
                )

    for module in sorted(seen_modules):
        base_mod = baseline.get(module)
        if not base_mod:
            continue
        obs_mod = observed.get(module, {})
        for signature, raw_entry in sorted(base_mod.items()):
            expected, is_bounded = _unpack_baseline_entry(raw_entry)
            if expected <= 0:
                continue
            actual = obs_mod.get(signature, 0)
            if is_bounded:
                if actual == 0:
                    notes.append(
                        {
                            "kind": "removable",
                            "module": module,
                            "signature": signature,
                            "actual": 0,
                            "baseline": expected,
                        }
                    )
            elif actual < expected:
                notes.append(
                    {
                        "kind": "below",
                        "module": module,
                        "signature": signature,
                        "actual": actual,
                        "baseline": expected,
                    }
                )

    return failures, notes


def build_baseline_payload(
    paths: list[Path],
    *,
    log_format: str = "auto",
    default_module: str = UNATTRIBUTED_MODULE,
) -> dict[str, object]:
    """Aggregate max counts per (module, signature) across one or more logs."""
    merged: dict[str, dict[str, int]] = defaultdict(dict)
    for path in paths:
        _, _, observed = scan_log(
            path,
            log_format=log_format,
            default_module=default_module,
        )
        for module, sig_counts in observed.items():
            for signature, count in sig_counts.items():
                prev = merged[module].get(signature, 0)
                if count > prev:
                    merged[module][signature] = count
    sorted_modules = {
        module: dict(sorted(sig_counts.items()))
        for module, sig_counts in sorted(merged.items())
    }
    return {
        "schema_version": 1,
        "description": (
            "Per-module baseline counts of known CI log error/warning signatures. "
            "New signatures or counts above baseline fail CI; counts below baseline "
            "pass with a note so entries can ratchet down to zero as bugs are fixed."
        ),
        "modules": sorted_modules,
    }


def check_log_file(
    path: Path,
    baseline: dict[str, dict[str, BaselineEntry]],
    *,
    log_format: str = "auto",
    default_module: str = UNATTRIBUTED_MODULE,
) -> bool:
    """Check one log file, print summary/notes/failures, and return True on pass."""
    resolved, seen_modules, observed = scan_log(
        path,
        log_format=log_format,
        default_module=default_module,
    )
    failures, notes = evaluate(observed, seen_modules, baseline)
    total_matched = sum(sum(sig_counts.values()) for sig_counts in observed.values())

    for note in notes:
        if note.get("kind") == "removable":
            print(
                f"NOTE: [{note['module']}] below baseline "
                f"(actual=0, max={note['baseline']}): entry can be removed: {note['signature']}"
            )
        else:
            print(
                f"NOTE: [{note['module']}] below baseline "
                f"(actual={note['actual']}, baseline={note['baseline']}): {note['signature']}"
            )

    for failure in failures:
        if failure["kind"] == "new":
            print(
                f"FAIL: [{failure['module']}] new error signature "
                f"(actual={failure['actual']}, baseline=0): {failure['signature']}",
                file=sys.stderr,
            )
        elif failure.get("bounded"):
            print(
                f"FAIL: [{failure['module']}] count above baseline max "
                f"(actual={failure['actual']}, max={failure['baseline']}): {failure['signature']}",
                file=sys.stderr,
            )
        else:
            print(
                f"FAIL: [{failure['module']}] count above baseline "
                f"(actual={failure['actual']}, baseline={failure['baseline']}): {failure['signature']}",
                file=sys.stderr,
            )

    status = "PASS" if not failures else "FAIL"
    summary = (
        f"log-gate: {status} ({path.name}, format={resolved}, "
        f"modules={len(seen_modules)}, matched={total_matched}, "
        f"failures={len(failures)}, below_baseline={len(notes)})"
    )
    print(summary, file=sys.stderr if failures else sys.stdout)
    return not failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--baseline",
        type=Path,
        default=DEFAULT_BASELINE,
        help="Path to per-module baseline JSON (default: %(default)s)",
    )
    parser.add_argument(
        "--format",
        dest="log_format",
        choices=("auto", "e2e", "integration", "pytest", "flat"),
        default="auto",
        help="Log module attribution format (default: auto)",
    )
    parser.add_argument(
        "--module",
        default=UNATTRIBUTED_MODULE,
        help="Default module bucket for unattributed lines or flat logs (default: %(default)s)",
    )
    parser.add_argument(
        "--dump-baseline",
        action="store_true",
        help="Scan the given log(s) and print a merged baseline JSON to stdout instead of gating",
    )
    parser.add_argument(
        "logs",
        type=Path,
        nargs="+",
        help="Log file(s) to scan",
    )
    args = parser.parse_args(argv)

    try:
        if args.dump_baseline:
            payload = build_baseline_payload(
                args.logs,
                log_format=args.log_format,
                default_module=args.module,
            )
            print(json.dumps(payload, indent=2))
            return 0

        baseline = load_baseline(args.baseline)
        all_ok = True
        for path in args.logs:
            if not check_log_file(
                path,
                baseline,
                log_format=args.log_format,
                default_module=args.module,
            ):
                all_ok = False
        return 0 if all_ok else 1
    except (OSError, ValueError) as exc:
        print(f"FATAL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
