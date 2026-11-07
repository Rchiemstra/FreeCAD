# SPDX-License-Identifier: LGPL-2.1-or-later
"""Host executables must link libpython when FreeCAD does not.

BUILD_DYNAMIC_LINK_PYTHON is OFF on non-MSVC, so FreeCADApp and FreeCADGui
leave Python symbols undefined. GNU ld then rejects an executable that loads
those libraries unless that executable itself links libpython:

    undefined reference to `Py_DebugFlag'

`--as-needed` can also drop `-lpython` when the executable's own objects never
mention it, which is why the test runners call `keep_python_linked`.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests"
FREECAD_LIBS = {"FreeCADApp", "FreeCADGui"}


def _commands(text):
    text = re.sub(r"#.*?$", "", text, flags=re.M)
    index = 0
    while True:
        match = re.search(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", text[index:])
        if not match:
            return
        start = index + match.end()
        depth = 1
        cursor = start
        while cursor < len(text) and depth:
            if text[cursor] == "(":
                depth += 1
            elif text[cursor] == ")":
                depth -= 1
            cursor += 1
        yield match.group(1), text[start : cursor - 1]
        index = cursor


def _tokens(body):
    return set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", body))


def _test_executables():
    names = []
    for command, body in _commands((TESTS / "CMakeLists.txt").read_text()):
        if command == "set" and body.lstrip().startswith("TestExecutables"):
            names.extend(re.findall(r"\b\w+_tests_run\b", body))
        elif command == "list" and "APPEND" in body and "TestExecutables" in body:
            names.extend(re.findall(r"\b\w+_tests_run\b", body))
    return names


def _explicit_keep_calls():
    calls = set()
    for path in TESTS.rglob("CMakeLists.txt"):
        for command, body in _commands(path.read_text()):
            if command != "keep_python_linked":
                continue
            target = body.strip()
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", target):
                calls.add(target)
    return calls


def test_freecad_host_executables_link_libpython():
    inventory = set(_test_executables())
    kept = _explicit_keep_calls()
    problems = []
    for path in sorted(TESTS.rglob("CMakeLists.txt")):
        executables = []
        libraries = {}
        for command, body in _commands(path.read_text()):
            if command == "add_executable":
                name = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)", body)
                if name:
                    executables.append(name.group(1))
            elif command == "target_link_libraries":
                name = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)", body)
                if name:
                    libraries.setdefault(name.group(1), []).append(body)
        for executable in executables:
            linked = "\n".join(libraries.get(executable, []))
            if not FREECAD_LIBS & _tokens(linked):
                continue
            relative = f"{path.relative_to(ROOT)}: {executable}"
            if "Python3_LIBRARIES" not in linked:
                problems.append(f"{relative} loads FreeCAD without Python3_LIBRARIES")
            if executable not in inventory and executable not in kept:
                problems.append(
                    f"{relative} is not passed to keep_python_linked "
                    "(--as-needed would drop libpython)"
                )
    assert not problems, "\n".join(problems)


def _function_body(text, name):
    match = re.search(
        rf"function\({re.escape(name)}\)(.*?)endfunction\(\)",
        text,
        flags=re.S,
    )
    if not match:
        raise AssertionError(f"missing function {name}")
    return match.group(1)


def test_shared_test_helpers_keep_libpython():
    text = (TESTS / "CMakeLists.txt").read_text()
    setup = _function_body(text, "setup_qt_test")
    assert "BUILD_DYNAMIC_LINK_PYTHON" in setup
    assert "Python3_LIBRARIES" in setup
    assert "keep_python_linked(${_testname}_Tests_run)" in setup
    assert "keep_python_linked(${exe})" in text

    main = (ROOT / "src/Main/CMakeLists.txt").read_text()
    assert main.count("if(NOT BUILD_DYNAMIC_LINK_PYTHON)") >= 2
    assert main.count("${Python3_LIBRARIES}") >= 2
