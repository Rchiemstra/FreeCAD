# SPDX-License-Identifier: LGPL-2.1-or-later
# Run: occt-draw -b -f generate.tcl (writes semantic.step in the current directory).
pload ALL
XNewDoc D
box b 10 20 30
set root [string trim [XAddShape D b 0]]
SetName D $root PmiBlock
explode b f
set f1 [string trim [XAddSubShape D b_1 $root]]
set f2 [string trim [XAddSubShape D b_2 $root]]
set dim [string trim [XAddDimension D $f1 $f2]]
XSetDimensionType D $dim 2
XSetDimensionValue D $dim 10
XSetDimensionPlusMinusTol D $dim 0.1 0.2
XSetGDTSemanticName D $dim Distance
set tol [string trim [XAddGeomTolerance D $f1]]
XSetTypeOfTolerance D $tol 9
XSetToleranceValue D $tol 0.05
XSetGDTSemanticName D $tol Perpendicularity
set datum [string trim [XAddDatum D $f2]]
XSetDatumName D $datum A
XSetDatumPosition D $datum 1
XSetDatum D $datum $tol
param write.step.schema AP242DIS
WriteStep D semantic.step
exit
