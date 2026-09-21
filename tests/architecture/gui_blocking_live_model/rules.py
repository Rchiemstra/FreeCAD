# SPDX-License-Identifier: LGPL-2.1-or-later
"""Search rules for the GUI blocking and live-model ingress inventory.

Each category below is a pair of deliberately narrow regular expressions -- one
applied to C++ source and one to Python source -- both run *after* comments and
string/char/raw literals have been masked out (see :mod:`scanner`). Keeping the
patterns narrow is what makes the inventory reproducible and keeps the
false-positive surface small.

Scope
-----
The inventory covers production GUI source only:

* ``src/Gui`` (the GUI framework), and
* every ``Gui`` directory under ``src/Mod`` (workbench GUI code), including the
  nested Python GUI directories such as ``src/Mod/CAM/Path/*/Gui``.

Both C++ (``.cpp``/``.h``/``.hpp``) and Python (``.py``) GUI source are
scanned. Python-only workbenches whose GUI code is *not* organised under a
``Gui`` directory (for example ``src/Mod/Draft`` and ``src/Mod/BIM``, where App
and GUI Python share the top-level package) are out of scope for this pass: the
scanner cannot separate their GUI Python from their App Python by directory, so
they need a module-aware scanner and are tracked as a bounded follow-up task in
the report. Two categories are C++-only by nature -- ``blocking-invokes`` (the
Qt ``BlockingQueuedConnection`` connection type) and ``update-data-provider``
(the ``ViewProvider::updateData(const App::Property*)`` interface) -- and carry
a ``None`` Python pattern to record that explicitly.

Dispositions
------------
Every finding is stamped with a migration disposition:

* ``migrate``    -- must move off the current mechanism to a nonblocking or
                    committed-state/value-only route before the nonblocking
                    cutover can complete.
* ``investigate``-- needs per-site triage; the mechanism is suspect but some
                    uses are legitimate (for example a shutdown-time join, or a
                    one-shot ``processEvents`` flush).
* ``accepted``   -- legitimate and expected to remain; recorded so reviewers
                    can see it was considered, not missed.

Defaults are assigned per category below; a finding can be re-classified in the
report after triage. The defaults encode the *baseline* migration posture.
"""

from __future__ import annotations

from dataclasses import dataclass

#: C++ source suffixes.
CPP_SUFFIXES: frozenset[str] = frozenset({".cpp", ".h", ".hpp"})

#: Python source suffixes.
PY_SUFFIXES: frozenset[str] = frozenset({".py"})

#: All source suffixes the scanner reads.
SOURCE_SUFFIXES: frozenset[str] = CPP_SUFFIXES | PY_SUFFIXES

#: Directories excluded from the scan (template code is not production GUI).
EXCLUDED_DIR_NAMES: frozenset[str] = frozenset({"_TEMPLATE_"})

#: Workbenches whose GUI directory is test/infrastructure code rather than
#: production GUI. ``src/Mod/Test/Gui`` hosts the unit-test workbench, not a
#: shipped workbench surface.
EXCLUDED_WORKBENCHES: frozenset[str] = frozenset({"Test"})

#: Individual test-harness files excluded from the scan. These are compiled into
#: the GUI library (or importable) but are test infrastructure, not a shipped
#: GUI surface; the rationale mirrors ``EXCLUDED_WORKBENCHES``.
EXCLUDED_FILES: frozenset[str] = frozenset(
    {
        # GUI test-command harness (FCCmdTest*/CmdTestProgress*/BarThread) whose
        # QWaitCondition/QThreadPool waits simulate user activity for the test
        # framework rather than block a production GUI code path.
        "src/Gui/CommandTest.cpp",
        # GUI test support module (QtGui test helpers); not production GUI.
        "src/Gui/FreeCADGuiTest.py",
    }
)


@dataclass(frozen=True)
class Category:
    """A single search rule: name, prose rationale, patterns, and disposition.

    ``cpp_pattern``/``py_pattern`` are compiled against the *masked* source of
    the matching language; ``None`` marks a category that has no expression in
    that language.
    """

    key: str
    title: str
    description: str
    cpp_pattern: str | None
    py_pattern: str | None
    default_disposition: str


