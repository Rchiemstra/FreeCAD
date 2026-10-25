SPDX-License-Identifier: LGPL-2.1-or-later
Author: FreeCAD contributors (Codex), 2026-10-25.
Provenance: cameras, clipping Boolean trees, shape map and annotation links
were independently hand-authored in Part 21, without an OCCT/FreeCAD writer.
The faceted block and semantic PMI reuse the hand-authored independent-inch
fixture (same license). Nested assemblies extend placed-instances by hand.

Expected source entities: #315 parallel, #316 central; VRS #301 origin
(10,20,30), X=(0,1,0), Y=(0,0,1), backward Z=(1,0,0).
Projection #302=(2,3,40) local -> (50,22,33) world.
Window #305: 18 x 12, origin (-7,-5,0), so it is asymmetric.
View plane distance 4, front 2, back 80; #315 front/back/sides true,
#316 front false/back true/sides false. These are lengths, never zoom ratios.
Planes #310/#312: n=(+/-1,0,0), signed d=-/+5 in n.x+d=0.
Expression #315 intersection, #316 union; retained positive half-space.
All lengths multiply by 1 (mm), 25.4 (inch), 1000 (metre).
Shape #322 maps exactly to block representation #10 (definition owner).
PMI #331 associates dimension #104 with callout #329 and both views.
Nested file: #10 is a definition used three times per subassembly, twice
under root #245. No full occurrence path is supplied by its view links;
all definition links must remain unavailable rather than choose an occurrence.
No production importer or exporter generated these expectations.

API evidence: OCCT V7_6_3 STEPCAFControl_Reader.cxx ReadViews/buildClippingPlanes
and the installed 7.6.3 XCAFView_Object/XCAFDoc_ViewTool headers. ReadViews
loses VRS origin/window placement and leaves scalar lengths in source units;
our raw STEP camera adapter must retain them and convert using view context.
No view activation, rendering, union evaluation or capping is implemented.
