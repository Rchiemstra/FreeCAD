"""Command-line interface for freecad-git."""

from __future__ import annotations

import argparse
import difflib
import sys
from pathlib import Path

from . import __version__
from .config import discover_fcstd_files, load_config
from .errors import (
    EXIT_DIAGNOSTIC_FAILURE,
    EXIT_GENERAL_FAILURE,
    EXIT_INVALID_CONFIG,
    EXIT_INVALID_SCHEMA,
    EXIT_INVALID_XML,
    EXIT_IO_ERROR,
    EXIT_STALE_OR_MISSING,
    EXIT_SUCCESS,
    EXIT_UNSAFE_ARCHIVE,
    EXIT_UNSUPPORTED_DOCUMENT,
    DiagnosticFailureError,
    FreecadGitError,
    InvalidConfigError,
    InvalidSchemaError,
    InvalidXmlError,
    IOError,
    MalformedSidecarError,
    MissingSidecarError,
    StaleSidecarError,
    UnsafeArchiveError,
    UnsupportedDocumentError,
)
from .export import check_file, export_file, export_to_bytes, sidecar_path_for
from .schema_validate import validate_sidecar_file


def _find_repo_root(start: Path | None = None) -> Path:
    current = (start or Path.cwd()).resolve()
    for parent in [current, *current.parents]:
        if (parent / ".freecad-git.toml").is_file() or (parent / ".git").is_dir():
            return parent
    return current


def cmd_export(args: argparse.Namespace) -> int:
    config = load_config(repo_root=_find_repo_root())

    if args.all:
        files = discover_fcstd_files(config)
        if not files:
            print("No .FCStd files found.", file=sys.stderr)
            return EXIT_SUCCESS
        failures = 0
        updated = 0
        unchanged = 0
        exit_code = EXIT_SUCCESS
        for path in files:
            try:
                data = export_to_bytes(path, config)
                if args.stdout:
                    sys.stdout.buffer.write(data)
                else:
                    from .export import write_sidecar_atomic

                    if write_sidecar_atomic(path, data):
                        updated += 1
                        print(f"Exported: {path}")
                    else:
                        unchanged += 1
            except FreecadGitError as exc:
                failures += 1
                exit_code = exc.exit_code
                print(f"FAILED: {path}: {exc.message}", file=sys.stderr)
        print(
            f"Summary: {len(files)} files, {updated} updated, {unchanged} unchanged, {failures} failed"
        )
        return exit_code if failures else EXIT_SUCCESS

    if not args.paths:
        print("error: path required (or use --all)", file=sys.stderr)
        return EXIT_GENERAL_FAILURE

    failures = 0
    exit_code = EXIT_SUCCESS
    for path_str in args.paths:
        path = Path(path_str)
        try:
            data = export_file(path, config, stdout=args.stdout)
            if args.stdout:
                sys.stdout.buffer.write(data)
            else:
                print(f"Exported: {path}")
        except FreecadGitError as exc:
            failures += 1
            print(f"error: {exc.message}", file=sys.stderr)
            exit_code = exc.exit_code
    return exit_code if failures else EXIT_SUCCESS


def cmd_check(args: argparse.Namespace) -> int:
    config = load_config(repo_root=_find_repo_root())

    if args.all:
        files = discover_fcstd_files(config)
        errors: list[str] = []
        for path in files:
            try:
                check_file(path, config)
            except FreecadGitError as exc:
                errors.append(exc.message)
        if errors:
            for err in errors:
                print(err, file=sys.stderr)
            return EXIT_STALE_OR_MISSING
        print(f"All {len(files)} sidecars are up to date.")
        return EXIT_SUCCESS

    if not args.paths:
        print("error: path required (or use --all)", file=sys.stderr)
        return EXIT_GENERAL_FAILURE

    exit_code = EXIT_SUCCESS
    for path_str in args.paths:
        path = Path(path_str)
        try:
            check_file(path, config)
            print(f"OK: {sidecar_path_for(path)}")
        except StaleSidecarError as exc:
            print(exc.message, file=sys.stderr)
            # Show bounded diff
            sidecar = sidecar_path_for(path)
            if sidecar.exists():
                try:
                    expected = export_to_bytes(path, config)
                    actual = sidecar.read_bytes()
                    diff = difflib.unified_diff(
                        actual.decode("utf-8").splitlines(keepends=True),
                        expected.decode("utf-8").splitlines(keepends=True),
                        fromfile=str(sidecar),
                        tofile="expected",
                        n=3,
                    )
                    lines = list(diff)
                    if len(lines) > 40:
                        lines = lines[:40] + ["... (diff truncated)\n"]
                    sys.stderr.writelines(lines)
                except Exception:
                    pass
            exit_code = EXIT_STALE_OR_MISSING
        except MissingSidecarError as exc:
            print(exc.message, file=sys.stderr)
            print(
                f"\nRun:\n  freecad-git export {path}\n  git add {path} {sidecar_path_for(path)}",
                file=sys.stderr,
            )
            exit_code = EXIT_STALE_OR_MISSING
        except FreecadGitError as exc:
            print(f"error: {exc.message}", file=sys.stderr)
            exit_code = exc.exit_code
    return exit_code


