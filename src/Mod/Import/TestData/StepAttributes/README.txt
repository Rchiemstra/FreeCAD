SPDX-License-Identifier: LGPL-2.1-or-later
Author: FreeCAD contributors (Codex), 2026-10-25.

Provenance: #600..#673 are independently hand-authored ISO 10303-21 AP242
property chains. No FreeCAD/OCCT writer or importer output supplied the chains
or expected values. Geometry, semantic PMI and cameras reuse the independently
hand-authored StepViews fixtures (same license; see their README.txt). These
reader fixtures are not an AP242 conformance certification.

Each file contains PROPERTY_DEFINITION #600 ->
PROPERTY_DEFINITION_REPRESENTATION #601 -> REPRESENTATION #602 with eight items:
#603 supplier name Label, string 'supplier label'; #604/#605 exact duplicate
Unicode names 名称, strings 'é温度' and 'second', in positions 2/3;
#606 span = 2 explicit source lengths; #607 angle = pi/2 explicit radians;
#608 unsupported compound array of item references (#603/#604); #609 unsupported area measure;
#610 length 3 in unsupported supplier unit #611. All have owner definition #7,
property-set 'supplier set', description 'revision context', representation
'inspection set'. mm/inch/metre sources give span 2/50.8/2000 mm; angle 90 deg.
The measure's explicit unit is authoritative, independently of representation
context units. Unsupported data is retained with reasons and no live Value.

Excluded chains: #620 material property / material name -> steel;
#630 geometric validation property -> volume; #640 shape-aspect PMI annotation.
#652 is an omitted geometry owner, intentionally named like definition #7;
#653..#656 orphan string 'no shape' must never bind by that name.

Nested source adds root product definition #242, root representation #245;
#660..#663 root string 'root value' belongs to #242. #670..#673 override string
'instance value' belongs to occurrence #210 inside the repeated subassembly.
The same child occurs in three positions under each of two root occurrences.
Its incomplete absolute path must be unavailable; never select one instance.
Definition #7 cannot claim a placed occurrence as its native definition object.
Legacy assembly callbacks omit the exact assembly owner, so it stays unavailable.
Merged/reduced/omitted/external or non-injective transfer owners stay unavailable.

TestStepAttributes.variant makes literal hand-authored variants: nonfinite
1.E999, missing $, malformed string 'bad' in #606's measure; conflicting mm
and metre representation context units; radian unit on a length measure.
Malformed/nonfinite values remain unavailable and independent valid siblings
survive. A conflicting representation context cannot override an explicit unit;
a conflicting explicit measure dimension is refused.

Installed OCCT 7.6.3 declarations, captured in the job334 API evidence, verify
PropertyDefinitionRepresentation::Definition/UsedRepresentation,
PropertyDefinition::Definition, CharacterizedDefinition's PD/PDS/relationship
selects, Representation::ItemsValue, DescriptiveRepresentationItem::Description,
MeasureRepresentationItem::Measure, ReprItemAndMeasureWithUnit::GetMeasureWithUnit,
MeasureWithUnit::ValueComponentMember/UnitComponent, STEPConstruct_UnitContext,
and Reader::GetShapeLabelMap with exact transfer binders. No NamedData bridge,
ShapeTool search, geometric equality, or display-name matching establishes owners.
Integer/Boolean/unitless-real and complex aggregates/references are outside this
slice; source types and reasons are preserved. Export is a later slice.

Native schema 1 records use generated object names; exact SupplierName is a
separate property. Value is editable (String/Length/Angle). RawMetadata retains
source context and is never replayed over live values or native links. Native
Owner links govern copy/remapping/omission, save/reopen, undo/redo and import
isolation. No source model/file or GUI activation survives capture.

Additional independent assembly source: attributes-assembly.step removes the
outer repeated assembly from attributes-nested.step. Its root is #202; #670
then owns #220, with complete path #202/#220 (the second placed block, X=4 in).
A literal variant places both #220 and #230 at X=8 in: the transfer label is
shared, so even a complete source path must remain unavailable as ambiguous.
The top-level occurrence and root can resolve in the link converter. Native
geometry labels follow the converter's settings and never establish identity.
A DEGREE conversion-based plane-angle unit has factor pi/180 rad; a 90-degree
source value remains 90 deg. A missing representation on #601 retains an
unavailable chain record while the independent orphan chain survives.

OCCT 7.6.3 accepts LENGTH_MEASURE($) and LENGTH_MEASURE('bad') as real zero,
without a read failure. A bounded streaming scalar-token check therefore reads
at most 64 KiB per entity, hides strings/comments, and validates only numeric
MEASURE_WITH_UNIT tokens against the exact typed source entity ID/type/value.
SourceToken keeps the literal; malformed/nonfinite/unverifiable scalars remain
unavailable. This check never establishes ownership or unit conversion.
Unknown item selects dropped from a representation are recovered only through
that representation's exact diagnostic item position/entity reference. The
original unsupported source type/name remains owned data. OCCT material and
validation defaults remain untouched; the opt-in model-label guard excludes
camera/annotation auxiliary labels from the legacy geometry traversal.
