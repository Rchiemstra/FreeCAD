# SPDX-License-Identifier: LGPL-2.1-or-later
"""Search rules for the GUI blocking and live-model ingress inventory.

Each category below is a single, deliberately narrow regular expression over
C++ source *after* comments and string/char/raw literals have been masked out
(see :mod:`scanner`). Keeping the patterns narrow is what makes the inventory
reproducible and keeps the false-positive surface small.

Scope
-----
The inventory covers production GUI C++ source only:

* ``src/Gui`` (the GUI framework), and
* ``src/Mod/<Workbench>/Gui`` (workbench GUI code).

Python workbench GUI code (for example ``src/Mod/Draft``, ``src/Mod/BIM``) is
deliberately out of scope for this pass: it needs a Python-aware scanner and is
tracked separately by the ``Isolate Python and the GIL from native GUI
activity`` and ``Convert Python observers to queued value events`` work items.
The document model core (``src/App``) is also out of scope; this inventory is
about *GUI* blocking and *live-model ingress* from GUI code.

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

#: File suffixes treated as C++ source.
SOURCE_SUFFIXES: frozenset[str] = frozenset({".cpp", ".h", ".hpp"})

#: Directories excluded from the scan (template code is not production GUI).
EXCLUDED_DIR_NAMES: frozenset[str] = frozenset({"_TEMPLATE_"})

#: Workbenches whose GUI directory is test/infrastructure code rather than
#: production GUI. ``src/Mod/Test/Gui`` hosts the unit-test workbench, not a
#: shipped workbench surface.
EXCLUDED_WORKBENCHES: frozenset[str] = frozenset({"Test"})


@dataclass(frozen=True)
class Category:
    """A single search rule: a name, prose rationale, regex, and disposition."""

    key: str
    title: str
    description: str
    #: Compiled against masked C++ source; one match == one candidate finding.
    pattern: str
    default_disposition: str


# Keep categories in a stable, documented order.
CATEGORIES: tuple[Category, ...] = (
    Category(
        key="thread-waits",
        title="Thread/process blocking waits",
        description=(
            "Synchronous waits for a worker thread, helper process, or future "
            "to finish while the GUI thread is blocked. Includes "
            "QProcess::waitForFinished, QFuture/QFutureWatcher::waitForFinished, "
            "QThread::wait and pthread_join. String joins (QStringList::join, "
            "QString::join) and std::thread::detach are explicitly not matches."
        ),
        pattern=r"\bwaitForFinished[^\S\n]*\(|QThread::wait[^\S\n]*\(|pthread_join[^\S\n]*\(",
        default_disposition="investigate",
    ),
    Category(
        key="blocking-invokes",
        title="Cross-thread blocking invokes",
        description=(
            "Queued cross-thread invocation that blocks the calling (GUI) thread "
            "until the receiver executes. Detected by the Qt::BlockingQueuedConnection "
            "connection type, which is the only portable way FreeCAD requests a "
            "blocking queued dispatch."
        ),
        pattern=r"Qt[^\S\n]*::[^\S\n]*BlockingQueuedConnection",
        default_disposition="migrate",
    ),
    Category(
        key="process-events-polling",
        title="Manual event-loop polling",
        description=(
            "Explicit processEvents() pumping of the Qt event loop. These sites "
            "re-enter the event loop from inside a handler and are the classic "
            "source of GUI-thread reentrancy and long-duration busy loops. Masked "
            "comments and literals are not matches."
        ),
        pattern=r"\bprocessEvents[^\S\n]*\(",
        default_disposition="investigate",
    ),
    Category(
        key="direct-recompute",
        title="Direct synchronous document recompute",
        description=(
            "Direct calls to Document/DocumentObject::recompute from GUI code. "
            "These run the model recompute synchronously on the GUI thread and "
            "must move onto the per-document execution lane."
        ),
        pattern=r"(?:\.|->)recompute[^\S\n]*\(",
        default_disposition="migrate",
    ),
    Category(
        key="live-app-dereference",
        title="Live App object dereference",
        description=(
            "Reaching into global application state for a live App::Document or "
            "App::DocumentObject handle and dereferencing it directly, bypassing "
            "the committed presentation state. Detected by the App::GetApplication "
            "document/object accessors."
        ),
        pattern=(
            r"App::GetApplication[^\S\n]*\([^\S\n]*\)[^\S\n]*\.[^\S\n]*"
            r"(?:getActiveDocument|getDocument|getDocumentObject|"
            r"getDocumentGroup|getDocumentByTag)[^\S\n]*\("
        ),
        default_disposition="investigate",
    ),
    Category(
        key="live-reference-callback",
        title="Live-reference lifecycle callbacks",
        description=(
            "Subscriptions and handlers for the document-object state-change "
            "signals that deliver live object references into GUI code: "
            "signal/slot Changed, Change, Touched, Deleted, Delete, "
            "BeforeChange, Recomputed (plus the ChangedView/DeletedView view "
            "variants). These callbacks fire with live App::DocumentObject or "
            "ViewProvider references and must become queued value events. "
            "Creation/rename/activate lifecycle signals (New, Relabel, "
            "Activated, Created) and tree-navigation signals (Highlight, "
            "Expand, Scroll) are intentionally out of scope and tracked "
            "separately. Pure signal member declarations are excluded "
            "individually in exclusions.json."
        ),
        pattern=(
            r"\b(?:signal|slot)"
            r"(?:Changed|Change|Touched|Deleted|Delete|BeforeChange|Recomputed)"
            r"(?:View)?Object\b"
        ),
        default_disposition="migrate",
    ),
    Category(
        key="update-data-provider",
        title="ViewProvider updateData providers",
        description=(
            "ViewProvider::updateData(const App::Property*) overrides. These are "
            "the presentation providers that read live model properties and must "
            "be migrated to bounded presentation adapters that consume committed "
            "state. The regex anchors on the concrete override signature so that "
            "base-class delegation calls (X::updateData(prop)) and the unrelated "
            "QAbstractItemModel-style PropertyItem::updateData() are not matched."
        ),
        pattern=r"::updateData[^\S\n]*\([^\S\n]*const[^\S\n]+App::Property",
        default_disposition="migrate",
    ),
)

#: Ordered migration dispositions and their meaning (see module docstring).
DISPOSITIONS: tuple[str, ...] = ("migrate", "investigate", "accepted")

CATEGORY_BY_KEY: dict[str, Category] = {category.key: category for category in CATEGORIES}
