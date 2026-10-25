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
slice; source types and reasons are preserved.

Native schema 1 records use generated object names; exact SupplierName is a
separate property. Value is editable (String/Distance/Import::PropertyUnwrappedAngle). RawMetadata retains
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

Slice 3 rework acceptance (job342): native PropertyLength clamps signed lengths
and PropertyAngle clamps unwrapped angles. PropertyDistance preserves signed mm;
Import::PropertyUnwrappedAngle is a native PropertyQuantity with a fixed angle
unit in its constructor, an ordinary unit editor, and native copy/restore.
A generic dynamic PropertyQuantity was rejected after runtime verification:
its assigned unit does not survive native save/reopen. No restore proxy, raw
metadata replay, or expression substitutes for editable live values.
The lifecycle test imports -2 mm / 450 deg and edits -2000 mm / 810 deg,
undoes/redoes, deletes the source, saves/reopens, and copies native owner links;
subsequent -3 m / -1080 deg edits retain units and immutable provenance.
GUI acceptance types signed lengths and large angles into native unit editors.

Independent unit_chain variants add #680 -> #681 -> #683 -> #684 -> base.
Length calculation: 2 source * 2 outer factor * 3 inner factor = 12 base units:
12 mm, 304.8 mm for inches (12 * 25.4), 12000 mm for metres (12 * 1000).
Angle calculation: 12 radians * 180/pi = 687.5493541569879 degrees;
a separate degree terminal (pi/180 rad) gives 12 degrees. Expectations are
hand calculated and never derived from importer output. Unit display names
'outer supplier', 'inner supplier', 'degrees' are provenance, never evidence
of dimensions or conversion. Every conversion hop validates its exact factor
ID, source numeric token (retained with its comments in SourceUnit), typed
value/type, reader failures and all seven
explicit dimensional exponents. SI conversion is used only at the terminal;
visited identities and a maximum depth of 16 bound traversal. Missing, cyclic,
excessive, conflicting and malformed intermediate chains remain unavailable
with reasons; independent strings and geometry survive supported reader input.

The scanner validates comment-masked numeric text and retains the original
SourceToken, including comments. Quoted text, doubled quotes, embedded comment
markers and semicolons cannot become zero or change record boundaries.
Duplicate IDs, including leading-zero equivalents and owners, are unavailable;
an ambiguous owner ID cannot bind a transferred object. A multiline record
over 64 KiB is unavailable while valid siblings survive. A separate 59 KiB
comment inside a complete numeric token is accepted by iterative lexical
scans, preserving the literal without recursive regular-expression parsing. OCCT 7.6 rejects the
unfinished-record fixture at file-read time; acceptance asserts that error and
checks the preexisting document records remain intact.

Already-passing acceptance is kept distinct from corrected cases: explicit
mm/inch/metre and radians/degrees, quoted numeric rejection, source-free native
owner remapping/partial copies, Unicode duplicate order, root/definition/exact
occurrence paths, omitted/repeated/nested/ambiguous owners, actual ReduceObjects
imports (separate from merging), option default-off and GUI dialog overrides,
repeated inserts/two documents, geometry/colors/placements/hierarchy and PMI/
view coexistence. Scanner duplicate-ID and comment-numeric acceptance and
signed/unwrapped lifecycle assertions are named independently.

The merged color-result regression verifies returned objects are live geometry,
with the same volume and placement when attributes are enabled or disabled.
Intermediate features deleted by merging cannot remain in the color-result map
when owned metadata objects are created.

Slice 4 export (job346, FreeCAD contributors / Codex, 2026-10-25):
Import[Gui].export(..., userAttributes=True) and ExportUserAttributes (Mod/Import,
default false) emit descriptive Unicode strings, finite signed lengths and
unwrapped angles. The STEP export dialog saves the same option. Explicit
userAttributes overrides preferences and the GUI options dictionary. Disabled
export preserves the previous None/semanticPMI report return convention. Enabled
export appends one {object, kind: userAttribute, status, reason} row per native
attribute to any existing PMI report. It does not classify PMI or material as
user attributes. Current native Value and Owner govern export, including edits,
undo/redo, recompute, source-free restore, deep copy and omitted-link copies.
RawMetadata is not read. Supplier property names, descriptions, representation
names, contexts, duplicate names and item order survive supported output.
Provenance separates property sets with identical names; it never binds owners.

Separate converter observations retain exact definition/component labels. A
writer transfer binder supplies the corresponding product definition or exact
NAUO under its observed free parent. Missing/deleted/out-of-selection owners,
shared definitions, repeated/nested scope without an exact path and unsupported
encodings are skipped with reasons. Object names, labels, imported STEP IDs and
geometric equality alone never establish ownership. Integer/Boolean/unitless
real, aggregates, references, saved-view export and authored PMI remain outside
this slice. Legacy converters may omit assembly/component observations; these
records remain explicitly unavailable.

Output measures use explicit typed LENGTH_MEASURE or PLANE_ANGLE_MEASURE selects
and units: millimetre SI, metre SI, inch conversion (25.4 mm), or radian SI.
Representation context text is preserved independently of explicit measure units.
An emission exception discards the partial writer model and retries a fresh
writer with valid geometry and PMI. Tests inject a later numeric-record failure
after descriptive items entered the model, and compare timestamp-normalized
bytes with the disabled export. The tests parse emitted property graphs, owner
targets and unit encodings independently before import roundtrips. Existing
independent supplier fixtures above supply the expectations; no new fixture
is derived from the writer. Geometry, hierarchy, placement, colors, PMI and views
have separate parity/isolation checks, with preferences restored by each test.

The export test's independent literal variant adds #680 PROPERTY_DEFINITION,
#681 PROPERTY_DEFINITION_REPRESENTATION and #682 REPRESENTATION, using #7 as
owner, #80 as context and #603 as its single descriptive item. Author/date/
license: FreeCAD contributors (Codex), 2026-10-25, LGPL-2.1-or-later. Expected:
two distinct identically named property sets, six emitted records, including
two Label strings 'supplier label', both owned by the exact observed definition.
