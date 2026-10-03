# SPDX-License-Identifier: LGPL-2.1-or-later

"""Wait for document recompute without blocking the GUI thread."""

from __future__ import annotations

import os
import threading
import time
from typing import Any, Callable

import FreeCAD


def _pump_gui_until(
    predicate,
    timeout_seconds: float,
    describe: Callable[[], Any] | None = None,
) -> None:
    from PySide import QtCore

    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        QtCore.QCoreApplication.processEvents(QtCore.QEventLoop.AllEvents, 50)
        if getattr(FreeCAD, "GuiUp", False):
            try:
                import FreeCADGui

                FreeCADGui.updateGui()
            except Exception:
                pass
        if predicate():
            return
        time.sleep(0.01)

    message = "document recompute timed out on the GUI thread"
    if describe is not None:
        try:
            message += f": {describe()}"
        except Exception as exc:
            message += f" (state unavailable: {exc!r})"
    raise RuntimeError(message)


def _terminal_recompute_state(handle: Any) -> str | None:
    status = handle.status()
    if isinstance(status, dict):
        recompute = status.get("recompute")
        if isinstance(recompute, dict):
            state = recompute.get("state")
            if isinstance(state, str):
                return state
        state = status.get("state")
        if isinstance(state, str):
            return state
    return None


def _readiness_report(document) -> dict:
    report = dict(document.getMutationReadiness())
    report["object_states"] = [f"{obj.Name} {obj.State}" for obj in document.Objects if obj.State][
        :10
    ]
    return report


def wait_for_mutation_ready(document, timeout_seconds: float = 120.0) -> None:
    """Pump Qt until App reports the document is ready for GUI-side mutations."""

    def ready() -> bool:
        readiness = document.getMutationReadiness()
        if isinstance(readiness, dict):
            return bool(readiness.get("ready"))
        return bool(getattr(readiness, "ready", False))

    _pump_gui_until(ready, timeout_seconds, lambda: _readiness_report(document))


def recompute_document(document, timeout_seconds: float = 120.0) -> None:
    if not getattr(FreeCAD, "GuiUp", False):
        document.recompute()
        return

    import FreeCADGui

    if FreeCAD.activeDocument() != document:
        FreeCAD.setActiveDocument(document.Name)

    if not document.mustExecute():
        wait_for_mutation_ready(document, timeout_seconds)
        return

    # Python onChanged hooks are replayed after the commit, so a hook that
    # touches another object (e.g. a window touching its host wall) leaves work
    # that a synchronous Document.recompute() would have absorbed in the same
    # pass. Run a few passes before waiting for the document to become ready.
    for _ in range(3):
        handle = _retry_while_lane_busy(document.recomputeAsync, timeout_seconds)

        def settled() -> bool:
            if not handle.done():
                return False
            if not document.mustExecute():
                return True
            state = _terminal_recompute_state(handle)
            if state in (None, "completed"):
                return True
            # Like Document.recompute(), failed features stay touched; the
            # recompute is over once the document has left the commit.
            return state == "partial_failure" and _left_commit(document)

        _pump_gui_until(
            settled,
            timeout_seconds,
            lambda: {"recompute": handle.status(), "readiness": _readiness_report(document)},
        )

        state = _terminal_recompute_state(handle)
        if state in ("cancelled", "cancelling"):
            raise RuntimeError(f"document recompute failed: {handle.status()}")

        FreeCADGui.updateGui()
        if state == "partial_failure":
            # The caller inspects the failed features; the document cannot
            # become ready while they are invalid.
            return
        if not document.mustExecute():
            break
    wait_for_mutation_ready(document, timeout_seconds)


def _left_commit(document) -> bool:
    readiness = document.getMutationReadiness()
    busy_flags = (
        "recomputing",
        "commit_barrier",
        "notification_replay",
        "pending_removal",
    )
    return not any(readiness.get(flag) for flag in busy_flags)


