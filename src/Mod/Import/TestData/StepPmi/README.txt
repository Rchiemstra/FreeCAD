semantic.step is generated test data, authored for FreeCAD under LGPL-2.1-or-later.
Generated with OCCT 7.6.3 DRAW, using the adjacent generate.tcl (no supplier data).
It reduces /tmp/job202/generate_xcaf.tcl from the row 21 AP242 audit to one box,
one linear distance dimension, one perpendicularity tolerance, and datum A.
No materials, views, clipping planes or graphic annotations are included.

Reproduce in the dependency image, from this directory:
  docker run --rm --memory=4g --memory-swap=4g \
    -v "$PWD:/fixture" -w /fixture --entrypoint /bin/bash freecad-ci-deps:box1 \
    -c 'occt-draw -b -f generate.tcl'
In this image the DRAW plugins have only versioned shared libraries. If the
command reports missing libTKTopTest.so, create temporary unversioned symlinks
inside the container and use them for DRAW (no installed-package changes):
  mkdir -p /tmp/pmi-occtlib
  for lib in /usr/lib/x86_64-linux-gnu/libTK*.so.7.6.3; do
    ln -s "$lib" "/tmp/pmi-occtlib/$(basename "$lib" .7.6.3)"
  done
  LD_LIBRARY_PATH=/tmp/pmi-occtlib occt-draw -b -f generate.tcl
Header timestamps may differ; the semantic content is deterministic.

Expected: millimetres, 10 nominal, lower/upper deviations -0.1/+0.2;
perpendicularity zone 0.05 mm, datum A in position 1; two opposed box faces.
Both FreeCAD converters must resolve exact faces. Merge/multi-document imports
must retain readable records and explicitly report unsupported association.

XCAF normalisation and lower-deviation magnitude convention are documented by
OCCT 7.6.3 STEPCAFControl_Reader::setDimObjectToXCAF / convertAngleValue:
https://github.com/Open-Cascade-SAS/OCCT/blob/V7_6_3/src/STEPCAFControl/STEPCAFControl_Reader.cxx

Independent fixture (Part B)
----------------------------
independent-inch.step was hand-authored as ISO 10303-21 text for the AP242 MIM
entity structure, under LGPL-2.1-or-later, by FreeCAD contributors on 2026-10-21.
No OCCT/FreeCAD STEP writer, supplier file or generated semantic.step geometry
was used to produce it. Its eight vertices, six poly-loop bounded planar faces,
product graph, unit graph and PMI graph are explicitly authored in this file.
This is an independent reader test, not an AP242 conformance certification.

The Part 21 graph uses FACETED_BREP / FACE_SURFACE / POLY_LOOP for the box;
GEOMETRIC_ITEM_SPECIFIC_USAGE connects shape aspects to representation items;
DIMENSIONAL_CHARACTERISTIC_REPRESENTATION connects dimensions to measures;
DATUM_REFERENCE_COMPARTMENT / DATUM_SYSTEM order the datum references.
The OCCT reader implementation linked above and the installed Step*/RWStep*
headers describe the AP242 entity layouts accepted by the dependency image.

Expected values and associations, asserted for both converters:
* inch-based box: 1 x 2 x 3 in -> 25.4 x 50.8 x 76.2 mm, volume 98322.384 mm^3;
* distance between x-min/x-max faces: 1 in -> 25.4 mm;
  lower/upper deviations -0.01/+0.02 in -> -0.254/+0.508 mm;
* ANGULAR_LOCATION named "角度": pi/2 rad -> 90 deg, x-min/bottom faces;
* angularity named "傾斜度": 0.005 in -> 0.127 mm (a length zone, not deg),
  x-max face, free-state modifier;
* datum "α" on x-min and datum B on bottom, in compartments 1 then 2.
The Unicode semantic names and datum identifier/label survive FCStd save/reopen
with the STEP source deleted. STEP strings use Part 21 X2 hexadecimal encoding.

unmapped-modifier.step is a copy of that hand-authored fixture, with the sole
semantic change .FREE_STATE. -> .UNMAPPED_TEST_MODIFIER. at entity #132.
This deliberately invalid enumeration is a reader robustness fixture, not a
standards-conforming modifier. OCCT substitutes a known enum but records the
original tokens in Interface_ReportEntity / StepData_UndefinedEntity. FreeCAD
keeps those original codes in file-level Warnings (with STEP entity number) and
RawMetadata; they persist after save/reopen. These warnings apply to the file,
without claiming to identify an exact XCAF record for a malformed STEP entity.
OCCT 7.6 can emit advisory complex-member ordering messages even for its own
writer's complex instances; the independent valid fixture has no read failures.

GUI: Edit > Preferences > Import-Export > STEP, Import section, checkbox
"Import semantic PMI (dimensions, tolerances, datums)" (default off).
File > Import uses the same settings page; accepting it saves the same preference.
ImportGui.open/insert(..., semanticPMI=True/False) explicitly override it.
The STEP PMI group exposes readable properties and selectable shape references;
no presentation rendering, STEP export or editing solver is provided.
SourceId is an import-local XCAF label entry, not a stable STEP entity number.
