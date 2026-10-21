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

Part A only: headless import and persistent semantic snapshots. GUI import hooks,
inches/angle/Unicode/unknown-modifier fixtures and an independent producer fixture
are deferred to Part B. No presentation rendering, STEP export or editing solver.
SourceId is an import-local XCAF label entry, not a stable STEP entity number.
