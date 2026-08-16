"""Explicit 9p save-compatibility gate.

Run under FreeCADCmd with a target directory that is deliberately ON the 9p
bind mount. This gate exists because ordinary App/GUI tests now run from a
temporary working directory outside 9p: moving that CWD keeps unrelated tests
honest, it does not fix or excuse 9p support, so 9p is covered here explicitly.

What it proves, on a filesystem where a rename performed while a descriptor is
still held leaves the destination name unresolvable:

* Save As reports Written and file_written, not a failure;
* the document adopts the destination as its FileName;
* the installed bytes are a valid FCStd ZIP containing Document.xml;
* repeated saves do not accumulate .displaced predecessors.

Never call sys.exit() here: under FreeCADCmd that discards the script's output.
The final ``9P_GATE_RESULT:`` line is the machine-readable verdict, and
run_9p_save_compat_gate.sh turns it into a process exit code.
"""

import os
import zipfile

import FreeCAD

TARGET_DIR = os.environ.get("FC_9P_GATE_DIR", "")

failures = []


def check(condition, description, detail=""):
    if condition:
        print("  ok   - %s" % description)
    else:
        print("  FAIL - %s %s" % (description, detail))
        failures.append(description)


def displaced_entries():
    return sorted(n for n in os.listdir(TARGET_DIR) if n.endswith(".displaced"))


print("9p save-compatibility gate")

if not TARGET_DIR:
    print("  FAIL - FC_9P_GATE_DIR is not set")
    failures.append("FC_9P_GATE_DIR is not set")
else:
    os.makedirs(TARGET_DIR, exist_ok=True)
    target = os.path.join(TARGET_DIR, "gate.FCStd")
    print("  target: %s" % target)

    doc = FreeCAD.newDocument("NinePGate")
    doc.addObject("App::VarSet", "VarSet")

    # --- Save As onto a pre-existing canonical file (the reproduced failure) --
    with open(target, "wb") as handle:
        handle.write(b"PRE-EXISTING CANONICAL CONTENT")

    outcome = doc.saveAsWithOutcome(target, True)
    check(str(outcome.get("save_disposition")) == "written",
          "Save As reports Written",
          "got %r message=%r" % (outcome.get("save_disposition"), outcome.get("message")))
    check(bool(outcome.get("file_written")), "file_written is true")
    check(bool(outcome.get("success")), "outcome reports success",
          "message=%r" % (outcome.get("message"),))
    check(os.path.abspath(doc.FileName) == os.path.abspath(target),
          "document adopted the destination as FileName",
          "got %r" % (doc.FileName,))

    check(zipfile.is_zipfile(target), "installed file is a ZIP archive")
    if zipfile.is_zipfile(target):
        with zipfile.ZipFile(target) as archive:
            names = archive.namelist()
            check("Document.xml" in names,
                  "archive contains Document.xml",
                  "names=%r" % (names[:5],))
            check(archive.testzip() is None, "archive has no corrupt members")

    # --- Repeated saves must not accumulate displaced predecessors -----------
    for iteration in range(5):
        doc.addObject("App::VarSet", "Extra%d" % iteration)
        doc.recompute()
        repeat = doc.saveWithOutcome()
        if str(repeat.get("save_disposition")) not in ("written", "unchanged"):
            check(False,
                  "repeat save %d reports a normal disposition" % iteration,
                  "got %r message=%r"
                  % (repeat.get("save_disposition"), repeat.get("message")))

    leftover = displaced_entries()
    check(not leftover,
          "no .displaced predecessors accumulated after repeated saves",
          "found %r" % (leftover,))

    # --- Reopen and confirm the persisted content ---------------------------
    FreeCAD.closeDocument(doc.Name)
    reopened = FreeCAD.openDocument(target)
    check(reopened.getObject("VarSet") is not None,
          "reopened document still contains the saved object")
    FreeCAD.closeDocument(reopened.Name)

print("")
if failures:
    print("9P_GATE_RESULT: FAILED (%d) %s" % (len(failures), "; ".join(failures)))
else:
    print("9P_GATE_RESULT: PASSED")
