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
 * non-group object on the path under the root (e.g. AssemblyCase.….Face68 → AssemblyCase).
 */
struct InterferenceComponentOccurrence
{
    App::DocumentObject* component = nullptr;
    /** Subname prefix relative to root, e.g. "AssemblyCase." or "Link.0.". */
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
 * Scan only across two leaf sets (A×B). Does not pair leaves within the same set.
 * Used for Check Selected Components between two complete root-level occurrences.
 */
AssemblyExport InterferenceScanResult runInterferenceScanBetweenLeafSets(
    const std::vector<InterferenceLeaf>& leavesA,
    const std::vector<InterferenceLeaf>& leavesB,
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
