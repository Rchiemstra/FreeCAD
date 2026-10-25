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

Rework variants (TestStepViews.variant) are independently authored Part 21
substitutions/additions to views-mm.step, written to temporary source files;
no importer/exporter output supplies their expected values:
- Conflicting context #340 declares mm #81, metre #341, radian #84, steradian
  #85, in that order. Even though the final angle units are valid, no unique
  length conversion exists: source origin stays (10,20,30), factor is 0, all
  four declarations survive, and view/planes are unavailable.
- Camera #315 is shared by models #330 and #343. #343 maps representation
  #340 (independent solid #351/shell #352) of product #348 and associates
  angular dimension #114 via
  callout #347; #330 maps #10 and dimension #104 via #329. Neither shape nor
  PMI may bind for #315; both roles retain ambiguity reasons. #316 belongs
  only to #330 and must retain that model's links.
- Centered windows (0,12), (-18,12), (18,-12) are unavailable. View plane
  distance 0, front -2, and enabled front/back 80/2 are also unavailable.
  Raw dimensions remain unchanged. Positive 18x12 at (-9,-6,0) is available.
  A +90-degree window has right (0,1,0), up (-1,0,0): its centered origin is
  (6,-9,0). It retains rotation and is unavailable for rotation alone; origin
  (-9,-6,0) with that rotation is also asymmetric. Centering uses window axes.
- Alternating union/intersection nodes #400..#469 exceed adapter depth 64.
  #315 retains its identity, full typed adjacency, reason and valid sibling
  planes #310/#312; #316 retains its original camera and clipping expression.
  A missing #315 volume or unsupported clipping operand #323 similarly keeps
  both cameras and the two valid planes, with individual unavailable reasons.
  The optional OCCT view pass is skipped when its required data is incomplete;
  raw capture still runs, and incomplete XCAF tables never supply owner links.
- Duplicate camera names retain distinct STEP/XCAF identities (native display
  labels may be uniquified). A last orphan camera #340 makes the XCAF table
  incomplete: all three raw cameras survive, no shape/PMI links are guessed.

TestStepPmiGui adds a hand-authored solid surface style #500..#508 to the
nested source: RGB (0.8,0.2,0.1), on solid #11. Both converters must retain
colors, placements, real geometry and hierarchy with combined metadata enabled.
Partial-copy tests omit owners, then add matching names and save/reopen: no
provenance is replayed. Native copy rules remain authoritative. Observer-created
objects survive metadata rollback; multi-document modes refuse both link roles.
Schema remains 1: extra source fields are additive owned data. Unsupported
operands discarded by OCCT's typed reader retain an explicit missing operand;
source camera identity is retained, without inferring an owner from names.
