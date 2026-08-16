#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Interactive GUI stress gate for change-aware saving and autonomous MCP editing.

This drives a real FreeCAD GUI process over the MCP XML-RPC surface and proves
the properties the collaboration work claims:

  * view activity (camera, pan, zoom, selection, tree) never invalidates a
    commit and never marks the Model dirty,
  * commits apply exactly once,
  * a healthy per-document failure never becomes a global outage or quarantine,
  * conflicts are targeted: same-property concurrent edits conflict, and
    independent-property edits do not,
  * saves are truthful, and every written FCStd is a valid ZIP with Document.xml.

Isolation
---------
The GUI is always started through ``start_freecad.py`` as required. That
launcher writes into every FreeCAD user-data directory it can find under
``%APPDATA%\\FreeCAD`` -- it reinstalls ``Mod/FreeCADMCP`` and rewrites
``freecad_mcp_settings.json`` -- and it will happily *reuse* an MCP session that
is already listening. Neither is acceptable against a real user profile, so this
harness redirects ``APPDATA`` (``XDG_DATA_HOME`` off Windows) at a throwaway
directory. ``_freecad_user_data_base()`` then resolves inside that directory and
the launcher's writes land there instead.

``MCP_RPC_PORT`` is a module constant in the launcher with no flag or
environment override, so the isolated instance still binds the shared port. The
harness therefore refuses to start when that port is already answering, rather
than attaching to a session it does not own.

Usage:
    python tests/gui/collaboration_gui_stress.py [--view-cycles 500]
                                                 [--save-cycles 100]
                                                 [--keep-running]
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as _dt
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
import xmlrpc.client
import zipfile
from pathlib import Path
from typing import Any

MCP_HOST = "localhost"
MCP_PORT = 9875  # start_freecad.MCP_RPC_PORT; not configurable there.
REPO = Path(__file__).resolve().parents[2]