def cmd_validate(args: argparse.Namespace) -> int:
    failures = 0
    for path_str in args.paths:
        path = Path(path_str)
        try:
            validate_sidecar_file(path)
            print(f"Valid: {path}")
        except FreecadGitError as exc:
            failures += 1
            print(f"INVALID: {path}: {exc.message}", file=sys.stderr)
    return EXIT_INVALID_SCHEMA if failures else EXIT_SUCCESS


def cmd_diagnostics(args: argparse.Namespace) -> int:
    from .diagnostics import run_diagnostics

    config = load_config(repo_root=_find_repo_root())
    for path_str in args.paths:
        path = Path(path_str)
        try:
            report = run_diagnostics(path, config)
            print(report)
        except DiagnosticFailureError as exc:
            print(f"error: {exc.message}", file=sys.stderr)
            return EXIT_DIAGNOSTIC_FAILURE
    return EXIT_SUCCESS


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="freecad-git",
        description="Deterministic Git sidecar generator for FreeCAD .FCStd documents",
    )
    parser.add_argument("--version", action="version", version=f"freecad-git {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    export_p = sub.add_parser("export", help="Generate .FCStd.git.json sidecar")
    export_p.add_argument("paths", nargs="*", help="Path to .FCStd file(s)")
    export_p.add_argument("--all", action="store_true", help="Export all configured models")
    export_p.add_argument("--stdout", action="store_true", help="Write JSON to stdout only")
    export_p.set_defaults(func=cmd_export)

    check_p = sub.add_parser("check", help="Verify sidecar matches FCStd")
    check_p.add_argument("paths", nargs="*", help="Path to .FCStd file(s)")
    check_p.add_argument("--all", action="store_true", help="Check all configured models")
    check_p.set_defaults(func=cmd_check)

    validate_p = sub.add_parser("validate", help="Validate a .git.json sidecar")
    validate_p.add_argument("paths", nargs="+", help="Path to sidecar file(s)")
    validate_p.set_defaults(func=cmd_validate)

    diag_p = sub.add_parser(
        "diagnostics",
        help="Run trusted FreeCAD diagnostics (non-authoritative)",
    )
    diag_p.add_argument("paths", nargs="+", help="Path to .FCStd file(s)")
    diag_p.set_defaults(func=cmd_diagnostics)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except InvalidConfigError as exc:
        print(f"configuration error: {exc.message}", file=sys.stderr)
        return EXIT_INVALID_CONFIG
    except UnsafeArchiveError as exc:
        print(f"unsafe archive: {exc.message}", file=sys.stderr)
        return EXIT_UNSAFE_ARCHIVE
    except InvalidXmlError as exc:
        print(f"invalid XML: {exc.message}", file=sys.stderr)
        return EXIT_INVALID_XML
    except InvalidSchemaError as exc:
        print(f"invalid schema: {exc.message}", file=sys.stderr)
        return EXIT_INVALID_SCHEMA
    except UnsupportedDocumentError as exc:
        print(f"unsupported document: {exc.message}", file=sys.stderr)
        return EXIT_UNSUPPORTED_DOCUMENT
    except IOError as exc:
        print(f"I/O error: {exc.message}", file=sys.stderr)
        return EXIT_IO_ERROR
    except FreecadGitError as exc:
        print(f"error: {exc.message}", file=sys.stderr)
        return exc.exit_code
    except KeyboardInterrupt:
        return EXIT_GENERAL_FAILURE
    except Exception as exc:
        print(f"unexpected error: {exc}", file=sys.stderr)
        return EXIT_GENERAL_FAILURE


if __name__ == "__main__":
    sys.exit(main())
