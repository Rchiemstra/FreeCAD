"""Explicit 9p save-compatibility gate.

Run under FreeCADCmd with a target directory that is deliberately ON the 9p
bind mount. This gate exists because ordinary App/GUI tests now run from a
temporary working directory outside 9p: moving that CWD keeps unrelated tests
honest, it does not fix or excuse 9p support, so 9p is covered here explicitly.

What it proves, on a filesystem where a rename performed while a descriptor is
still held leaves the destination name unresolvable:

* Save As reports Written and file_written, not a failure;
* durability is verified rather than quietly skipped;
* no false "save failed" is reported after valid bytes were installed;
* the document adopts the destination as its FileName;
* the installed bytes are a valid FCStd ZIP containing Document.xml;
* an immediate second save reports Unchanged and rewrites nothing, proved by
  an unchanged SHA-256 of the installed file;
* a save after an edit reports Written and really does replace those bytes;
* repeated saves do not accumulate .displaced predecessors;
* nothing but the document and its lock anchor is left behind, so recovery
  evidence matches doc/save-artifact-contract.md.

Never call sys.exit() here: under FreeCADCmd that discards the script's output.
The final ``9P_GATE_RESULT:`` line is the machine-readable verdict, and
run_9p_save_compat_gate.sh turns it into a process exit code.
"""

import hashlib
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


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def stray_artifacts(document_name):
    """Everything in the gate directory that is neither the document nor its lock anchor.

    The zero-byte ``<document>.FreeCAD-save.lock`` is a persistent rendezvous
    name for a kernel byte-range lock, not a leftover, so it is excluded here by
    contract rather than by accident. See doc/save-artifact-contract.md.
    """
    allowed = {document_name, document_name + ".FreeCAD-save.lock"}
    return sorted(n for n in os.listdir(TARGET_DIR) if n not in allowed)


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

    check(bool(outcome.get("durability_verified")),
          "durability is reported truthfully, not skipped on 9p",
          "got %r warnings=%r"
          % (outcome.get("durability_verified"), outcome.get("warnings")))

    # A save that physically installed valid bytes must never come back as a
    # failure. This is the exact false-negative the 9p descriptor behaviour used
    # to produce: the bytes land, then fstat on the retained descriptor fails.
    check(not outcome.get("error_code") and str(outcome.get("resulting_state")) == "clean",
          "no false save failure after valid bytes were installed",
          "error_code=%r state=%r" % (outcome.get("error_code"), outcome.get("resulting_state")))

    installed_digest = sha256_of(target)
    check(installed_digest == sha256_of(target),
          "installed SHA-256 is stable across reads")

    # --- An immediate unchanged save must not rewrite the installed bytes ----
    unchanged = doc.saveWithOutcome()
    check(str(unchanged.get("save_disposition")) == "unchanged",
          "an immediate second save reports Unchanged",
          "got %r" % (unchanged.get("save_disposition"),))
    check(not unchanged.get("file_written"), "an Unchanged save reports file_written false")
    check(sha256_of(target) == installed_digest,
          "an Unchanged save leaves the installed bytes byte-identical")

    # --- An edit followed by a save must report Written and change the bytes -
    doc.addObject("App::VarSet", "AfterEdit")
    doc.recompute()
    edited = doc.saveWithOutcome()
    check(str(edited.get("save_disposition")) == "written",
          "a save after an edit reports Written",
          "got %r message=%r" % (edited.get("save_disposition"), edited.get("message")))
    check(bool(edited.get("file_written")), "the edited save reports file_written true")
    edited_digest = sha256_of(target)
    check(edited_digest != installed_digest,
          "the edited save actually replaced the installed bytes")
    check(zipfile.is_zipfile(target), "the file installed by the edited save is still a valid ZIP")

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

    # --- Recovery evidence must match the artifact contract -----------------
    # A clean save leaves nothing to recover: no EphemeralPartial .tmp, no
    # VerifiedSerialization, no DisplacedCanonical, no .cas-* evidence. The lock
    # anchor is excluded by contract, not because it happened to be there.
    strays = stray_artifacts(os.path.basename(target))
    check(not strays,
          "no recovery or temporary artifacts remain after successful saves",
          "found %r" % (strays,))

    final = doc.saveWithOutcome()
    check(not final.get("warnings"),
          "a successful save publishes no recovery warnings",
          "warnings=%r" % (final.get("warnings"),))

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