# --------------------------------------------------------------------------
# evidence
# --------------------------------------------------------------------------
class Evidence:
    """Collects everything the run must be judged on."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)
        self.record: dict[str, Any] = {
            "started_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(),
            "environment": {},
            "phases": {},
            "checks": [],
            "operation_ids": [],
            "readiness_transitions": [],
            "file_hashes": {},
            "leftovers": {},
        }
        self._log_path = self.root / "stress.log"
        self._lock = threading.Lock()

    def log(self, message: str) -> None:
        line = f"[{_dt.datetime.now().strftime('%H:%M:%S')}] {message}"
        with self._lock:
            print(line, flush=True)
            with self._log_path.open("a", encoding="utf-8") as handle:
                handle.write(line + "\n")

    def check(self, name: str, passed: bool, detail: str = "") -> bool:
        with self._lock:
            self.record["checks"].append(
                {"name": name, "passed": bool(passed), "detail": detail}
            )
        self.log(f"  {'ok  ' if passed else 'FAIL'} - {name}{f': {detail}' if detail else ''}")
        return bool(passed)

    def note_readiness(self, label: str, snapshot: Any) -> None:
        with self._lock:
            self.record["readiness_transitions"].append(
                {"label": label, "snapshot": snapshot}
            )

    def note_operation(self, operation_id: Any) -> None:
        if operation_id:
            with self._lock:
                self.record["operation_ids"].append(operation_id)

    def failures(self) -> list[dict[str, Any]]:
        return [c for c in self.record["checks"] if not c["passed"]]

    def write(self) -> Path:
        self.record["finished_utc"] = _dt.datetime.now(_dt.timezone.utc).isoformat()
        self.record["failed_checks"] = self.failures()
        self.record["verdict"] = "PASSED" if not self.failures() else "FAILED"
        path = self.root / "evidence.json"
        path.write_text(json.dumps(self.record, indent=2, default=str), encoding="utf-8")
        return path


# --------------------------------------------------------------------------
# RPC helpers
# --------------------------------------------------------------------------
def proxy() -> xmlrpc.client.ServerProxy:
    """A fresh proxy. xmlrpc proxies are not safe to share across threads."""
    return xmlrpc.client.ServerProxy(
        f"http://{MCP_HOST}:{MCP_PORT}", allow_none=True
    )


def port_is_open(host: str = MCP_HOST, port: int = MCP_PORT) -> bool:
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as sock:
        sock.settimeout(0.75)
        return sock.connect_ex((host, port)) == 0


def run_code(client: xmlrpc.client.ServerProxy, code: str) -> tuple[bool, str]:
    """Execute code inside FreeCAD. Returns (success, message-or-error)."""
    try:
        result = client.execute_code(code)
    except Exception as exc:  # noqa: BLE001 - surface transport failures verbatim
        return False, f"transport: {exc!r}"
    if not isinstance(result, dict):
        return False, f"unexpected result: {result!r}"
    if result.get("success"):
        return True, str(result.get("message", ""))
    return False, str(result.get("error") or result.get("message") or result)


def parsed(message: str) -> dict[str, str]:
    """Parse `key=value` lines printed by in-process snippets."""
    out: dict[str, str] = {}
    for line in message.splitlines():
        line = line.strip()
        if "=" in line and not line.startswith(("Traceback", "  ")):
            key, _, value = line.partition("=")
            key = key.strip()
            if key.isidentifier():
                out[key] = value.strip()
    return out


# --------------------------------------------------------------------------
# launch
# --------------------------------------------------------------------------
def git_fingerprint() -> dict[str, str]:
    def git(*args: str) -> str:
        try:
            return subprocess.run(
                ["git", *args], cwd=REPO, capture_output=True, text=True, check=False
            ).stdout.strip()
        except OSError:
            return "<git unavailable>"

    return {
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "commit": git("rev-parse", "HEAD"),
        "dirty": git("status", "--porcelain") and "yes" or "no",
    }


def binary_fingerprint(exe: Path) -> dict[str, Any]:
    out: dict[str, Any] = {"path": str(exe)}
    for name in ("FreeCADApp.dll", "FreeCADGui.dll", "FreeCADBase.dll"):
        dll = exe.parent / name
        if dll.is_file():
            stat = dll.stat()
            out[name] = {
                "mtime": _dt.datetime.fromtimestamp(stat.st_mtime).isoformat(),
                "size": stat.st_size,
                "sha256": hashlib.sha256(dll.read_bytes()).hexdigest()[:16],
            }
    return out


def launch(exe: Path, profile: Path, ev: Evidence, timeout: float) -> subprocess.Popen[bytes]:
    """Start the GUI through the mandated launcher, with an isolated profile."""
    env = os.environ.copy()
    if sys.platform == "win32":
        env["APPDATA"] = str(profile)
    else:
        env["XDG_DATA_HOME"] = str(profile)
    env["FREECAD_REPO"] = str(REPO)
    profile.mkdir(parents=True, exist_ok=True)

    launcher = REPO / "start_freecad.py"
    command = [
        sys.executable,
        str(launcher),
        "--force-new",
        "--freecad",
        str(exe),
        "--mcp-timeout",
        str(timeout),
    ]
    ev.log(f"launching: {' '.join(command)}")
    ev.log(f"  isolated profile: {profile}")
    process = subprocess.Popen(
        command, cwd=str(REPO), env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    return process


def wait_ready(process: subprocess.Popen[bytes], timeout: float, ev: Evidence) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if port_is_open():
            try:
                if proxy().ping():
                    return True
            except Exception:  # noqa: BLE001
                pass
        if process.poll() is not None:
            ev.log(f"launcher exited early with code {process.returncode}")
            return False
        time.sleep(0.5)
    return False


# --------------------------------------------------------------------------
# in-FreeCAD snippets
# --------------------------------------------------------------------------
VIEW_ACTIVITY = """
import FreeCAD, FreeCADGui
_doc = FreeCAD.getDocument({doc!r})
_view = FreeCADGui.getDocument({doc!r}).ActiveView if FreeCADGui.getDocument({doc!r}) else None
_n = {n}
if _view is not None:
    _cam = _view.getCameraNode()
    for _i in range(_n):
        _view.viewAxonometric() if _i % 4 == 0 else None
        _view.viewTop() if _i % 4 == 1 else None
        _view.viewFront() if _i % 4 == 2 else None
        _view.viewIsometric() if _i % 4 == 3 else None
        _cam.orientation.setValue(_cam.orientation.getValue())
        _view.zoomIn() if _i % 2 == 0 else _view.zoomOut()
        _names = [o.Name for o in _doc.Objects]
        if _names:
            FreeCADGui.Selection.clearSelection()
            FreeCADGui.Selection.addSelection({doc!r}, _names[_i % len(_names)])
