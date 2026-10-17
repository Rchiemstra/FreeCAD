#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Fail closed on incomplete CI test inventories and test results (stdlib only)."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


def inventory(directory):
    manifest = Path(__file__).with_name("debug-test-binaries.txt")
    names = [line.strip() for line in manifest.read_text().splitlines()
             if line.strip() and not line.startswith("#")]
    if not names or len(names) != len(set(names)):
        raise ValueError("invalid/empty debug test inventory")
    problems = []
    for name in names:
        binary = directory / name
        if not binary.is_file():
            problems.append(f"missing binary: {binary}")
        elif not os.access(binary, os.X_OK):
            problems.append(f"binary not executable: {binary}")
    if problems:
        raise ValueError("\n".join(problems))
    print("\n".join(names))


def count_attribute(node, name, required=False):
    value = node.get(name)
    if value is None and not required:
        return 0
    if value is None or not re.fullmatch(r"[0-9]+", value):
        raise ValueError(f"invalid XML: {node.tag} has invalid {name} count")
    return int(value)


def xml_count(path):
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as error:
        raise ValueError(f"invalid XML: {error}") from error
    if root.tag not in ("testsuite", "testsuites"):
        raise ValueError("invalid XML: expected testsuite/testsuites root")
    for node in root.iter():
        if node.tag in ("failure", "error"):
            raise ValueError("XML contains a failure/error")
        if node.tag in ("testsuite", "testsuites"):
            if count_attribute(node, "failures") or count_attribute(node, "errors"):
                raise ValueError("XML reports a failure/error")
            cases = list(node.iter("testcase"))
            if count_attribute(node, "tests", required=True) != len(cases):
                raise ValueError("invalid XML: test count disagrees with testcase records")
    executed = sum(
        case.get("status") != "notrun"
        and case.get("result") != "skipped"
        and case.find("skipped") is None
        for case in root.iter("testcase")
    )
    if executed == 0:
        raise ValueError("zero executed tests in XML")
    return executed


def xml_results(paths):
    total = 0
    problems = []
    for path in paths:
        try:
            executed = xml_count(path)
            total += executed
            print(f"{path.name}: executed={executed}")
        except (OSError, ValueError) as error:
            problems.append(f"{path}: {error}")
    if problems:
        raise ValueError("\n".join(problems))
    print(f"RESULTS reports={len(paths)} executed={total}")


def gtest_result(args):
    if args.executable:
        args.path.unlink(missing_ok=True)
        with args.log.open("w") as output:
            args.exit_code = subprocess.run(
                [str(args.executable), f"--gtest_filter={args.filter}",
                 f"--gtest_output=xml:{args.path}"],
                stdout=output, stderr=subprocess.STDOUT, check=False,
            ).returncode
    # A complete green report is mandatory even for quarantined processes.
    executed = xml_count(args.path)
    if args.exit_code == 0:
        print(f"{args.binary}: executed={executed} process exit 0")
        return
    entries = json.loads(args.quarantine.read_text())
    if not isinstance(entries, list):
        raise ValueError("invalid quarantine: expected a list")
    log = args.log.read_text(errors="replace")
    passed = re.findall(r"^\[  PASSED  \] ([0-9]+) tests?\.$", log, re.MULTILINE)
    for entry in entries:
        if (entry.get("binary") == args.binary
                and entry.get("filter") == args.filter
                and entry.get("failure_mode") == "signal:11-after-results"
                and entry.get("reason") and entry.get("tracking")
                # subprocess preserves signals as negative numbers. An explicit
                # exit(139) must not impersonate a quarantined SIGSEGV.
                and args.exit_code == -11
                and passed == [str(executed)]
                and "[  FAILED  ]" not in log):
            print(f"QUARANTINE: {args.binary} filter={args.filter!r} "
                  f"{entry['failure_mode']}: {entry['reason']} ({entry['tracking']})")
            return
    raise ValueError(f"{args.binary}: process exit {args.exit_code} (executed={executed})")


def integration_result(path, exit_code):
    if exit_code != 0:
        raise ValueError(f"integration process exit {exit_code}")
    log = path.read_text(errors="replace")
    # TestApp.RunConfiguredTextTest uses unittest.TextTestRunner. Inspect its
    # final summary, so an earlier nested runner's OK cannot mask an empty,
    # failed or truncated outer run. Skip counts do not count as execution.
    summaries = list(re.finditer(
        r"^Ran ([0-9]+) tests? in ([0-9]+(?:\.[0-9]+)?)s\s*$", log, re.MULTILINE))
    if not summaries:
        raise ValueError("integration summary missing")
    summary = summaries[-1]
    status_lines = log[summary.end():].lstrip().splitlines()
    status = status_lines[0] if status_lines else ""
    if not re.fullmatch(r"OK(?: \((?:skipped=[0-9]+|expected failures=[0-9]+)"
                        r"(?:, (?:skipped=[0-9]+|expected failures=[0-9]+))*\))?", status):
        raise ValueError(f"integration summary unsuccessful/incomplete: {status!r}")
    skipped_match = re.search(r"skipped=([0-9]+)", status)
    skipped = int(skipped_match[1]) if skipped_match else 0
    executed = int(summary[1]) - skipped
    if executed <= 0:
        raise ValueError("integration summary has zero executed tests")
    print(f"integration: executed={executed} skipped={skipped} duration={summary[2]}s")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("inventory").add_argument("directory", type=Path)
    commands.add_parser("xml").add_argument("paths", type=Path, nargs="+")
    integration = commands.add_parser("integration")
    integration.add_argument("--exit-code", type=int, required=True)
    integration.add_argument("path", type=Path)
    gtest = commands.add_parser("gtest")
    gtest.add_argument("--binary", required=True)
    gtest.add_argument("--filter", default="*")
    process = gtest.add_mutually_exclusive_group(required=True)
    process.add_argument("--exit-code", type=int)
    process.add_argument("--executable", type=Path)
    gtest.add_argument("--log", type=Path, required=True)
    gtest.add_argument("--quarantine", type=Path,
                       default=Path(__file__).with_name("test-process-quarantine.json"))
    gtest.add_argument("path", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "inventory":
            inventory(args.directory)
        elif args.command == "xml":
            xml_results(args.paths)
        elif args.command == "gtest":
            gtest_result(args)
        else:
            integration_result(args.path, args.exit_code)
    except (OSError, ValueError) as error:
        print(f"FATAL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
