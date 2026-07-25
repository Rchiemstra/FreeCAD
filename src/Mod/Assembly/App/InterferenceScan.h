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

struct InterferenceExclusionRule
{
    App::DocumentObject* first = nullptr;
    App::DocumentObject* second = nullptr;
    /** Stable identity even when the endpoint DocumentObject* is unresolved. */
    std::string firstIdentity;
    std::string secondIdentity;
    bool valid = true;
    std::string diagnostic;
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
};

struct InterferencePairResult
{
    std::size_t leafIndexA = 0;
    std::size_t leafIndexB = 0;
    Part::InterferenceResult detection;
    bool excluded = false;
};

struct InterferenceComponentIssue
{
    std::size_t leafIndex = 0;
    std::string diagnostic;
};

struct InterferenceScanCounts
{
    int penetrations = 0;
    int contacts = 0;
    int clearanceViolations = 0;
    int excludedViolations = 0;
    int invalidInputs = 0;
    int inconclusivePairs = 0;
    int clearPairs = 0;
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

struct InterferenceScanOptions
{
    double clearance = 0.0;
    bool includeHidden = false;
    Part::InterferenceOptions detectionOptions;
    const std::atomic<bool>* cancelFlag = nullptr;
    std::function<void(int current, int total)> progress;
};

/**
 * True for App::Part containers that can be interference roots
 * (includes AssemblyObject / AssemblyLink; excludes PartDesign::Body).
 */
AssemblyExport bool isInterferenceRoot(const App::DocumentObject* obj);

/** Clearance / exclusion metadata for AssemblyObject (static props) or App::Part (dynamic). */
AssemblyExport double getInterferenceClearance(const App::DocumentObject* host);
AssemblyExport void setInterferenceClearance(App::DocumentObject* host, double clearanceMm);
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

/** Leaves whose occurrenceSubName starts with the given prefix. */
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
    /** Count of non-empty subName handles (literal subelement picks). */
    int subelementHandleCount = 0;
    /** Distinct resolved occurrence prefixes among those handles. */
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
 * pair: exactly two non-empty subelement handles on the same interference root
 * that resolve to two distinct occurrence prefixes. Otherwise a valid edit-mode
 * assembly remains the host. With no edit-mode assembly, selected
 * App::Part/Assembly roots (including whole-object picks) can still host scans.
 */
AssemblyExport App::DocumentObject* resolveInterferenceHostFromHandles(
    const std::vector<InterferenceSelectionHandle>& handles,
    App::DocumentObject* editModeAssemblyOrNull = nullptr
);

/**
 * Exactly two non-empty subelement handles that resolve to two distinct occurrences
 * → SelectedPair. Any other cardinality or resolution → AllComponents.
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