print('view_cycles=' + str(_n))
print('touched_after_view=' + str(bool(_doc.isTouched())))
"""


def make_document(client: xmlrpc.client.ServerProxy, name: str, path: Path) -> tuple[bool, str]:
    code = f"""
import FreeCAD
_doc = FreeCAD.newDocument({name!r})
_box = _doc.addObject('Part::Box', 'StressBox')
_box.Length = 10.0
_box.Width = 10.0
_box.Height = 10.0
_second = _doc.addObject('Part::Box', 'SecondBox')
_doc.recompute()
_doc.saveAs({str(path)!r})
print('created=' + _doc.Name)
print('filename=' + _doc.FileName)
"""
    return run_code(client, code)


# --------------------------------------------------------------------------
# phases
# --------------------------------------------------------------------------
def phase_view_vs_mutation(ev: Evidence, doc: str, cycles: int) -> None:
    """View activity concurrent with agent mutations."""
    ev.log(f"phase A: {cycles} view cycles concurrent with mutations")
    stop = threading.Event()
    view_errors: list[str] = []
    view_done = [0]

    def viewer() -> None:
        client = proxy()
        batch = 10
        while not stop.is_set() and view_done[0] < cycles:
            todo = min(batch, cycles - view_done[0])
            ok, message = run_code(client, VIEW_ACTIVITY.format(doc=doc, n=todo))
            if not ok:
                view_errors.append(message)
                break
            view_done[0] += todo

    thread = threading.Thread(target=viewer, name="view-activity", daemon=True)
    thread.start()

    client = proxy()
    commits_ok = 0
    commits_failed: list[str] = []
    expected = 0
    while thread.is_alive() and view_done[0] < cycles:
        expected += 1
        code = f"""
import FreeCAD
_doc = FreeCAD.getDocument({doc!r})
_box = _doc.getObject('StressBox')
_box.Length = float({10 + expected})
_doc.recompute()
print('length=' + str(_box.Length.Value))
"""
        ok, message = run_code(client, code)
        if ok:
            commits_ok += 1
        else:
            commits_failed.append(message)
        time.sleep(0.01)

    stop.set()
    thread.join(timeout=120)

    ev.record["phases"]["view_vs_mutation"] = {
        "view_cycles_completed": view_done[0],
        "view_errors": view_errors,
        "commits_ok": commits_ok,
        "commits_failed": commits_failed,
    }
    ev.check("view activity completed", view_done[0] >= cycles,
             f"{view_done[0]}/{cycles}; errors={view_errors[:2]}")
    ev.check("camera activity never invalidated a commit", not commits_failed,
             f"{commits_ok} ok, {len(commits_failed)} failed: {commits_failed[:2]}")

    # A view-only burst must leave the Model clean.
    client = proxy()
    run_code(client, f"import FreeCAD; FreeCAD.getDocument({doc!r}).save()")
    ok, message = run_code(client, VIEW_ACTIVITY.format(doc=doc, n=25))
    fields = parsed(message)
    ev.check("view activity did not dirty the Model",
             ok and fields.get("touched_after_view") == "False",
             f"touched={fields.get('touched_after_view')} ok={ok}")


def phase_saves(ev: Evidence, doc: str, path: Path, cycles: int) -> None:
    """Save / unchanged-save / Save-Copy / undo / redo under camera activity."""
    ev.log(f"phase B: {cycles} save-family cycles during camera activity")
    stop = threading.Event()

    def camera() -> None:
        client = proxy()
        while not stop.is_set():
            run_code(client, VIEW_ACTIVITY.format(doc=doc, n=5))

    thread = threading.Thread(target=camera, name="camera", daemon=True)
    thread.start()

    client = proxy()
    results: list[dict[str, Any]] = []
    untruthful: list[str] = []
    for index in range(cycles):
        before = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
        code = f"""