def _techdraw_draft_view_svg_head() -> str:
    return (
        '<svg\n\txmlns="http://www.w3.org/2000/svg" version="1.1"\n'
        '\txmlns:freecad="https://www.freecad.org/wiki/index.php?title=Svg_Namespace">\n'
    )


def _techdraw_draft_view_svg_tail() -> str:
    return "\n</svg>"


def sync_techdraw_draft_view_symbols(document) -> None:
    """Fill empty TechDraw DraftView symbols on the GUI thread (post owner recompute)."""
    if not getattr(FreeCAD, "GuiUp", False):
        return

    import Draft

    for obj in document.Objects:
        if obj.TypeId != "TechDraw::DrawViewDraft":
            continue
        source = getattr(obj, "Source", None)
        if not source or getattr(obj, "Symbol", ""):
            continue
        svg_body = Draft.get_svg(
            source,
            scale=obj.Scale,
            linewidth=obj.LineWidth,
            fontsize=obj.FontSize,
            direction=obj.Direction,
            linestyle=obj.LineStyle,
            color=obj.Color,
            linespacing=obj.LineSpacing,
            techdraw=True,
            override=obj.OverrideStyle,
        )
        obj.Symbol = _techdraw_draft_view_svg_head() + svg_body + _techdraw_draft_view_svg_tail()


def recompute_off_gui_thread(document, timeout_seconds: float = 120.0) -> None:
    """Run a synchronous recompute off the GUI thread (lane owner path)."""
    if not getattr(FreeCAD, "GuiUp", False):
        document.recompute()
        return

    if not document.mustExecute():
        return

    error: dict[str, BaseException] = {}

    def worker() -> None:
        os.environ["FREECAD_OFF_GUI_RECOMPUTE"] = "1"
        try:
            document.recompute()
        except BaseException as exc:  # pragma: no cover - surfaced below
            error["exc"] = exc
        finally:
            os.environ.pop("FREECAD_OFF_GUI_RECOMPUTE", None)

    thread = threading.Thread(target=worker, name="document-recompute")
    thread.start()
    _pump_gui_until(lambda: not thread.is_alive(), timeout_seconds)
    thread.join(timeout=0.0)
    if error:
        raise error["exc"]

    import FreeCADGui

    FreeCADGui.updateGui()
    sync_techdraw_draft_view_symbols(document)


def refresh_document(document, timeout_seconds: float = 120.0) -> None:
    """Run Std_Refresh and wait until the document is up to date."""
    if not getattr(FreeCAD, "GuiUp", False):
        document.recompute()
        return

    import FreeCADGui

    if FreeCAD.activeDocument() != document:
        FreeCAD.setActiveDocument(document.Name)

    if not document.mustExecute():
        return

    FreeCADGui.runCommand("Std_Refresh", 0)

    def settled() -> bool:
        return not document.mustExecute()

    _pump_gui_until(settled, timeout_seconds)
    FreeCADGui.updateGui()
    sync_techdraw_draft_view_symbols(document)


def _is_lane_busy(exc: BaseException) -> bool:
    message = str(exc).lower()
    return "lane is busy" in message or "was not admitted" in message


def _retry_while_lane_busy(
    action: Callable[[], Any],
    timeout_seconds: float,
) -> Any:
    deadline = time.monotonic() + timeout_seconds
    last_error: BaseException | None = None
    while time.monotonic() < deadline:
        try:
            return action()
        except RuntimeError as exc:
            last_error = exc
            if not _is_lane_busy(exc):
                raise
        from PySide import QtCore

        QtCore.QCoreApplication.processEvents(QtCore.QEventLoop.AllEvents, 50)
        time.sleep(0.01)
    if last_error is not None:
        raise last_error
    raise RuntimeError("document execution lane stayed busy")


