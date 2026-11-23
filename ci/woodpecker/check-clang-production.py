#!/usr/bin/env python3
"""Compile the real DocumentPyImp translation unit with Clang, without producing an object."""

import argparse
import json
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


def clang_command(entry, compiler, source):
    """Translate one CMake compile command without interpreting it in a shell."""
    args = entry.get("arguments")
    if args is None:
        args = shlex.split(entry["command"])
    if not isinstance(args, list) or not all(isinstance(arg, str) for arg in args):
        raise ValueError("compile command arguments must be a string list")
    args = list(args)
    if args and Path(args[0]).name == "ccache":
        args.pop(0)
    if not args or Path(args[0]).name not in ("g++", "c++"):
        raise ValueError("expected the configured GCC C++ compiler in compile_commands.json")
    # Clang expands bare @response tokens before interpreting option operands.
    if any(arg.startswith("@") for arg in args[1:]):
        raise ValueError("unsupported response-file argument")
    # CMake/Ninja emits ... -o OBJECT -c SOURCE. Do not infer the source from
    # another operand such as -I or -o: only the final -c input counts.
    if len(args) < 5 or args[-4] != "-o" or args[-2] != "-c":
        raise ValueError("expected CMake compile suffix: -o OBJECT -c SOURCE")
    output, actual_input = args[-3], args[-1]
    if not output or output.startswith("-") or not actual_input or actual_input.startswith("-"):
        raise ValueError("invalid output or source in CMake compile suffix")
    directory = Path(entry["directory"])
    if (directory / actual_input).resolve() != source:
        raise ValueError("CMake -c input is not the selected production source")
    if (directory / output).resolve() == source:
        raise ValueError("CMake output must differ from the source")

    # Permit only the GCC Debug CMake flags needed by this translation unit.
    # Unknown options are errors: alternate driver actions, forwarded options,
    # response files and dependency-output flags must not turn this into a
    # successful preprocessing/driver-only run or create side effects.
    prefix = args[1:-4]
    separate_operands = {"-D", "-U", "-I", "-isystem", "-iquote", "-idirafter"}
    exact_flags = {"-fPIC", "-fpic", "-fPIE", "-fpie", "-pthread", "-pipe",
                   "-fdiagnostics-color", "-fno-diagnostics-color"}
    patterns = (
        r"-D.+", r"-U.+", r"-I.+", r"-isystem.+", r"-iquote.+", r"-idirafter.+",
        r"-W[A-Za-z0-9][A-Za-z0-9_=-]*",
        r"-g(?:[0-3]|gdb[0-3]?)?", r"-O(?:[0-3sgz]|fast)?",
        r"-std=(?:c\+\+|gnu\+\+)(?:[0-9]{2}|2[abcz])",
        r"-fdiagnostics-color=(?:auto|always|never)",
    )
    i = 0
    while i < len(prefix):
        arg = prefix[i]
        if arg in separate_operands:
            if i + 1 >= len(prefix) or not prefix[i + 1] or prefix[i + 1].startswith("-"):
                raise ValueError(f"unsupported or missing compiler option operand: {arg}")
            i += 2
            continue
        if arg not in exact_flags and not any(re.fullmatch(pattern, arg) for pattern in patterns):
            raise ValueError(f"unsupported compiler option: {arg}")
        i += 1
    return [compiler, *args[1:-4], actual_input, "-fsyntax-only"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", default="build/debug/compile_commands.json")
    parser.add_argument("--source", default="src/App/DocumentPyImp.cpp")
    parser.add_argument("--compiler", default="clang++")
    options = parser.parse_args()
    try:
        compiler = shutil.which(options.compiler)
        if not compiler:
            raise ValueError(f"required Clang compiler not found: {options.compiler}")
        # which() can return ./clang++; the compile runs in another directory.
        # Keep the basename/symlink so Clang selects the intended driver mode.
        compiler = str(Path(compiler).absolute())
        source = Path(options.source).resolve(strict=True)
        entries = json.loads(Path(options.database).read_text(encoding="utf-8"))
        if not isinstance(entries, list):
            raise ValueError("compilation database is not a list")
        matches = []
        for entry in entries:
            directory = Path(entry["directory"])
            candidate = Path(entry["file"])
            if not candidate.is_absolute():
                candidate = directory / candidate
            if candidate.resolve() == source:
                matches.append(entry)
        if len(matches) != 1:
            raise ValueError(f"expected one production compile command for {source}, found {len(matches)}")
        entry = matches[0]
        command = clang_command(entry, compiler, source)
        print("Clang production syntax check:", shlex.join(command), flush=True)
        return subprocess.run(command, cwd=entry["directory"], check=False).returncode
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
        print(f"Clang production syntax check failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