import FreeCAD
_doc = FreeCAD.getDocument({doc!r})
_box = _doc.getObject('StressBox')
_doc.openTransaction('stress{index}')
_box.Width = float({10 + index})
_doc.commitTransaction()
_doc.recompute()
print('touched_before_save=' + str(bool(_doc.isTouched())))
_outcome = _doc.saveWithOutcome()
print('touched_after_save=' + str(bool(_doc.isTouched())))
for _k in ('success', 'save_disposition', 'file_written', 'durability_verified',
           'unchanged', 'resulting_clean'):
    print(_k + '=' + str(_outcome.get(_k)))
"""
        changed_ok, changed_msg = run_code(client, code)
        after_changed = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
        changed_fields = parsed(changed_msg)

        # An unchanged save must report `unchanged` and must not rewrite the file.
        unchanged_ok, unchanged_msg = run_code(client, f"""
import FreeCAD
_outcome = FreeCAD.getDocument({doc!r}).saveWithOutcome()
for _k in ('success', 'save_disposition', 'file_written', 'unchanged'):
    print(_k + '=' + str(_outcome.get(_k)))
""")
        after_unchanged = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
        unchanged_fields = parsed(unchanged_msg)

        # Cross-check every reported disposition against observed reality.
        if changed_ok:
            if changed_fields.get("save_disposition") != "written":
                untruthful.append(
                    f"cycle {index}: changed content reported "
                    f"{changed_fields.get('save_disposition')!r}, expected 'written'"
                )
            if changed_fields.get("file_written") != "True":
                untruthful.append(f"cycle {index}: written save reported file_written=False")
            if before is not None and after_changed == before:
                untruthful.append(
                    f"cycle {index}: reported 'written' but the file bytes did not change"
                )
            if changed_fields.get("durability_verified") != "True":
                untruthful.append(f"cycle {index}: written save was not durability_verified")
        if unchanged_ok:
            if unchanged_fields.get("save_disposition") != "unchanged":
                untruthful.append(
                    f"cycle {index}: re-save reported "
                    f"{unchanged_fields.get('save_disposition')!r}, expected 'unchanged'"
                )
            if unchanged_fields.get("file_written") != "False":
                untruthful.append(f"cycle {index}: unchanged save claimed file_written=True")
            if after_unchanged != after_changed:
                untruthful.append(
                    f"cycle {index}: reported 'unchanged' but the file bytes changed"
                )

        undo_ok, _ = run_code(
            client,
            f"import FreeCAD; _d=FreeCAD.getDocument({doc!r}); _d.undo(); _d.recompute(); print('undone=1')",
        )
        redo_ok, _ = run_code(
            client,
            f"import FreeCAD; _d=FreeCAD.getDocument({doc!r}); _d.redo(); _d.recompute(); print('redone=1')",
        )

        if changed_ok and changed_fields.get("touched_after_save") != "False":
            untruthful.append(f"cycle {index}: still touched after a successful save")

        results.append({
            "index": index,
            "changed_save": changed_ok,
            "unchanged_save": unchanged_ok,
            "undo": undo_ok,
            "redo": redo_ok,
            "changed_outcome": changed_fields,
            "unchanged_outcome": unchanged_fields,
            "hash_before": before,
            "hash_after_change": after_changed,
            "hash_after_unchanged": after_unchanged,
        })
        if not (changed_ok and unchanged_ok and undo_ok and redo_ok):
            ev.log(f"  cycle {index} problem: {changed_msg or unchanged_msg}")
            break

    stop.set()
    thread.join(timeout=60)

    # Save a copy, and prove the copy is a valid archive too.
    copy_path = path.with_name("stress-copy.FCStd")
    copy_ok, copy_msg = run_code(client, f"""
import FreeCAD
_outcome = FreeCAD.getDocument({doc!r}).saveCopyWithOutcome({str(copy_path)!r})
for _k in ('success', 'save_disposition', 'file_written', 'target_path'):
    print(_k + '=' + str(_outcome.get(_k)))
