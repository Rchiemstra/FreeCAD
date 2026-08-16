#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Interactive GUI stress gate for change-aware saving and autonomous MCP editing.

This drives a real FreeCAD GUI process over the MCP JSON-RPC 2.0 surface
(``POST /jsonrpc``) and proves the properties the collaboration work claims:

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
``%APPDATA%\\FreeCAD``: it reinstalls ``Mod/FreeCADMCP`` (via a delete) and
rewrites ``freecad_mcp_settings.json`` to force ``auto_start_rpc``. It also has
a ``_reuse_existing_mcp_if_possible()`` path that would drive an MCP session it
did not start. None of that is acceptable against a real user profile, so this
harness redirects ``APPDATA`` (``XDG_DATA_HOME`` off Windows) at a throwaway
directory. ``_freecad_user_data_base()`` then resolves inside that directory and
the launcher's writes land there instead.

``MCP_RPC_PORT`` is a module constant in the launcher with no flag or
environment override, so the isolated instance still binds the shared port. The
harness therefore refuses to start when that port is already answering, rather
than attaching to a session it does not own.

Note on the launcher's own MCP handling: ``_ping_mcp_rpc``, ``_mcp_rpc_proxy``
and ``_reuse_existing_mcp_if_possible`` all still speak XML-RPC, which the
server retired. They therefore always fail, which means the launcher's readiness
wait burns its full timeout and then reports failure for a server that is
actually up (and, as a side effect, its session-reuse path can no longer
trigger). The harness passes ``--no-wait-for-mcp`` and waits for readiness
itself over JSON-RPC.

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
import socket
import subprocess
import sys
import threading
import time
import urllib.request
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
class Rpc:
    """JSON-RPC 2.0 client for the MCP listener.

    XML-RPC is retired. The listener answers `410 Gone` on `/RPC2` with
    `Link: </jsonrpc>; rel="successor-version"` and expects JSON-RPC 2.0 at
    `/jsonrpc`. start_freecad.py now speaks JSON-RPC too, so its own readiness
    wait works and the harness no longer passes --no-wait-for-mcp; it still
    confirms readiness itself before driving anything.
    """

    def __init__(self, host: str = MCP_HOST, port: int = MCP_PORT) -> None:
        self.url = f"http://{host}:{port}/jsonrpc"
        self._id = 0

    def call(self, method: str, params: Any = None, timeout: float = 120.0) -> Any:
        self._id += 1
        payload = json.dumps({
            "jsonrpc": "2.0", "id": self._id,
            "method": method, "params": {} if params is None else params,
        }).encode("utf-8")
        request = urllib.request.Request(
            self.url, data=payload, headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = json.loads(response.read().decode("utf-8"))
        if "error" in body:
            raise RuntimeError(f"{method}: {body['error']}")
        return body.get("result")


def proxy() -> Rpc:
    """A fresh client. One per thread; do not share."""
    return Rpc()


def port_is_open(host: str = MCP_HOST, port: int = MCP_PORT) -> bool:
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as sock:
        sock.settimeout(0.75)
        return sock.connect_ex((host, port)) == 0


def process_alive(pid: int) -> bool:
    """True if the OS still has this process."""
    if sys.platform == "win32":
        out = subprocess.run(
            ["tasklist", "/FI", f"PID eq {pid}", "/NH"],
            capture_output=True, text=True, check=False,
        ).stdout
        return str(pid) in out
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def rpc_is_ready() -> bool:
    try:
        return bool(Rpc().call("ping", timeout=5.0))
    except Exception:  # noqa: BLE001
        return False


def run_code(client: Rpc, code: str) -> tuple[bool, str]:
    """Execute code inside FreeCAD. Returns (success, message-or-error)."""
    try:
        result = client.call("execute_code", {"code": code})
    except Exception as exc:  # noqa: BLE001 - surface transport failures verbatim
        return False, f"transport: {exc!r}"
    if not isinstance(result, dict):
        return False, f"unexpected result: {result!r}"
    if result.get("success"):
        return True, str(result.get("message", ""))
    return False, str(result.get("error") or result.get("message") or result)


def parsed(message: str) -> dict[str, str]:
    """Parse `key=value` lines printed by in-process snippets.

    execute_code returns captured stdout wrapped as
    "Python code execution completed.\\nOutput: first=1\\nsecond=2\\n", so the
    first payload line carries an "Output: " prefix. Without stripping it the
    key parses as "Output: first", which is not an identifier, and every value
    from that line is silently dropped.
    """
    out: dict[str, str] = {}
    for line in message.splitlines():
        line = line.strip()
        if line.startswith("Output:"):
            line = line[len("Output:"):].strip()
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
    # Isolation needs BOTH halves, and they must agree.
    #
    # APPDATA steers start_freecad.py, whose _freecad_user_data_base() reads it
    # to decide where to install the addon and write the settings file.
    #
    # APPDATA does NOT steer FreeCAD itself. On Windows the profile is resolved
    # by SHGetFolderPathW(CSIDL_APPDATA) (Application.cpp:4043), which reads the
    # user token and ignores the environment entirely. Setting APPDATA alone
    # redirects the launcher's writes while FreeCAD keeps using the real
    # profile. The supported override is FREECAD_USER_HOME / FREECAD_USER_DATA
    # (Application.cpp:4141), and FREECAD_USER_HOME must already exist -- a
    # missing directory is silently discarded by toNativePath().
    #
    # The two must be offset by one level to line up. Measured, not assumed:
    # with FREECAD_USER_HOME=<x>, FreeCAD reports getUserAppDataDir() == "<x>\"
    # exactly -- no vendor and no version subdirectory. The launcher meanwhile
    # installs to $APPDATA/FreeCAD/Mod. So APPDATA gets the parent and
    # FREECAD_USER_* gets <parent>/FreeCAD, and both land on the same tree:
    #
    #   APPDATA            = <profile>
    #   FREECAD_USER_HOME  = <profile>/FreeCAD   <- FreeCAD's data dir
    #   launcher installs to <profile>/FreeCAD/Mod/FreeCADMCP
    #
    # Point them at the same path instead and the addon is installed one level
    # above where FreeCAD looks for it, so the MCP server never starts.
    fc_data = profile / "FreeCAD"
    fc_data.mkdir(parents=True, exist_ok=True)
    (fc_data / "temp").mkdir(exist_ok=True)

    env = os.environ.copy()
    env["APPDATA"] = str(profile)
    env["XDG_DATA_HOME"] = str(profile)
    env["FREECAD_USER_HOME"] = str(fc_data)
    env["FREECAD_USER_DATA"] = str(fc_data)
    env["FREECAD_USER_TEMP"] = str(fc_data / "temp")
    env["FREECAD_REPO"] = str(REPO)

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
    # Drain the launcher's output to a file. Leaving it in an unread PIPE loses
    # the diagnostics and can block the child once the buffer fills.
    launcher_log = (ev.root / "launcher.log").open("wb")
    process = subprocess.Popen(
        command, cwd=str(REPO), env=env,
        stdout=launcher_log, stderr=subprocess.STDOUT,
    )
    return process


def wait_ready(process: subprocess.Popen[bytes], timeout: float, ev: Evidence) -> bool:
    """Wait for the RPC server, independently of the launcher's lifetime.

    With --no-wait-for-mcp the launcher returns as soon as it has spawned
    FreeCAD, so its exit says nothing about readiness and must not be treated
    as failure.
    """
    deadline = time.monotonic() + timeout
    reported_exit = False
    while time.monotonic() < deadline:
        if port_is_open() and rpc_is_ready():
            return True
        if process.poll() is not None and not reported_exit:
            ev.log(f"launcher returned code {process.returncode}; still waiting for RPC")
            reported_exit = True
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
print('had_view=' + str(_view is not None))
print('view_cycles=' + str(_n if _view is not None else 0))
print('touched_after_view=' + str(bool(_doc.isTouched())))
"""


def make_document(client: Rpc, name: str, path: Path) -> tuple[bool, str]:
    code = f"""
import FreeCAD
import Part  # noqa: F401 - registers the Part::Box type
_doc = FreeCAD.newDocument({name!r})
_box = _doc.addObject('Part::Box', 'StressBox')
_box.Length = 10.0
_box.Width = 10.0
_box.Height = 10.0
_second = _doc.addObject('Part::Box', 'SecondBox')

# The collaborative set-property operation checks that value_type matches the
# property exactly. Part::Box uses App::PropertyLength (a PropertyQuantity), so
# the conflict phases edit dedicated dynamic integer properties instead --
# mirroring tests/src/App/CollaborativeSetPropertyIndependence.cpp.
_box.addProperty('App::PropertyInteger', 'AlphaValue', 'Stress', '')
_second.addProperty('App::PropertyInteger', 'BetaValue', 'Stress', '')
_box.AlphaValue = 0
_second.BetaValue = 0

_doc.recompute()
_doc.saveAs({str(path)!r})
print('created=' + _doc.Name)
print('filename=' + _doc.FileName)
print('alpha=' + str(_box.AlphaValue))
print('beta=' + str(_second.BetaValue))
"""
    return run_code(client, code)


# --------------------------------------------------------------------------
# phases
# --------------------------------------------------------------------------
def phase_view_vs_mutation(ev: Evidence, doc: str, cycles: int) -> None:
    """View activity interleaved with agent mutations.

    The two run from separate connections, but the RPC listener is a
    single-threaded SimpleXMLRPCServer and view work must stay on the GUI
    thread, so these interleave rather than overlap. That is the realistic
    shape of a user orbiting the model while an agent edits it, and it is what
    the "camera never invalidates a commit" claim is about. It is deliberately
    not described as simultaneous execution.
    """
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
            fields = parsed(message)
            if not ok:
                view_errors.append(message)
                break
            if fields.get("had_view") != "True":
                # Without a 3D view this phase would report success having done
                # nothing at all. Fail loudly instead of passing vacuously.
                view_errors.append("no 3D view was available; view activity is vacuous")
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
             ok and fields.get("had_view") == "True"
             and fields.get("touched_after_view") == "False",
             f"had_view={fields.get('had_view')} "
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
            snapshot = client.call("get_mutation_readiness", {"doc_name": doc})
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
            snapshot = client.call("get_mutation_readiness", {"doc_name": doc})
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


# Conflicts in this architecture are NOT thread races. Every RPC mutation is
# marshalled onto the GUI thread, so two clients writing at once are serialised
# and could never collide. Arbitration is done by the native edit-session layer:
# a session snapshots the semantic revisions it intends to edit, and a commit
# built on a snapshot another actor has since superseded is refused. Racing
# threads here would report success while proving nothing, so the test drives
# the real interleaving deterministically instead.
SAME_PROPERTY = """
import FreeCAD
_d = FreeCAD.getDocument(DOCNAME)
_a = _d.beginEditSession('agent-a')['session_id']
_b = _d.beginEditSession('agent-b')['session_id']
_key = [{'object': 'StressBox', 'property': 'AlphaValue'}]

# Both actors observe the same revision of the same property.
_d.snapshotForEdit(_a, _key)
_d.snapshotForEdit(_b, _key)

_args_a = {'object': 'StressBox', 'property': 'AlphaValue',
           'value_type': 'integer', 'value': '55'}
_args_b = {'object': 'StressBox', 'property': 'AlphaValue',
           'value_type': 'integer', 'value': '66'}
_pa = _d.prepareEdit(_a, 'stress-same-a', 'App.CollaborativeSetProperty', _args_a, 'stress')
_pb = _d.prepareEdit(_b, 'stress-same-b', 'App.CollaborativeSetProperty', _args_b, 'stress')

_ra = _d.commitEdit(_a, _pa)
_rb = _d.commitEdit(_b, _pb)
print('first_status=' + str(_ra.get('status')))
print('second_status=' + str(_rb.get('status')))
print('first_committed=' + str(bool(_ra.get('committed', _ra.get('status') == 'Committed'))))
print('second_committed=' + str(bool(_rb.get('committed', _rb.get('status') == 'Committed'))))
print('final_alpha=' + str(_d.getObject('StressBox').AlphaValue))
print('first_operation_id=' + str(_ra.get('operation_id')))
print('second_operation_id=' + str(_rb.get('operation_id')))
"""

INDEPENDENT_PROPERTY = """
import FreeCAD
_d = FreeCAD.getDocument(DOCNAME)
_a = _d.beginEditSession('agent-c')['session_id']
_b = _d.beginEditSession('agent-d')['session_id']

# Each actor observes only the property it intends to edit.
_d.snapshotForEdit(_a, [{'object': 'StressBox', 'property': 'AlphaValue'}])
_d.snapshotForEdit(_b, [{'object': 'SecondBox', 'property': 'BetaValue'}])

_args_a = {'object': 'StressBox', 'property': 'AlphaValue',
           'value_type': 'integer', 'value': '42'}
_args_b = {'object': 'SecondBox', 'property': 'BetaValue',
           'value_type': 'integer', 'value': '24'}
_pa = _d.prepareEdit(_a, 'stress-indep-a', 'App.CollaborativeSetProperty', _args_a, 'stress')
_pb = _d.prepareEdit(_b, 'stress-indep-b', 'App.CollaborativeSetProperty', _args_b, 'stress')

_ra = _d.commitEdit(_a, _pa)
_rb = _d.commitEdit(_b, _pb)
print('a_status=' + str(_ra.get('status')))
print('b_status=' + str(_rb.get('status')))
print('alpha=' + str(_d.getObject('StressBox').AlphaValue))
print('beta=' + str(_d.getObject('SecondBox').BetaValue))
print('a_operation_id=' + str(_ra.get('operation_id')))
print('b_operation_id=' + str(_rb.get('operation_id')))
"""


def phase_conflicts(ev: Evidence, doc: str) -> None:
    ev.log("phase D: targeted conflict detection (native edit sessions)")
    client = proxy()

    ok, message = run_code(client, SAME_PROPERTY.replace("DOCNAME", repr(doc)))
    fields = parsed(message)
    ev.record["phases"]["same_property"] = {"ok": ok, "fields": fields, "raw": message}
    ev.note_operation(fields.get("first_operation_id"))
    ev.note_operation(fields.get("second_operation_id"))
    if not ev.check("same-property scenario ran", ok, message[:300]):
        return
    ev.check("first same-property commit succeeded",
             fields.get("first_committed") == "True",
             f"status={fields.get('first_status')}")
    ev.check("second same-property commit was refused as a conflict",
             fields.get("second_committed") == "False",
             f"status={fields.get('second_status')}")
    ev.check("the losing commit did not land (exactly-once)",
             fields.get("final_alpha") == "55",
             f"final_alpha={fields.get('final_alpha')}, expected 55")

    ok, message = run_code(client, INDEPENDENT_PROPERTY.replace("DOCNAME", repr(doc)))
    fields = parsed(message)
    ev.record["phases"]["independent_property"] = {"ok": ok, "fields": fields, "raw": message}
    ev.note_operation(fields.get("a_operation_id"))
    ev.note_operation(fields.get("b_operation_id"))
    if not ev.check("independent-property scenario ran", ok, message[:300]):
        return
    ev.check("independent edits did not conflict with each other",
             fields.get("a_status") == fields.get("b_status") != "None",
             f"a={fields.get('a_status')} b={fields.get('b_status')}")
    ev.check("both independent edits landed exactly once",
             fields.get("alpha") == "42" and fields.get("beta") == "24",
             f"alpha={fields.get('alpha')} beta={fields.get('beta')}")


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
        isolated = str(profile).lower() in reported.lower()
        ev.check("GUI is using the isolated profile, not the real one",
                 isolated, reported)
        if not isolated:
            # Never run the stress against a real user profile. Stop before
            # touching a single document; FreeCAD rewrites user.cfg on a clean
            # exit, so this instance is killed outright rather than closed.
            ev.log("FATAL: profile isolation failed; aborting before any document work")
            ev.record["environment"]["isolation_failed_reported_path"] = reported
            ev.write()
            return 2

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
            # If the GUI turned out to be on the real profile, do NOT close it
            # gracefully: a clean exit rewrites user.cfg in that profile. Kill
            # it instead, so a failed isolation check leaves no trace.
            graceful = not ev.record["environment"].get(
                "isolation_failed_reported_path"
            )
            ev.log("shutting the GUI down" if graceful
                   else "killing the GUI (isolation failed; no clean exit)")
            if graceful:
                with contextlib.suppress(Exception):
                    run_code(proxy(),
                             "import FreeCADGui; FreeCADGui.getMainWindow().close()")
                time.sleep(3)
            # Terminating the launcher does NOT stop FreeCAD -- the launcher
            # spawns it (often via `pixi run`) and returns. Kill the real
            # process by the PID the RPC server reported, or the GUI is leaked.
            with contextlib.suppress(Exception):
                process.terminate()
            # Kill on liveness, NOT on rpc_is_ready(). A closing window stops
            # answering RPC long before the process exits, so gating the kill
            # on readiness leaves the GUI running and holding build outputs.
            gui_pid = ev.record["environment"].get("freecad_pid")
            if gui_pid and process_alive(int(gui_pid)):
                ev.log(f"GUI still alive; terminating FreeCAD pid {gui_pid}")
                with contextlib.suppress(Exception):
                    subprocess.run(
                        ["taskkill", "/PID", str(gui_pid), "/F", "/T"]
                        if sys.platform == "win32"
                        else ["kill", "-9", str(gui_pid)],
                        capture_output=True, check=False,
                    )
                time.sleep(2)
                if process_alive(int(gui_pid)):
                    ev.log(f"WARNING: FreeCAD pid {gui_pid} survived termination")
            if port_is_open() and rpc_is_ready():
                ev.log("WARNING: an MCP server is still answering after shutdown")
        path = ev.write()
        ev.log(f"evidence: {path}")
        ev.log(f"GUI_STRESS_RESULT: {ev.record['verdict']}")

    return 0 if not ev.failures() else 1


if __name__ == "__main__":
    raise SystemExit(main())
