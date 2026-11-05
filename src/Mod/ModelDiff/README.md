# Visual model diff

Compare saved FreeCAD documents with:

```sh
FreeCAD --diff local.FCStd remote.FCStd
FreeCADCmd --diff local.FCStd remote.FCStd
```

LOCAL is the old version; REMOTE is the new version. The GUI opens a new,
unsaved **Model difference** document and the Report view. Green means added
material, red means removed material, and faint grey means unchanged material.
The tree groups results under Added, Removed, Changed, and Unchanged. These
objects contain independent geometry, so the input files can be removed after
comparison. The tool does not save or recompute either source model.
The comparison always gets its own process/window, including when
`--single-instance` is enabled. Closing the result or quitting discards it
without a save prompt. Use **Save As** to keep a copy explicitly.

## Git difftool

Configure the path to the GUI executable (use its absolute path if needed):

```sh
git config --global diff.tool freecad
git config --global difftool.freecad.cmd 'FreeCAD --diff "$LOCAL" "$REMOTE"'
git config --global difftool.prompt false
git difftool --no-prompt HEAD~1 HEAD -- model.FCStd
```

Keep the quotes around `$LOCAL` and `$REMOTE`: Git often uses temporary paths,
and model filenames can contain spaces. Close the FreeCAD diff window to let
Git finish comparing that file. Use `FreeCADCmd` in the command above for a
text-only report.

## Python API

```python
from modeldiff import compare_files, show_result

result = compare_files("local.FCStd", "remote.FCStd")
print(result.report())
print(result.identical)
diff_document = show_result(result)  # also works without the GUI

# Compare just one chosen pair (names may differ, and may be hidden history).
from modeldiff import compare_objects
pair = compare_objects("local.FCStd", "remote.FCStd", "OldPad", "NewPad")
show_result(pair)
```

In GUI code use `compare_files_async()` or
`compare_objects_async(local, remote, local_name, remote_name=None)`. They return
a job with `finished` (a Qt signal), `done`, `result`, `error` and `cancel()`:

```python
from modeldiff import compare_objects_async, show_result
job = compare_objects_async("local.FCStd", "remote.FCStd", "Pad", "Pad")
def display_pair():
    if job.error is None:
        show_result(job.result)
job.finished.connect(display_pair)
```

The event loop must keep running. Synchronous file comparison is reserved for
headless callers; GUI callers receive an error directing them to the async API.
Loading, live input reads, extraction, measurements and Booleans run in an
isolated FreeCADCmd helper, so they cannot race an open GUI document's owner.
Only detached geometry and plain values cross the boundary. Decoding also runs
off the GUI thread. CLI presentation adds objects in timer slices and shows
progress in the status bar; it does not pump nested GUI events or wait for owners.
The public headless `compare_object_pair(before, after)` engine is also used for
the whole-document 3D view. Its live arguments must belong to the calling owner.

`result.properties` maps changed document properties to `(old, new)` strings
(`None` means absent). `result.objects` contains `ObjectChange` records with
internal name, label, Added/Removed/Changed kind, changed properties, and shape
measurements `(old_volume, new_volume, old_area, new_area)` in mm³ and mm².
`result.visuals` contains detached geometry, source names, grouping, and material
roles. `result.warnings` includes Boolean failures and missing external dependencies. Both executable entry points
call this same API, including the same textual report.

Objects pair by **internal name**, so a changed label is a property change.
Saved document and object properties are read from the archive, including
binary-backed properties and individual Spreadsheet cell contents. Input
archives and their external document dependencies are copied into an isolated
temporary directory, loaded as hidden documents, and closed once comparison
finishes. Relative paths are preserved; absolute links are redirected to the
private copies. No source document is reused, even if it is already
open with unsaved edits. Missing external dependencies do not prevent reading
saved properties and cached geometry. A missing link without cached geometry
can only contribute a textual difference; save and recompute with dependencies
available before committing if you need fresh linked geometry.

Only visible result geometry is rendered. Body history is represented by its
Body tip, and hidden intermediate features are omitted from the 3D difference
while their property changes remain in the report without geometry extraction,
copying, serialization, measurement or Boolean work. For files saved without GUI
visibility data, standard Part Boolean inputs are inferred as hidden. Container
placements are included. Closed solids use REMOTE minus LOCAL, LOCAL minus
REMOTE, and their common volume. A failed or invalid Boolean falls back to two
colour overlays and adds a warning to the report. Faces, wires, sketches, and
meshes use overlays. This is a two-file comparison, with no merge operation.

Possible follow-ups are an archive-only quick mode that never loads documents,
and a separate object/property comparison dialog with an on-demand comparison
button. Neither is part of this implementation.

## Tests

The module registers `TestModelDiff` in `Init.py` and `TestModelDiffGui` in
`InitGui.py` for the standard FreeCAD CI discovery. Run:

```sh
QT_QPA_PLATFORM=offscreen FreeCADCmd -t TestModelDiff
xvfb-run -a FreeCAD -t TestModelDiffGui
```