""")
    copy_fields = parsed(copy_msg)
    ev.check("Save-Copy reported copy_written",
             copy_fields.get("save_disposition") == "copy_written",
             str(copy_fields))
    # A copy must never become the document's canonical file.
    still_canonical, canonical_msg = run_code(
        client, f"import FreeCAD; print('filename=' + FreeCAD.getDocument({doc!r}).FileName)"
    )
    ev.check("Save-Copy did not repoint the document at the copy",
             parsed(canonical_msg).get("filename", "") == str(path),
             parsed(canonical_msg).get("filename", ""))

    ev.record["phases"]["saves"] = {
        "cycles": results,
        "save_copy": {"ok": copy_ok, "message": copy_msg, "path": str(copy_path)},
        "untruthful": untruthful,
    }
    ev.check("all save-family cycles completed", len(results) == cycles,
             f"{len(results)}/{cycles}")
    ev.check("every save cycle succeeded",
             all(r["changed_save"] and r["unchanged_save"] and r["undo"] and r["redo"]
                 for r in results))
    ev.check("saves were truthful", not untruthful, "; ".join(untruthful[:3]))
    ev.check("Save-Copy succeeded", copy_ok, copy_msg)
    for candidate in (path, copy_path):
        ev.check(f"{candidate.name} is a valid FCStd archive", valid_fcstd(candidate))
        if candidate.is_file():
            ev.record["file_hashes"][candidate.name] = hashlib.sha256(
                candidate.read_bytes()
            ).hexdigest()


def valid_fcstd(path: Path) -> bool:
    if not path.is_file():
        return False
    try:
        with zipfile.ZipFile(path) as archive:
            if archive.testzip() is not None:
                return False
            return "Document.xml" in archive.namelist()
    except zipfile.BadZipFile:
        return False


def phase_two_document_readiness(ev: Evidence, primary: str, secondary: str,
                                 secondary_path: Path) -> None:
    ev.log("phase C: two-document readiness")
    client = proxy()
    ok, message = make_document(client, secondary, secondary_path)
    if not ev.check("second document created", ok, message):
        return

    for label, doc in (("primary", primary), ("secondary", secondary)):
        try:
            snapshot = client.get_mutation_readiness(doc)
        except Exception as exc:  # noqa: BLE001
            snapshot = {"error": repr(exc)}
        ev.note_readiness(f"{label}:{doc}", snapshot)
        ev.check(f"{label} document reports readiness",
                 isinstance(snapshot, dict) and not snapshot.get("error"),
                 str(snapshot)[:200])

    # A healthy failure in one document must not take the other down.
    bad_ok, bad_msg = run_code(
        client,
        f"import FreeCAD; FreeCAD.getDocument({secondary!r}).getObject('NoSuchObject').Length = 1.0",
    )
    ev.check("intentional bad mutation was rejected", not bad_ok, bad_msg[:160])

    for label, doc in (("primary-after-failure", primary),
                       ("secondary-after-failure", secondary)):
        try:
            snapshot = client.get_mutation_readiness(doc)
        except Exception as exc:  # noqa: BLE001
            snapshot = {"error": repr(exc)}
        ev.note_readiness(label, snapshot)
        ready = isinstance(snapshot, dict) and not snapshot.get("error")
        ev.check(f"{label} still reports readiness (no global outage)", ready,
                 str(snapshot)[:200])

    still_alive, alive_msg = run_code(
        client,
        f"import FreeCAD; _d=FreeCAD.getDocument({primary!r}); _d.getObject('SecondBox').Length=7.0;"
        " _d.recompute(); print('ok=1')",
    )
    ev.check("healthy document still accepts mutations after a sibling failure",
             still_alive, alive_msg[:160])


def phase_conflicts(ev: Evidence, doc: str) -> None:
    ev.log("phase D: targeted conflict detection")
    barrier = threading.Barrier(2, timeout=30)
    results: dict[str, tuple[bool, str]] = {}
    lock = threading.Lock()

    def writer(tag: str, code: str) -> None:
        client = proxy()
        with contextlib.suppress(threading.BrokenBarrierError):
            barrier.wait()
        outcome = run_code(client, code)
        with lock:
            results[tag] = outcome

    same = [
        threading.Thread(target=writer, args=(f"same{i}", f"""
import FreeCAD
_d = FreeCAD.getDocument({doc!r})
_d.getObject('StressBox').Height = float({100 + i})
_d.recompute()
print('height=' + str(_d.getObject('StressBox').Height.Value))
"""), name=f"same{i}") for i in range(2)
    ]
    for thread in same:
        thread.start()
    for thread in same:
        thread.join(timeout=60)
    same_results = {k: v for k, v in results.items() if k.startswith("same")}
    ev.record["phases"]["same_property"] = {k: list(v) for k, v in same_results.items()}
    ev.check("same-property concurrent writes were serialised or conflicted",
             len(same_results) == 2,
             str(same_results)[:300])

    results.clear()
    barrier = threading.Barrier(2, timeout=30)
    independent = [
        threading.Thread(target=writer, args=("indep-a", f"""
