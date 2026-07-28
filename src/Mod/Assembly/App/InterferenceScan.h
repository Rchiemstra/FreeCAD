// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Assembly/AssemblyGlobal.h>

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <Base/BoundBox.h>
#include <Base/Vector3D.h>
#include <Mod/Part/App/InterferenceDetection.h>
#include <TopoDS_Shape.hxx>

namespace App
{
class DocumentObject;
class Part;
class PropertyLength;
class PropertyXLinkSubList;
}

namespace Assembly
{

class AssemblyObject;
class ReviewNote;

struct InterferenceExclusionRule
{
    App::DocumentObject* first = nullptr;
    App::DocumentObject* second = nullptr;
    /** Stable identity even when the endpoint DocumentObject* is unresolved. */
    std::string firstIdentity;
    std::string secondIdentity;
    /** Optional ReviewNote explaining why this source pair is excluded. */
    ReviewNote* reason = nullptr;
    /** Stable string identity retained when the reason note is missing/deleted. */
    std::string reasonIdentity;
    bool valid = true;
    std::string diagnostic;
};

/** Cached face geometry for a leaf (worker-built; immutable for the scan). */
struct InterferenceCachedFace
{
    int index = 0;
    TopoDS_Shape face;
    Base::BoundBox3d box;
};

/** Immutable snapshot of one physical leaf occurrence (safe for worker threads). */
struct InterferenceLeaf
{
    /** Sub-object path relative to the scanned root (e.g. NestedPart.InnerBox.). */
    std::string occurrenceSubName;
    std::string displayPath;
    /** Stable source-definition identity (document#name of linked definition). */
    std::string sourceId;
    TopoDS_Shape worldShape;
    Base::BoundBox3d worldBoundBox;
    bool visible = true;
    bool shapeValid = true;
    std::string diagnostic;
    /** Filled once on the worker; reused for face×face enumeration. */
    mutable std::vector<InterferenceCachedFace> cachedFaces;
    mutable bool facesCached = false;
};

enum class InterferenceClearanceRuleKind
{
    ExactPair,
    MaxIndividual,
    DefaultStar,
    AssemblyGlobal,
    Unresolved
};

/** One immutable spreadsheet design-clearance rule (worker-safe). */
struct InterferenceClearanceRule
{
    bool enabled = true;
    /** Normalized face path, or "*" for default. */
    std::string faceA;
    /** Empty → single-face rule; set → exact unordered face-pair override. */
    std::string faceB;
    double clearanceMm = 0.0;
    int spreadsheetRow = 0;
    std::string comment;
    bool valid = true;
    std::string diagnostic;
};

struct InterferenceClearanceLookup
{
    double clearanceMm = 0.0;
    InterferenceClearanceRuleKind kind = InterferenceClearanceRuleKind::AssemblyGlobal;
    /** All contributing spreadsheet rows (1-based); may be two for MaxIndividual. */
    std::vector<int> sourceRows;
    std::vector<std::string> sourceComments;
    std::string diagnostic;
};

/** Deterministic counters for clearance spreadsheet parsing (unit tests). */
struct InterferenceClearanceSheetParseStats
{
    /** Times listInterferenceStructuralLeafSites ran (no shape extraction). */
    int structuralOccurrenceTraversalPasses = 0;
    /** Times collectInterferenceLeaves ran during spreadsheet parsing (should stay 0). */
    int hostLeafCollectionPasses = 0;
    /** Per-occurrence world-shape resolutions while validating referenced faces. */
    int leafShapeExtractions = 0;
    /** TopExp::MapShapes face-index passes during parsing (one per extracted occurrence). */
    int faceEnumerationPasses = 0;
    /** First-time host face validation for a referenced occurrence path. */
    int lazyOccurrenceValidations = 0;
    /** Reused prepared-leaf or cached occurrence validation results. */
    int occurrenceValidationCacheHits = 0;
};

/** Immutable snapshot of spreadsheet design-clearance rules. */
struct InterferenceClearanceRuleTable
{
    std::vector<InterferenceClearanceRule> rules;
    /**
     * Max enabled spreadsheet Tolerance (face / pair / *). Host clearance is
     * combined at scan time via conservativeMaxDesignClearance().
     */
    double maxEnabledClearance = 0.0;
    bool hasDefaultStar = false;
    double defaultStarClearance = 0.0;
    std::vector<std::string> diagnostics;
    /** Number of enabled invalid rules (+ header failures). Counted once per rule. */
    int invalidRuleCount = 0;
};

/** Face-level clearance evaluation under a component pair (includes Clear). */
struct InterferenceFaceHit
{
    std::string facePathA;
    std::string facePathB;
    double distance = -1.0;
    double appliedClearance = 0.0;
    Part::InterferenceKind classification = Part::InterferenceKind::Inconclusive;
    InterferenceClearanceRuleKind ruleKind = InterferenceClearanceRuleKind::AssemblyGlobal;
    std::vector<int> sourceRows;
    std::vector<std::string> sourceComments;
    std::string diagnostic;
    /** Worker-computed closest points for this exact face pair, in world coordinates. */
    bool closestPointsValid = false;
    Base::Vector3d pointOnFirst;
    Base::Vector3d pointOnSecond;
    /** Worker-computed contact/common geometry for this exact face pair, when available. */
    TopoDS_Shape commonShape;
    /**
     * True when this hit is a Penetration/Contact/ClearanceViolation that is
     * suppressed by a source-pair exclusion. InvalidInput / Inconclusive never
     * set this; they stay visible by default even on mixed excluded pairs.
     */
    bool suppressedByExclusion = false;
};

/**
 * One component/leaf pair result. Face-specific clearance checks live in
 * faceHits; solid penetration is never multiplied by intersecting faces.
 */
struct InterferencePairResult
{
    std::size_t leafIndexA = 0;
    std::size_t leafIndexB = 0;
    /** Component-level solid classification (Penetration / Clear / …). */
    Part::InterferenceResult detection;
    /**
     * True when the pair has at least one exclusion-suppressed violation.
     * Does not imply InvalidInput/Inconclusive details are hidden.
     */
    bool excluded = false;
    /** Face evaluations including Clear (App/tests); GUI filters Clear by default. */
    std::vector<InterferenceFaceHit> faceHits;
    /**
     * Face hit that determined detection.kind/minimumDistance, or npos when the
     * whole-solid result (for example Penetration/Inconclusive) governs.
     */
    std::size_t governingFaceHitIndex = static_cast<std::size_t>(-1);
    /** Optional note when face-pair candidates were capped. */
    std::string faceEnumerationDiagnostic;
};

struct InterferenceComponentIssue
{
    enum class Kind
    {
        InvalidLeaf,
        InvalidRule,
        FaceEnumerationCapped,
        Other
    };
    Kind kind = Kind::Other;
    std::size_t leafIndex = 0;
    /** Second leaf for pair-scoped diagnostics; npos when unused. */
    std::size_t leafIndexB = static_cast<std::size_t>(-1);
    std::string diagnostic;
};

struct InterferenceScanCounts
{
    /** Penetrating component/leaf pairs. */
    int penetrations = 0;
    /** Face interactions classified Contact. */
    int contacts = 0;
    /** Face-pair clearance violations. */
    int clearanceViolations = 0;
    /** Excluded component pairs that would otherwise be reportable. */
    int excludedViolations = 0;
    /** Malformed / unresolved spreadsheet rules. */
    int invalidRules = 0;
    /** Invalid leaf/geometry inputs. */
    int invalidInputs = 0;
    /** Unclassifiable component or face evaluations. */
    int inconclusivePairs = 0;
    /** Component pairs with no reportable issue (incl. broad-phase clears). */
    int clearPairs = 0;
    /** Face evaluations classified Clear. */
    int clearFaceHits = 0;
};

struct InterferenceScanResult
{
    std::vector<InterferenceLeaf> leaves;
    std::vector<InterferencePairResult> pairs;
    std::vector<InterferenceComponentIssue> componentIssues;
    InterferenceScanCounts counts;
    bool cancelled = false;
    bool complete = false;
};

/**
 * Test hook payload: when pointed from InterferenceScanOptions::solidOverride,
 * replaces whole-solid OCCT classification.
 */
struct InterferenceSolidOverride
{
    Part::InterferenceResult result;
};

struct InterferenceScanOptions
{
    double clearance = 0.0;
    bool includeHidden = false;
    /** Optional immutable spreadsheet design-clearance rules (caller-thread snapshot). */
    InterferenceClearanceRuleTable clearanceRules;
    Part::InterferenceOptions detectionOptions;
    const std::atomic<bool>* cancelFlag = nullptr;
    std::function<void(int current, int total)> progress;
    /**
     * Soft cap on face×face candidates evaluated per leaf pair. Oversized sets
     * evaluate a deterministic nearest-AABB subset and emit a visible diagnostic.
     */
    std::size_t maxFacePairCandidates = 10000;
    /**
     * Test hook: when non-null, replaces whole-solid OCCT classification so
     * solid-Inconclusive aggregation can be exercised without flaky OCCT failures.
     */
    const InterferenceSolidOverride* solidOverride = nullptr;
};

/**
 * True for App::Part containers that can be interference roots
 * (includes AssemblyObject / AssemblyLink; excludes PartDesign::Body).
 */
AssemblyExport bool isInterferenceRoot(const App::DocumentObject* obj);

/** Clearance / exclusion metadata for AssemblyObject (static props) or App::Part (dynamic). */
AssemblyExport double getInterferenceClearance(const App::DocumentObject* host);
AssemblyExport void setInterferenceClearance(App::DocumentObject* host, double clearanceMm);
AssemblyExport App::DocumentObject* getInterferenceClearanceSheet(const App::DocumentObject* host);
AssemblyExport void setInterferenceClearanceSheet(
    App::DocumentObject* host,
    App::DocumentObject* sheetOrNull
);
/** Parse spreadsheet design-clearance rules (caller thread; DocumentObject-safe). */
AssemblyExport InterferenceClearanceRuleTable snapshotInterferenceClearanceRules(
    const App::DocumentObject* host,
    const std::vector<InterferenceLeaf>* preparedLeaves = nullptr
);
/** Build a rule table from an explicit Spreadsheet::Sheet (or null → empty).
 * Parsing is host-independent until enabled rules require occurrence/FaceN checks.
 * Host validation uses a lightweight occurrence traversal plus lazy per-occurrence
 * shape extraction (optionally reusing preparedLeaves from scan preparation).
 * Hostless parsing is structural-only (FaceN syntax / * rules).
 */
AssemblyExport InterferenceClearanceRuleTable parseInterferenceClearanceSheet(
    const App::DocumentObject* sheetOrNull,
    const App::DocumentObject* hostOrNull = nullptr,
    InterferenceClearanceSheetParseStats* parseStats = nullptr,
    const std::vector<InterferenceLeaf>* preparedLeaves = nullptr
);
AssemblyExport InterferenceClearanceLookup lookupInterferenceClearance(
    const InterferenceClearanceRuleTable& table,
    const std::string& facePathA,
    const std::string& facePathB,
    double assemblyClearanceMm
);
/**
 * Conservative max design clearance for broad-phase / face AABB probe:
 * max(enabled sheet rules, * default if present, host clearance when reachable).
 * Host is reachable when no enabled * default covers unmatched faces.
 */
AssemblyExport double conservativeMaxDesignClearance(
    const InterferenceClearanceRuleTable& table,
    double hostClearanceMm
);
/**
 * Deterministic aggregation of whole-solid classification with face-hit outcomes.
 * A solid-level Inconclusive is never downgraded to Clear, Contact, or
 * ClearanceViolation (face hits remain available for diagnostics/counts).
 */
AssemblyExport Part::InterferenceResult finalizeInterferencePairDetection(
    const Part::InterferenceResult& solid,
    const std::vector<InterferenceFaceHit>& faceHits,
    bool faceEnumerationCapped,
    const std::string& faceEnumerationDiagnostic
);
/**
 * Count unsuppressed violating component-pair results for an unordered,
 * currently resolvable source-definition pair. Multiple violating face hits
 * within one component pair count once.
 */
AssemblyExport std::size_t countInterferenceExclusionAffectedPairs(
    const InterferenceScanResult& result,
    const std::string& sourceIdA,
    const std::string& sourceIdB
);
AssemblyExport std::vector<InterferenceExclusionRule>
getInterferenceExclusionRules(const App::DocumentObject* host);
AssemblyExport bool hasInterferenceExclusion(
    const App::DocumentObject* host,
    App::DocumentObject* first,
    App::DocumentObject* second
);
AssemblyExport void addInterferenceExclusion(
    App::DocumentObject* host,
    App::DocumentObject* first,
    App::DocumentObject* second
);
AssemblyExport void addInterferenceExclusionWithReason(
    App::DocumentObject* host,
    App::DocumentObject* first,
    App::DocumentObject* second,
    ReviewNote* reason
);
AssemblyExport void removeInterferenceExclusion(
    App::DocumentObject* host,
    App::DocumentObject* first,
    App::DocumentObject* second
);
AssemblyExport void removeInterferenceExclusionAt(App::DocumentObject* host, std::size_t ruleIndex);

/**
 * Collect physical leaf occurrences for interference checking.
 * Descends parts/assemblies/links/groups; treats Body Tip and solid GeoFeatures as leaves.
 * Resolves world shapes through the root subobject path (including nested
 * App::Part / App::Link / AssemblyLink transforms).
 */
AssemblyExport std::vector<InterferenceLeaf> collectInterferenceLeaves(
    const App::DocumentObject* root,
    bool includeHidden
);

/**
 * First component occurrence directly beneath an interference root.
 * Selected faces/edges/vertices are pick handles only; the component is the first
 * non-organizer object on the path under the root (groups remain in the prefix).
 * Example: Folder.AssemblyCase.….Face68 → prefix "Folder.AssemblyCase."
 */
struct InterferenceComponentOccurrence
{
    App::DocumentObject* component = nullptr;
    /** Subname prefix relative to root, e.g. "Folder.AssemblyCase." or "Link.0.". */
    std::string occurrencePrefix;
    std::string displayPath;
};

AssemblyExport bool resolveInterferenceComponentOccurrence(
    const App::DocumentObject* root,
    App::DocumentObject* selObj,
    const std::string& subName,
    InterferenceComponentOccurrence& out
);

/** Leaves under an occurrence prefix relative to root.
 * Traverses only that occurrence branch when the prefix resolves to a concrete
 * object path (preferred). Digit/array tokens fall back to one full collect +
 * filter. Selected-pair callers must pass includeHidden=true so explicitly
 * selected hidden occurrences still contribute leaves.
 */
AssemblyExport std::vector<InterferenceLeaf> collectInterferenceLeavesUnderPrefix(
    const App::DocumentObject* root,
    const std::string& occurrencePrefix,
    bool includeHidden
);

/**
 * Top-level component occurrences under the root (through ordinary organizer groups).
 * Link arrays contribute one occurrence per element. Hidden occurrences (including
 * those under a hidden ancestor group) are omitted unless includeHidden is true.
 */
AssemblyExport std::vector<InterferenceComponentOccurrence> listInterferenceComponentOccurrences(
    const App::DocumentObject* root,
    bool includeHidden
);

/**
 * Immutable, worker-safe snapshot: one leaf collection plus component assignment.
 * Built on the caller thread while DocumentObjects are valid.
 */
struct InterferenceComponentScanSnapshot
{
    std::vector<InterferenceComponentOccurrence> components;
    std::vector<InterferenceLeaf> leaves;
    /** Parallel to leaves; npos if a leaf matches no listed component. */
    std::vector<std::size_t> componentIndexOfLeaf;
    bool cancelled = false;
};

/**
 * List components and collect leaves once, then assign each leaf to the longest
 * matching occurrence prefix. Optional testBarrier runs after listing and before
 * leaf extraction (still on the caller thread).
 */
AssemblyExport InterferenceComponentScanSnapshot prepareInterferenceComponentScanSnapshot(
    const App::DocumentObject* root,
    bool includeHidden,
    const InterferenceScanOptions& options = {},
    const std::function<void()>& testBarrier = {}
);

/**
 * Selection handle for interference scope resolution.
 * Empty subName means a whole-object/tree selection (not a subelement pick handle).
 */
struct InterferenceSelectionHandle
{
    App::DocumentObject* object = nullptr;
    std::string subName;
};

enum class InterferenceScanScopeMode
{
    AllComponents,
    SelectedPair
};

struct InterferenceSelectionScope
{
    InterferenceScanScopeMode mode = InterferenceScanScopeMode::AllComponents;
    InterferenceComponentOccurrence first;
    InterferenceComponentOccurrence second;
    /** Count of pair endpoints: whole-object (empty subName) and subelement picks. */
    int subelementHandleCount = 0;
    /** Distinct resolved occurrence prefixes among those endpoints. */
    int distinctOccurrenceCount = 0;
};

/**
 * Normalize a possibly TNP-encoded selection subname to an old-style path
 * (e.g. Part.Body.;#…F.Face6 → Part.Body.Face6), matching UtilsAssembly.getComponentReference.
 */
AssemblyExport std::string normalizeInterferenceSubName(
    App::DocumentObject* obj,
    const std::string& subName
);

/**
 * Choose an interference host from selection handles.
 *
 * An unrelated edit-mode assembly is overridden only by an exact global selected
 * pair: exactly two endpoints (whole-object or subelement) that share one
 * interference root (same document as the edit assembly when one is present) and
 * resolve to two distinct occurrence prefixes. Whole-object tree picks of child
 * components resolve the common App::Part/Assembly ancestor as host. Otherwise a
 * valid edit-mode assembly remains the host for all-components scans. With no
 * edit-mode assembly, selected App::Part/Assembly roots can still host scans.
 *
 * Check Selected Components must use SelectedPair only; an invalid/ambiguous
 * selection leaves that command inactive (no all-components fallback).
 */
AssemblyExport App::DocumentObject* resolveInterferenceHostFromHandles(
    const std::vector<InterferenceSelectionHandle>& handles,
    App::DocumentObject* editModeAssemblyOrNull = nullptr
);

/**
 * Resolve an exact selected-pair request for Check Selected Components.
 * Valid only when exactly two endpoints share one interference root and resolve
 * to two distinct occurrences. Never falls back to an all-components host.
 */
struct InterferenceSelectedPairRequest
{
    App::DocumentObject* host = nullptr;
    InterferenceComponentOccurrence first;
    InterferenceComponentOccurrence second;
    bool valid() const
    {
        return host && !first.occurrencePrefix.empty() && !second.occurrencePrefix.empty()
            && first.occurrencePrefix != second.occurrencePrefix;
    }
};

AssemblyExport InterferenceSelectedPairRequest resolveInterferenceSelectedPairRequest(
    const std::vector<InterferenceSelectionHandle>& handles,
    App::DocumentObject* editModeAssemblyOrNull = nullptr
);

/**
 * Exactly two pair endpoints (whole-object or subelement) that resolve to two
 * distinct occurrences → SelectedPair. Any other cardinality or resolution →
 * AllComponents. A third whole-object or subelement endpoint disables SelectedPair.
 */
AssemblyExport InterferenceSelectionScope resolveInterferenceSelectionScope(
    const App::DocumentObject* root,
    const std::vector<InterferenceSelectionHandle>& handles
);

/**
 * Scan only across two leaf sets (A×B). Does not pair leaves within the same set.
 */
AssemblyExport InterferenceScanResult runInterferenceScanBetweenLeafSets(
    const std::vector<InterferenceLeaf>& leavesA,
    const std::vector<InterferenceLeaf>& leavesB,
    const InterferenceScanOptions& options,
    const std::vector<std::pair<std::string, std::string>>& excludedSourceIdPairs = {}
);

/**
 * Cross-component scan over an immutable snapshot (worker-safe).
 * Does not pair leaves that share the same componentIndexOfLeaf.
 */
AssemblyExport InterferenceScanResult runInterferenceScanAcrossComponents(
    const InterferenceComponentScanSnapshot& snapshot,
    const InterferenceScanOptions& options,
    const std::vector<std::pair<std::string, std::string>>& excludedSourceIdPairs = {}
);

/**
 * Convenience: prepare snapshot then run across components.
 * Prefer prepare + runAcross from GUI so workers never touch DocumentObject*.
 */
AssemblyExport InterferenceScanResult runInterferenceScanAllVisibleComponents(
    const App::DocumentObject* root,
    bool includeHidden,
    const InterferenceScanOptions& options,
    const std::vector<std::pair<std::string, std::string>>& excludedSourceIdPairs = {}
);

/** Deterministic sweep-and-prune candidate pairs (i < j). */
AssemblyExport std::vector<std::pair<std::size_t, std::size_t>> broadPhaseCandidatePairs(
    const std::vector<InterferenceLeaf>& leaves,
    double clearance,
    double tolerance
);

/**
 * Run full scan over an immutable leaf snapshot.
 * Exclusions are canonical unordered source-ID pairs; the worker never uses
 * App::DocumentObject*. Exclusions apply only to penetration/contact/clearance.
 */
AssemblyExport InterferenceScanResult runInterferenceScan(
    const std::vector<InterferenceLeaf>& leaves,
    const InterferenceScanOptions& options,
    const std::vector<std::pair<std::string, std::string>>& excludedSourceIdPairs = {}
);

}  // namespace Assembly