# Keep categories in a stable, documented order.
CATEGORIES: tuple[Category, ...] = (
    Category(
        key="thread-waits",
        title="Thread/process blocking waits",
        description=(
            "Synchronous waits for a worker thread, helper process, condition "
            "variable, or future to finish while the GUI thread is blocked. "
            "C++: QProcess/QFuture/QThreadPool waitForFinished, waitForDone, "
            "waitForStarted and waitForBytesWritten; QThread::wait; pthread_join; "
            "and the instance member wait (thread->wait() and QWaitCondition().wait). "
            "Python: the same QProcess/QThread waitFor* family and instance .wait() "
            "(e.g. subprocess/QThread.wait). String joins (QString::join, str.join, "
            "os.path.join) and std::thread::detach are explicitly not matches."
        ),
        cpp_pattern=(
            r"\b(?:waitForFinished|waitForDone|waitForStarted|waitForBytesWritten)[^\S\n]*\(|"
            r"QThread[^\S\n]*::[^\S\n]*wait[^\S\n]*\(|"
            r"pthread_join[^\S\n]*\(|"
            r"(?:->|\.)wait[^\S\n]*\("
        ),
        py_pattern=(
            r"\.(?:waitForFinished|waitForDone|waitForStarted|waitForBytesWritten)[^\S\n]*\(|"
            r"\.wait[^\S\n]*\("
        ),
        default_disposition="investigate",
    ),
    Category(
        key="blocking-invokes",
        title="Cross-thread blocking invokes",
        description=(
            "Queued cross-thread invocation that blocks the calling (GUI) thread "
            "until the receiver executes. Detected by the Qt::BlockingQueuedConnection "
            "connection type (C++), or the PySide BlockingQueuedConnection enum "
            "(Python), which is the only portable way FreeCAD requests a blocking "
            "queued dispatch. No Python GUI site currently uses it; the rule is "
            "recorded so any future use is inventoried."
        ),
        cpp_pattern=r"Qt[^\S\n]*::[^\S\n]*BlockingQueuedConnection",
        py_pattern=r"\bBlockingQueuedConnection\b",
        default_disposition="migrate",
    ),
    Category(
        key="process-events-polling",
        title="Manual event-loop polling",
        description=(
            "Explicit processEvents() pumping of the Qt event loop, or the Python "
            "FreeCADGui/Gui.updateGui() wrapper that performs the same re-entrant "
            "flush. These sites re-enter the event loop from inside a handler and "
            "are the classic source of GUI-thread reentrancy and long-duration busy "
            "loops. Masked comments and literals are not matches."
        ),
        cpp_pattern=r"\bprocessEvents[^\S\n]*\(",
        py_pattern=(
            r"\bprocessEvents[^\S\n]*\(|" r"(?:FreeCADGui|Gui)[^\S\n]*\.[^\S\n]*updateGui[^\S\n]*\("
        ),
        default_disposition="investigate",
    ),
    Category(
        key="direct-recompute",
        title="Direct synchronous document recompute",
        description=(
            "Direct calls to Document/DocumentObject::recompute from GUI code "
            "(C++ ``.recompute(``/``->recompute(``, Python ``.recompute(``). "
            "These run the model recompute synchronously on the GUI thread and "
            "must move onto the per-document execution lane."
        ),
        cpp_pattern=r"(?:\.|->)recompute[^\S\n]*\(",
        py_pattern=r"\.recompute[^\S\n]*\(",
        default_disposition="migrate",
    ),
    Category(
        key="live-app-dereference",
        title="Live App object dereference",
        description=(
            "Reaching into global application state for a live App::Document or "
            "App::DocumentObject handle and dereferencing it directly, bypassing "
            "the committed presentation state. C++: App::GetApplication() document/"
            "object accessors (getActiveDocument, getDocument, getDocuments, "
            "getDocumentOrActive, getDocumentByPath), including the multi-line "
            "receiver-chain form. Python: App/FreeCAD.ActiveDocument and "
            "App/FreeCAD.getDocument(). Gui.ActiveDocument (the GUI-side document "
            "handle) is tracked separately, as is the model-layer src/App core."
        ),
        cpp_pattern=(
            r"App::GetApplication\s*\(\s*\)\s*\.\s*"
            r"(?:getActiveDocument|getDocuments|getDocumentOrActive|getDocumentByPath|getDocument)"
            r"[^\S\n]*\("
        ),
        py_pattern=(
            r"(?:App|FreeCAD)[^\S\n]*\.[^\S\n]*" r"(?:ActiveDocument\b|getDocument[^\S\n]*\()"
        ),
        default_disposition="investigate",
    ),
    Category(
        key="live-reference-callback",
        title="Live-reference lifecycle callbacks",
        description=(
            "Subscriptions and handlers for the state-change signals that deliver "
            "live object references into GUI code. C++: the document-object "
            "signal/slot identifiers Changed, Change, Touched, Deleted, Delete, "
            "BeforeChange, Recomputed (plus the ChangedView/DeletedView view "
            "variants). Python: addObserver/removeObserver subscriptions (Selection "
            "and Document observers) that register callbacks receiving live "
            "references. Creation/rename/activate lifecycle signals and tree/"
            "selection widget navigation signals are out of scope and tracked "
            "separately. Pure signal member declarations are excluded individually "
            "in exclusions.json."
        ),
        cpp_pattern=(
            r"\b(?:signal|slot)"
            r"(?:Changed|Change|Touched|Deleted|Delete|BeforeChange|Recomputed)"
            r"(?:View)?Object\b"
        ),
        py_pattern=r"\b(?:addObserver|removeObserver)[^\S\n]*\(",
        default_disposition="migrate",
    ),
    Category(
        key="update-data-provider",
        title="ViewProvider updateData providers",
        description=(
            "ViewProvider updateData(const App::Property*) declarations and "
            "overrides -- both the qualified definitions and the inline "
            "declarations/definitions (``void updateData(const App::Property*) "
            "override``). These are the presentation providers that read live "
            "model properties and must be migrated to bounded presentation "
            "adapters that consume committed state. Anchoring on the concrete "
            "signature means base-class delegation calls (``X::updateData(prop)``) "
            "and the unrelated QAbstractItemModel-style PropertyItem::updateData() "
            "are not matched. Python has no equivalent interface (None pattern)."
        ),
        cpp_pattern=r"\bupdateData[^\S\n]*\([^\S\n]*const[^\S\n]+App::Property",
        py_pattern=None,
        default_disposition="migrate",
    ),
)

#: Ordered migration dispositions and their meaning (see module docstring).
DISPOSITIONS: tuple[str, ...] = ("migrate", "investigate", "accepted")

CATEGORY_BY_KEY: dict[str, Category] = {category.key: category for category in CATEGORIES}