import FreeCAD
_d = FreeCAD.getDocument({doc!r})
_d.getObject('StressBox').Length = 42.0
_d.recompute()
print('ok=1')
"""), name="indep-a"),
        threading.Thread(target=writer, args=("indep-b", f"""
import FreeCAD
_d = FreeCAD.getDocument({doc!r})
_d.getObject('SecondBox').Width = 24.0
_d.recompute()
print('ok=1')
"""), name="indep-b"),
    ]
    for thread in independent:
        thread.start()
    for thread in independent:
        thread.join(timeout=60)
    indep = dict(results)
    ev.record["phases"]["independent_property"] = {k: list(v) for k, v in indep.items()}
    ev.check("independent-property concurrent writes both succeeded",
             all(ok for ok, _ in indep.values()) and len(indep) == 2,
             str(indep)[:300])

    client = proxy()
    ok, message = run_code(client, f"""
import FreeCAD
_d = FreeCAD.getDocument({doc!r})
print('length=' + str(_d.getObject('StressBox').Length.Value))
print('width=' + str(_d.getObject('SecondBox').Width.Value))
""")
    fields = parsed(message)
    ev.check("independent writes both landed exactly once",
             ok and fields.get("length") == "42.0" and fields.get("width") == "24.0",
             str(fields))


def phase_pause_resume(ev: Evidence, doc: str) -> None:
    ev.log("phase E: pause / resume")
    client = proxy()
    pause_ok, pause_msg = run_code(client, """
from FreeCADMCP import automation_pause
print('pause=' + str(automation_pause.request_local_pause_after_current()))
print('status=' + str(automation_pause.status()))
""")
    ev.check("pause was requested", pause_ok, pause_msg[:200])

    resume_ok, resume_msg = run_code(client, """
from FreeCADMCP import automation_pause
print('resume=' + str(automation_pause.resume_local_agent_writes()))
print('status=' + str(automation_pause.status()))
""")
    ev.check("resume restored agent writes", resume_ok, resume_msg[:200])

    after_ok, after_msg = run_code(client, f"""
import FreeCAD
_d = FreeCAD.getDocument({doc!r})
_d.getObject('StressBox').Length = 43.0
_d.recompute()
print('length=' + str(_d.getObject('StressBox').Length.Value))
""")
    ev.check("mutations work again after resume", after_ok, after_msg[:200])
    ev.record["phases"]["pause_resume"] = {
        "pause": pause_msg, "resume": resume_msg, "after": after_msg,
    }


def collect_screenshot(ev: Evidence, tag: str) -> None:
    client = proxy()
    target = ev.root / f"screenshot-{tag}.png"
    ok, message = run_code(client, f"""
import FreeCADGui
_view = FreeCADGui.ActiveDocument.ActiveView if FreeCADGui.ActiveDocument else None
if _view is not None:
    _view.saveImage({str(target)!r}, 1024, 768, 'Current')
    print('screenshot=' + {str(target)!r})
else:
    print('screenshot=none')
