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
Native capture does not activate views. Explicit GUI activation is described below;
union evaluation and capping remain unavailable.

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
  Raw dimensions remain unchanged. Positive 18x12 at (-9,-6,4) is available on view plane Z=4.
  A +90-degree window has right (0,1,0), up (-1,0,0): its centered origin is
  (6,-9,4). It retains rotation and is unavailable for rotation alone; origin
  (-9,-6,4) with that rotation is also asymmetric. Centering uses window axes.
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

Explicit activation (slice 2)
StepViewsGui.activate(record, targetView) applies owned live values only to a
live existing 3D view in the record's document. clear(targetView) removes this
view's owned clipping and restores its camera preceding the first activation.
Replace retains that baseline; repeated clear is harmless. Both return True;
unavailable records/targets raise ValueError before mutation. Presentation
failures propagate after camera and scene rollback. A tree context action,
"Activate STEP saved view", uses the selected record and current existing view;
"Clear STEP saved view" restores that view. Neither action switches documents,
creates views, fits geometry, edits preferences or writes document properties.
Import remains default off. Import/recompute/native restore/copy/undo/redo never
activate a saved view. Deleting an activated record or linked plane restores its view; view or
document closure releases the owned presentation nodes. Pre-existing clipping
nodes and tools survive activation, replacement, failure and clear.

Supported: finite mm camera data with a verified positive source unit factor;
orthonormal VRS at arbitrary origins/orientations; parallel or central projection
on the centered window's normal axis; positive dimensions, VRS view plane Z=d;
centered, unrotated lower-left window placement (-width/2,-height/2,d).
Camera position is the owned world projection point. Let s=sign(d-projection.z):
forward=s*VRS.Z, up=VRS.Y, camera right=-s*VRS.X. Focal distance is
abs(d-projection.z); orthographic height is window height; perspective vertical
angle is 2*atan(height/(2*focal distance)); aspect is width/height. Coin's cropped
viewport without a frame preserves that window's aspect/dimensions with letterbox
space. Front/back distances are VRS plane coordinates, not eye distances.
Enabled planes retain respectively the forward/backward half-space. They use
owned scene clip planes because the renderer adjusts camera near/far automatically.
The visible viewport supplies the side boundary; no geometry or PMI visibility
is modified. Native shape/PMI associations are never used to select geometry.

Clipping supports only complete intersection expressions of linked planes.
Live STEP IDs must match the expression exactly; links must be live, in the same
document/unit context and, when present, native saved-view owner group. STEP
n.x+offset>=0 maps to Coin normal n and distance -offset. Plane origin/normal/
offset must agree. Links omitted by native partial copy remain omitted; source
names, RawMetadata and source files are never used to reconstruct them.
Refused unchanged: unavailable/partial/unknown-unit records; nonfinite or invalid
scalars/dimensions/bases; inconsistent position; off-axis/oblique projection;
rotated/asymmetric/off-plane windows; eye on view plane; invalid or behind-eye
clipping planes; unordered enabled front/back; union, capping, incomplete/invalid
expression, missing/duplicate/foreign plane ownership. The portable limit is six
active scene clipping planes including pre-existing tools. Aspect outside
(1e-7,1e7), perspective angle outside (1e-7,pi-1e-7), focal distance <=1e-7 mm,
or scalars outside the finite +/-1e30 range are unavailable. The schema-1 reader
also conservatively refuses negative clipping coordinates and enabled back<=front;
activation does not reinterpret unavailable source records.

Independent activation fixtures views-centered-{mm,inch,metre}.step were authored
by literal Part 21 substitutions in the original supplier-independent fixtures:
#302 projection=(0,0,0); #303 window=(-9,-6,4); #316 clips by direct intersection (#310,#312).
No FreeCAD/OCCT writer or activation output supplied these expectations. VRS is
still O=(10,20,30), X=(0,1,0), Y=(0,0,1), Z=(1,0,0). Expected eye is O,
forward=(1,0,0), up=(0,0,1), right=(0,-1,0), focal=4, height=12, aspect=1.5,
perspective angle=2*atan(1.5). All lengths scale by 1, 25.4, 1000 into mm.
Plane #310 retains x>=5*scale, #312 retains x<=5*scale. Enabled front retains
x>=12*scale and back x<=90*scale. Perspective front is disabled. A further
hand-authored test variant moves #302 to (0,0,100) and disables depth clipping:
eye=(110,20,30), forward=(-1,0,0), right=(0,1,0), focal=96, central angle=
2*atan(6/96). Rotated/asymmetric and unsupported operand variants remain refused.

Mapping reference: ISO 10303-46 view_volume attribute definitions and propositions:
https://www.steptools.com/stds/smrl/data/resource_docs/visual_presentation/sys/4_schema.htm
View plane distance and front/back distances are measured along VRS Z; a 3D
window's placement Z equals the view plane distance (WR2). The reader now checks
XY centering separately from this Z constraint. Older schema-1 files with window
Z=0 and a distinct d remain native data but cannot be activated until valid owned
values are provided. GUI tests assert independent numeric camera basis/position,
framing and planes, two-document/two-view isolation, full mutation rollback,
source-free persistence/copy/undo, omitted links, lifetime cleanup and no doc edits.
