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
```

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
while their property changes remain in the report. For files saved without GUI
visibility data, standard Part Boolean inputs are inferred as hidden. Container
placements are included. Closed solids use REMOTE minus LOCAL, LOCAL minus
REMOTE, and their common volume. A failed or invalid Boolean falls back to two
colour overlays and adds a warning to the report. Faces, wires, sketches, and
meshes use overlays. This is a two-file comparison, with no merge operation.

## Tests

The module registers `TestModelDiff` in `Init.py` and `TestModelDiffGui` in
`InitGui.py` for the standard FreeCAD CI discovery. Run:

```sh
QT_QPA_PLATFORM=offscreen FreeCADCmd -t TestModelDiff
xvfb-run -a FreeCAD -t TestModelDiffGui
```