def save_document(document, timeout_seconds: float = 120.0) -> None:
    """Save the canonical document path from GUI tests without blocking the GUI thread."""
    if not getattr(FreeCAD, "GuiUp", False):
        document.save()
        return

    handle = _retry_while_lane_busy(document.saveAsync, timeout_seconds)
    _pump_gui_until(lambda: handle.done(), timeout_seconds)


def save_document_as(
    document,
    path: str,
    *,
    overwrite: bool = True,
    timeout_seconds: float = 120.0,
) -> None:
    """Save the document under a new path without blocking the GUI thread."""
    if not getattr(FreeCAD, "GuiUp", False):
        document.saveAs(path)
        return

    def submit_save_async():
        return document.saveAsync(path, overwrite)

    handle = _retry_while_lane_busy(submit_save_async, timeout_seconds)
    _pump_gui_until(lambda: handle.done(), timeout_seconds)


def undo_document(document, timeout_seconds: float = 120.0) -> None:
    if not getattr(FreeCAD, "GuiUp", False):
        document.undo()
        return

    handle = _retry_while_lane_busy(document.undoAsync, timeout_seconds)
    _pump_gui_until(lambda: handle.done(), timeout_seconds)


def redo_document(document, timeout_seconds: float = 120.0) -> None:
    if not getattr(FreeCAD, "GuiUp", False):
        document.redo()
        return

    handle = _retry_while_lane_busy(document.redoAsync, timeout_seconds)
    _pump_gui_until(lambda: handle.done(), timeout_seconds)


def touch_on_owner_thread(document, obj, timeout_seconds: float = 120.0) -> None:
    if not getattr(FreeCAD, "GuiUp", False):
        obj.touch()
        return

    error: dict[str, BaseException] = {}

    def worker() -> None:
        try:
            obj.touch()
        except BaseException as worker_exc:  # pragma: no cover - surfaced below
            error["exc"] = worker_exc

    thread = threading.Thread(target=worker, name="document-object-touch")
    thread.start()
    _pump_gui_until(lambda: not thread.is_alive(), timeout_seconds)
    thread.join(timeout=0.0)
    if error:
        raise error["exc"]


def write_recovery_snapshot(document, timeout_seconds: float = 120.0, **options) -> bool:
    """Write a recovery snapshot without blocking the GUI thread on the owner lane."""
    result: dict[str, bool] = {}
    error: dict[str, BaseException] = {}

    def worker() -> None:
        try:
            result["written"] = FreeCAD.writeRecoverySnapshotToTransientDir(document, **options)
        except BaseException as exc:  # pragma: no cover - surfaced through assertion below
            error["exc"] = exc

    thread = threading.Thread(target=worker, name="recovery-snapshot-write")
    thread.start()
    _pump_gui_until(lambda: not thread.is_alive(), timeout_seconds)
    thread.join(timeout=0.0)
    if error:
        raise error["exc"]
    return result.get("written", False)


def close_document(document, timeout_seconds: float = 120.0) -> None:
    """Close a document from GUI tests without blocking the GUI thread."""
    try:
        name = document.Name
    except ReferenceError:
        return

    if not getattr(FreeCAD, "GuiUp", False):
        if name in FreeCAD.listDocuments():
            FreeCAD.closeDocument(name)
        return

    if name not in FreeCAD.listDocuments():
        return

    def submit_close_async():
        if name not in FreeCAD.listDocuments():
            return None
        return FreeCAD.getDocument(name).closeAsync()

    handle = _retry_while_lane_busy(submit_close_async, timeout_seconds)
    if handle is not None:
        _pump_gui_until(lambda: handle.done(), timeout_seconds)

    # GUI ingress admits Close asynchronously; wait until the document is gone.
    _pump_gui_until(lambda: name not in FreeCAD.listDocuments(), timeout_seconds)


def close_document_by_name(name: str, timeout_seconds: float = 120.0) -> None:
    """Close the named document if it is open (see close_document)."""
    if name in FreeCAD.listDocuments():
        close_document(FreeCAD.getDocument(name), timeout_seconds)