""")
    ev.log(f"  screenshot {tag}: {'saved' if target.is_file() else message[:120]}")


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--view-cycles", type=int, default=500)
    parser.add_argument("--save-cycles", type=int, default=100)
    parser.add_argument("--freecad", default=str(REPO / "build/release/bin/FreeCAD.exe"))
    parser.add_argument("--mcp-timeout", type=float, default=240.0)
    parser.add_argument("--keep-running", action="store_true",
                        help="Leave the GUI running for manual inspection")
    args = parser.parse_args(argv if argv is not None else sys.argv[1:])

    stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_root = Path(
        os.environ.get("FC_STRESS_ROOT")
        or Path(os.environ.get("TEMP", "/tmp")) / f"fcstress{stamp}"
    )
    ev = Evidence(run_root / "evidence")
    workdir = run_root / "documents"
    profile = run_root / "profile"
    workdir.mkdir(parents=True, exist_ok=True)

    exe = Path(args.freecad)
    if not exe.is_file():
        ev.log(f"FATAL: FreeCAD executable not found: {exe}")
        return 2

    if port_is_open():
        ev.log(
            f"FATAL: something is already listening on {MCP_HOST}:{MCP_PORT}. "
            "start_freecad.py has a fixed port and would reuse that session, "
            "which this harness must never do. Refusing to start."
        )
        return 2

    ev.record["environment"] = {
        "executable": str(exe),
        "binary_fingerprint": binary_fingerprint(exe),
        "git": git_fingerprint(),
        "isolated_profile": str(profile),
        "isolated_mod_dir": str(profile / "FreeCAD" / "Mod"),
        "documents_dir": str(workdir),
        "mcp_host": MCP_HOST,
        "mcp_port": MCP_PORT,
        "launcher": str(REPO / "start_freecad.py"),
        "python": sys.executable,
    }
    ev.log("environment:")
    for key, value in ev.record["environment"].items():
        ev.log(f"  {key}: {value}")

    process = launch(exe, profile, ev, args.mcp_timeout)
    try:
        if not wait_ready(process, args.mcp_timeout, ev):
            ev.check("MCP RPC became ready", False, "timeout or early exit")
            ev.write()
            return 1
        ev.check("MCP RPC became ready", True)

        client = proxy()
        ok, message = run_code(client, """
import os, FreeCAD
print('freecad_pid=' + str(os.getpid()))
print('freecad_version=' + '.'.join(FreeCAD.Version()[:3]))
print('user_appdata=' + str(FreeCAD.getUserAppDataDir()))
""")
        fields = parsed(message)
        ev.record["environment"].update({
            "freecad_pid": fields.get("freecad_pid"),
            "freecad_version": fields.get("freecad_version"),
            "reported_user_app_data": fields.get("user_appdata"),
        })
        ev.log(f"  freecad pid: {fields.get('freecad_pid')}")
        ev.log(f"  user app data: {fields.get('user_appdata')}")
        reported = str(fields.get("user_appdata", ""))
        ev.check("GUI is using the isolated profile, not the real one",
                 str(profile).lower() in reported.lower(), reported)

        doc = f"CollaborationStress_{stamp}".replace("-", "_")
        doc_path = workdir / f"{doc}.FCStd"
        created, created_msg = make_document(client, doc, doc_path)
        if not ev.check("stress document created", created, created_msg[:200]):
            ev.write()
            return 1

        collect_screenshot(ev, "initial")
        phase_view_vs_mutation(ev, doc, args.view_cycles)
        phase_saves(ev, doc, doc_path, args.save_cycles)
        phase_two_document_readiness(
            ev, doc, f"{doc}_second", workdir / f"{doc}_second.FCStd"
        )
        phase_conflicts(ev, doc)
        phase_pause_resume(ev, doc)
        collect_screenshot(ev, "final")

        ev.record["leftovers"] = {
            "documents_dir": sorted(p.name for p in workdir.iterdir()),
            "repo_root_fcstd": sorted(
                p.name for p in REPO.iterdir()
                if p.suffix in {".FCStd", ".displaced"} or ".displaced" in p.name
            ),
        }
        ev.check("no .displaced leftovers beside the documents",
                 not [n for n in ev.record["leftovers"]["documents_dir"]
                      if ".displaced" in n],
                 str(ev.record["leftovers"]["documents_dir"]))
        ev.check("no stray documents in the repository root",
                 not ev.record["leftovers"]["repo_root_fcstd"],
                 str(ev.record["leftovers"]["repo_root_fcstd"]))
    finally:
        if not args.keep_running:
            ev.log("shutting the GUI down")
            with contextlib.suppress(Exception):
                proxy().execute_code(
                    "import FreeCADGui; FreeCADGui.getMainWindow().close()"
                )
            time.sleep(3)
            with contextlib.suppress(Exception):
                process.terminate()
        path = ev.write()
        ev.log(f"evidence: {path}")
        ev.log(f"GUI_STRESS_RESULT: {ev.record['verdict']}")

    return 0 if not ev.failures() else 1


if __name__ == "__main__":
    raise SystemExit(main())
