// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"
#ifndef _PreComp_
# include <BRepBndLib.hxx>
# include <BRepCheck_Analyzer.hxx>
# include <Bnd_Box.hxx>
# include <TopExp_Explorer.hxx>
# include <algorithm>
# include <cmath>
# include <set>
# include <sstream>
# include <unordered_set>
#endif

#include "InterferenceScan.h"
#include "AssemblyLink.h"
#include "AssemblyObject.h"
#include "Groups.h"

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentObjectGroup.h>
#include <App/GeoFeature.h>
#include <App/Link.h>
#include <App/Datums.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Mod/Part/App/BodyBase.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/PartDesign/App/Body.h>
#include <Precision.hxx>
#include <cmath>
#include <Standard_Failure.hxx>

namespace Assembly
{
namespace
{

bool isCancelled(const InterferenceScanOptions& options)
{
    return options.cancelFlag && options.cancelFlag->load(std::memory_order_relaxed);
}

bool shapeHasSolid(const TopoDS_Shape& shape)
{
    if (shape.IsNull()) {
        return false;
    }
    TopExp_Explorer explorer(shape, TopAbs_SOLID);
    return explorer.More();
}

std::string objectKey(const App::DocumentObject* obj)
{
    if (!obj || !obj->isAttachedToDocument()) {
        return {};
    }
    return std::string(obj->getDocument()->getName()) + "#" + obj->getNameInDocument();
}

App::DocumentObject* resolveSourceDefinition(App::DocumentObject* obj)
{
    if (!obj) {
        return nullptr;
    }
    if (auto* link = freecad_cast<App::Link*>(obj)) {
        auto* linked = link->getLinkedObject(true);
        return linked ? linked : obj;
    }
    if (auto* asmLink = freecad_cast<AssemblyLink*>(obj)) {
        auto* linked = asmLink->getLinkedObject2(true);
        return linked ? linked : obj;
    }
    auto* linked = obj->getLinkedObject(true);
    return linked ? linked : obj;
}

bool isHelperOrNonPhysical(const App::DocumentObject* obj)
{
    if (!obj) {
        return true;
    }
    if (obj->isDerivedFrom<App::LocalCoordinateSystem>()
        || obj->isDerivedFrom<App::DatumElement>()) {
        return true;
    }
    if (obj->isDerivedFrom<JointGroup>() || obj->isDerivedFrom<ViewGroup>()
        || obj->isDerivedFrom<BomGroup>() || obj->isDerivedFrom<SimulationGroup>()
        || obj->isDerivedFrom<SnapshotGroup>()) {
        return true;
    }
    return false;
}

bool isContainerToDescend(const App::DocumentObject* obj)
{
    if (!obj) {
        return false;
    }
    if (freecad_cast<const AssemblyObject*>(obj) || freecad_cast<const AssemblyLink*>(obj)) {
        return true;
    }
    // App::Link arrays are handled explicitly in collectRecursively so collapsed
    // PlacementList instances are not dropped when ElementList is empty.
    if (obj->isDerivedFrom<App::DocumentObjectGroup>() && !freecad_cast<const App::Link*>(obj)) {
        return true;
    }
    if (obj->isDerivedFrom<App::Part>() && !obj->isDerivedFrom<Part::BodyBase>()) {
        return true;
    }
    return false;
}

std::vector<App::DocumentObject*> childrenOf(App::DocumentObject* obj)
{
    std::vector<App::DocumentObject*> children;
    if (!obj) {
        return children;
    }
    if (auto* asmObj = freecad_cast<AssemblyObject*>(obj)) {
        return asmObj->Group.getValues();
    }
    if (auto* asmLink = freecad_cast<AssemblyLink*>(obj)) {
        return asmLink->Group.getValues();
    }
    if (obj->isLinkGroup()) {
        auto* link = static_cast<App::Link*>(obj);
        return link->ElementList.getValues();
    }
    if (auto* group = freecad_cast<App::DocumentObjectGroup*>(obj)) {
        return group->Group.getValues();
    }
    if (auto* part = freecad_cast<App::Part*>(obj)) {
        return part->Group.getValues();
    }
    return children;
}

bool isPathVisible(const std::vector<App::DocumentObject*>& path)
{
    if (path.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < path.size(); ++i) {
        auto* obj = path[i];
        if (!obj) {
            return false;
        }
        if (!obj->Visibility.getValue()) {
            return false;
        }
        if (i + 1 < path.size() && path[i + 1] && path[i + 1]->getNameInDocument()) {
            const int elementVis = obj->isElementVisible(path[i + 1]->getNameInDocument());
            if (elementVis == 0) {
                return false;
            }
        }
    }
    return true;
}

std::string makeDisplayPath(const std::vector<App::DocumentObject*>& path)
{
    std::ostringstream ss;
    bool first = true;
    for (auto* obj : path) {
        if (!obj || !obj->getNameInDocument()) {
            continue;
        }
        if (!first) {
            ss << '.';
        }
        first = false;
        ss << obj->Label.getValue();
    }
    return ss.str();
}

/** Subname relative to the assembly root (path must not include the assembly). */
std::string makeOccurrenceSubName(const std::vector<App::DocumentObject*>& path)
{
    std::ostringstream ss;
    for (auto* obj : path) {
        if (!obj || !obj->getNameInDocument()) {
            continue;
        }
        ss << obj->getNameInDocument() << '.';
    }
    return ss.str();
}

Base::BoundBox3d shapeBoundBox(const TopoDS_Shape& shape)
{
    Base::BoundBox3d box;
    if (shape.IsNull()) {
        return box;
    }
    Bnd_Box bnd;
    BRepBndLib::Add(shape, bnd);
    if (bnd.IsVoid()) {
        return box;
    }
    double xmin {}, ymin {}, zmin {}, xmax {}, ymax {}, zmax {};
    bnd.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    box.MinX = xmin;
    box.MinY = ymin;
    box.MinZ = zmin;
    box.MaxX = xmax;
    box.MaxY = ymax;
    box.MaxZ = zmax;
    return box;
}

void enlargeBoundBox(Base::BoundBox3d& box, double margin)
{
    if (!box.IsValid() || margin <= 0.0) {
        return;
    }
    box.MinX -= margin;
    box.MinY -= margin;
    box.MinZ -= margin;
    box.MaxX += margin;
    box.MaxY += margin;
    box.MaxZ += margin;
}

bool boxesOverlap(const Base::BoundBox3d& a, const Base::BoundBox3d& b)
{
    return a.IsValid() && b.IsValid() && a.Intersect(b);
}

TopoDS_Shape resolveWorldShape(
    const AssemblyObject* assembly,
    App::DocumentObject* occurrence,
    const std::string& occurrenceSubName
)
{
    if (!assembly || !occurrence || occurrenceSubName.empty()) {
        return {};
    }

    // Resolve exclusively through the assembly/subobject path so nested
    // App::Part / Link / AssemblyLink placements are included. No manual
    // placement composition fallback.
    return Part::Feature::getShape(
        assembly,
        Part::ShapeOption::ResolveLink | Part::ShapeOption::Transform,
        occurrenceSubName.c_str()
    );
}

bool tryMakeLeaf(
    const AssemblyObject* assembly,
    App::DocumentObject* occurrence,
    const std::vector<App::DocumentObject*>& pathFromAssembly,
    const std::vector<App::DocumentObject*>& visibilityPath,
    bool includeHidden,
    std::vector<InterferenceLeaf>& leaves,
    int arrayIndex = -1
)
{
    if (!occurrence || isHelperOrNonPhysical(occurrence)) {
        return false;
    }

    if (occurrence->getTypeId().isDerivedFrom(Base::Type::fromName("PartDesign::Feature"))
        && !occurrence->isDerivedFrom<Part::BodyBase>()) {
        return false;
    }

    if (auto* body = freecad_cast<Part::BodyBase*>(occurrence)) {
        (void)body;
    }
    else if (!occurrence->isDerivedFrom<Part::Feature>()
             && !freecad_cast<App::Link*>(occurrence)
             && !occurrence->isDerivedFrom<App::GeoFeature>()) {
        return false;
    }

    const bool visible = isPathVisible(visibilityPath);
    if (!includeHidden && !visible) {
        return false;
    }

    std::string subName = makeOccurrenceSubName(pathFromAssembly);
    if (arrayIndex >= 0) {
        subName += std::to_string(arrayIndex);
        subName += '.';
    }
    std::string display = makeDisplayPath(pathFromAssembly);
    if (arrayIndex >= 0) {
        display += '.';
        display += std::to_string(arrayIndex);
    }

    TopoDS_Shape shape;
    try {
        shape = resolveWorldShape(assembly, occurrence, subName);
    }
    catch (const Base::Exception& exc) {
        InterferenceLeaf leaf;
        leaf.occurrenceSubName = subName;
        leaf.displayPath = display;
        leaf.sourceId = objectKey(resolveSourceDefinition(occurrence));
        leaf.visible = visible;
        leaf.shapeValid = false;
        leaf.diagnostic = exc.what();
        leaves.push_back(std::move(leaf));
        return true;
    }
    catch (const Standard_Failure& exc) {
        InterferenceLeaf leaf;
        leaf.occurrenceSubName = subName;
        leaf.displayPath = display;
        leaf.sourceId = objectKey(resolveSourceDefinition(occurrence));
        leaf.visible = visible;
        leaf.shapeValid = false;
        leaf.diagnostic =
            exc.GetMessageString() ? exc.GetMessageString() : "OCCT failure extracting shape";
        leaves.push_back(std::move(leaf));
        return true;
    }
    catch (...) {
        InterferenceLeaf leaf;
        leaf.occurrenceSubName = subName;
        leaf.displayPath = display;
        leaf.sourceId = objectKey(resolveSourceDefinition(occurrence));
        leaf.visible = visible;
        leaf.shapeValid = false;
        leaf.diagnostic = "Unknown failure extracting shape";
        leaves.push_back(std::move(leaf));
        return true;
    }

    if (shape.IsNull() || !shapeHasSolid(shape)) {
        return false;
    }

    InterferenceLeaf leaf;
    leaf.occurrenceSubName = subName;
    leaf.displayPath = display;
    leaf.sourceId = objectKey(resolveSourceDefinition(occurrence));
    leaf.worldShape = shape;
    try {
        leaf.worldBoundBox = shapeBoundBox(shape);
    }
    catch (const Standard_Failure& exc) {
        leaf.shapeValid = false;
        leaf.diagnostic =
            exc.GetMessageString() ? exc.GetMessageString() : "Failed to compute world bounds";
        leaves.push_back(std::move(leaf));
        return true;
    }
    catch (...) {
        leaf.shapeValid = false;
        leaf.diagnostic = "Unknown failure computing world bounds";
        leaves.push_back(std::move(leaf));
        return true;
    }
    leaf.visible = visible;

    try {
        BRepCheck_Analyzer analyzer(shape);
        leaf.shapeValid = analyzer.IsValid() ? true : false;
        if (!leaf.shapeValid) {
            leaf.diagnostic = "BRepCheck_Analyzer reported invalid geometry";
        }
        else if (!leaf.worldBoundBox.IsValid()) {
            leaf.shapeValid = false;
            leaf.diagnostic = "Missing or invalid world bounds";
        }
    }
    catch (const Standard_Failure& exc) {
        leaf.shapeValid = false;
        leaf.diagnostic = exc.GetMessageString() ? exc.GetMessageString()
                                                 : "BRepCheck_Analyzer failed";
    }
    catch (...) {
        leaf.shapeValid = false;
        leaf.diagnostic = "Unknown failure validating leaf geometry";
    }

    leaves.push_back(std::move(leaf));
    return true;
}

void collectRecursively(
    const AssemblyObject* assembly,
    App::DocumentObject* obj,
    std::vector<App::DocumentObject*> pathFromAssembly,
    std::vector<App::DocumentObject*> visibilityPath,
    bool includeHidden,
    std::unordered_set<App::DocumentObject*>& visiting,
    std::vector<InterferenceLeaf>& leaves
)
{
    if (!obj) {
        return;
    }
    if (visiting.contains(obj)) {
        return;
    }
    if (isHelperOrNonPhysical(obj)) {
        return;
    }

    pathFromAssembly.push_back(obj);
    visibilityPath.push_back(obj);

    if (isContainerToDescend(obj)) {
        visiting.insert(obj);
        for (auto* child : childrenOf(obj)) {
            collectRecursively(
                assembly,
                child,
                pathFromAssembly,
                visibilityPath,
                includeHidden,
                visiting,
                leaves
            );
        }
        visiting.erase(obj);
        return;
    }

    if (auto* body = freecad_cast<Part::BodyBase*>(obj)) {
        tryMakeLeaf(assembly, body, pathFromAssembly, visibilityPath, includeHidden, leaves);
        return;
    }

    if (auto* link = freecad_cast<App::Link*>(obj)) {
        const int elementCount = link->ElementCount.getValue();
        if (elementCount > 0) {
            const bool expanded =
                link->ShowElement.getValue() && !link->ElementList.getValues().empty();
            if (expanded) {
                visiting.insert(obj);
                for (auto* child : link->ElementList.getValues()) {
                    collectRecursively(
                        assembly,
                        child,
                        pathFromAssembly,
                        visibilityPath,
                        includeHidden,
                        visiting,
                        leaves
                    );
                }
                visiting.erase(obj);
            }
            else {
                // Collapsed link array: virtual occurrences via PlacementList indices.
                for (int i = 0; i < elementCount; ++i) {
                    tryMakeLeaf(
                        assembly,
                        link,
                        pathFromAssembly,
                        visibilityPath,
                        includeHidden,
                        leaves,
                        i
                    );
                }
            }
            return;
        }

        auto* linked = link->getLinkedObject(true);
        if (linked && isContainerToDescend(linked) && !freecad_cast<Part::BodyBase*>(linked)) {
            visiting.insert(obj);
            for (auto* child : childrenOf(linked)) {
                collectRecursively(
                    assembly,
                    child,
                    pathFromAssembly,
                    visibilityPath,
                    includeHidden,
                    visiting,
                    leaves
                );
            }
            visiting.erase(obj);
        }
        else {
            tryMakeLeaf(assembly, link, pathFromAssembly, visibilityPath, includeHidden, leaves);
        }
        return;
    }

    tryMakeLeaf(assembly, obj, pathFromAssembly, visibilityPath, includeHidden, leaves);
}

std::pair<std::string, std::string> canonicalSourceIdPair(
    const std::string& a,
    const std::string& b
)
{
    if (a.empty() || b.empty()) {
        return {a, b};
    }
    return (a <= b) ? std::make_pair(a, b) : std::make_pair(b, a);
}

bool isExcludedBySourceId(
    const std::string& sourceA,
    const std::string& sourceB,
    const std::set<std::pair<std::string, std::string>>& excluded
)
{
    if (sourceA.empty() || sourceB.empty()) {
        return false;
    }
    return excluded.contains(canonicalSourceIdPair(sourceA, sourceB));
}

}  // namespace

std::vector<InterferenceLeaf> collectInterferenceLeaves(
    const AssemblyObject* assembly,
    bool includeHidden
)
{
    std::vector<InterferenceLeaf> leaves;
    if (!assembly) {
        return leaves;
    }

    std::unordered_set<App::DocumentObject*> visiting;
    visiting.insert(const_cast<AssemblyObject*>(assembly));
    std::vector<App::DocumentObject*> visibilityRoot {const_cast<AssemblyObject*>(assembly)};
    for (auto* child : assembly->Group.getValues()) {
        collectRecursively(
            assembly,
            child,
            {},
            visibilityRoot,
            includeHidden,
            visiting,
            leaves
        );
    }
    return leaves;
}

std::vector<std::pair<std::size_t, std::size_t>> broadPhaseCandidatePairs(
    const std::vector<InterferenceLeaf>& leaves,
    double clearance,
    double tolerance
)
{
    const double margin = 0.5 * clearance + tolerance;
    struct Item
    {
        std::size_t index = 0;
        Base::BoundBox3d box;
    };
    std::vector<Item> items;
    items.reserve(leaves.size());
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        if (!leaves[i].shapeValid || leaves[i].worldShape.IsNull()) {
            continue;
        }
        Item item;
        item.index = i;
        item.box = leaves[i].worldBoundBox;
        enlargeBoundBox(item.box, margin);
        if (item.box.IsValid()) {
            items.push_back(item);
        }
    }

    if (items.size() < 2) {
        return {};
    }

    Base::BoundBox3d global;
    for (const auto& item : items) {
        global.Add(item.box);
    }
    const double dx = global.MaxX - global.MinX;
    const double dy = global.MaxY - global.MinY;
    const double dz = global.MaxZ - global.MinZ;
    const int axis = (dx >= dy && dx >= dz) ? 0 : (dy >= dz ? 1 : 2);

    auto minOnAxis = [axis](const Base::BoundBox3d& box) {
        return axis == 0 ? box.MinX : (axis == 1 ? box.MinY : box.MinZ);
    };
    auto maxOnAxis = [axis](const Base::BoundBox3d& box) {
        return axis == 0 ? box.MaxX : (axis == 1 ? box.MaxY : box.MaxZ);
    };

    std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        const double amin = minOnAxis(a.box);
        const double bmin = minOnAxis(b.box);
        if (amin < bmin) {
            return true;
        }
        if (amin > bmin) {
            return false;
        }
        return a.index < b.index;
    });

    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    for (std::size_t i = 0; i < items.size(); ++i) {
        for (std::size_t j = i + 1; j < items.size(); ++j) {
            if (minOnAxis(items[j].box) > maxOnAxis(items[i].box)) {
                break;
            }
            if (!boxesOverlap(items[i].box, items[j].box)) {
                continue;
            }
            std::size_t a = items[i].index;
            std::size_t b = items[j].index;
            if (a > b) {
                std::swap(a, b);
            }
            if (a != b) {
                pairs.emplace_back(a, b);
            }
        }
    }

    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}

InterferenceScanResult runInterferenceScan(
    const std::vector<InterferenceLeaf>& leaves,
    const InterferenceScanOptions& options,
    const std::vector<std::pair<std::string, std::string>>& excludedSourceIdPairs
)
{
    InterferenceScanResult result;
    result.leaves = leaves;

    if (options.clearance < 0.0 || !std::isfinite(options.clearance)
        || options.detectionOptions.linearTolerance < 0.0
        || !std::isfinite(options.detectionOptions.linearTolerance)) {
        result.complete = false;
        result.counts.invalidInputs += 1;
        InterferenceComponentIssue issue;
        issue.diagnostic = "Clearance/tolerance must be finite and nonnegative";
        result.componentIssues.push_back(issue);
        return result;
    }

    std::set<std::pair<std::string, std::string>> excluded;
    for (const auto& pair : excludedSourceIdPairs) {
        auto canon = canonicalSourceIdPair(pair.first, pair.second);
        if (canon.first.empty() || canon.second.empty()) {
            continue;
        }
        excluded.insert(std::move(canon));
    }

    // Normalize bounds and demote leaves that cannot participate in pairing.
    for (std::size_t i = 0; i < result.leaves.size(); ++i) {
        auto& leaf = result.leaves[i];
        if (!leaf.shapeValid) {
            InterferenceComponentIssue issue;
            issue.leafIndex = i;
            issue.diagnostic =
                leaf.diagnostic.empty() ? "Invalid leaf geometry" : leaf.diagnostic;
            result.componentIssues.push_back(issue);
            result.counts.invalidInputs += 1;
            continue;
        }
        if (leaf.worldShape.IsNull()) {
            leaf.shapeValid = false;
            leaf.diagnostic = "Null world shape";
            InterferenceComponentIssue issue;
            issue.leafIndex = i;
            issue.diagnostic = leaf.diagnostic;
            result.componentIssues.push_back(issue);
            result.counts.invalidInputs += 1;
            continue;
        }
        if (!leaf.worldBoundBox.IsValid()) {
            try {
                leaf.worldBoundBox = shapeBoundBox(leaf.worldShape);
            }
            catch (...) {
                leaf.worldBoundBox = Base::BoundBox3d();
            }
        }
        if (!leaf.worldBoundBox.IsValid()) {
            leaf.shapeValid = false;
            leaf.diagnostic = "Missing or invalid world bounds";
            InterferenceComponentIssue issue;
            issue.leafIndex = i;
            issue.diagnostic = leaf.diagnostic;
            result.componentIssues.push_back(issue);
            result.counts.invalidInputs += 1;
        }
    }

    if (isCancelled(options)) {
        result.cancelled = true;
        return result;
    }

    const double tolerance = options.detectionOptions.linearTolerance > 0.0
        ? options.detectionOptions.linearTolerance
        : Precision::Confusion();
    const auto candidates =
        broadPhaseCandidatePairs(result.leaves, options.clearance, tolerance);
    const int total = static_cast<int>(candidates.size());
    int current = 0;

    Part::InterferenceOptions detection = options.detectionOptions;
    detection.clearance = options.clearance;
    detection.cancelFlag = options.cancelFlag;
    // Leaves were validated once during collection / normalization above.
    detection.skipGeometryValidation = true;

    // Count theoretically clear pairs pruned by broad-phase among paired-capable leaves.
    std::size_t validLeafCount = 0;
    for (const auto& leaf : result.leaves) {
        if (leaf.shapeValid && !leaf.worldShape.IsNull() && leaf.worldBoundBox.IsValid()) {
            ++validLeafCount;
        }
    }
    const std::size_t allValidPairs =
        validLeafCount < 2 ? 0 : (validLeafCount * (validLeafCount - 1)) / 2;
    result.counts.clearPairs = static_cast<int>(allValidPairs > candidates.size()
                                                    ? allValidPairs - candidates.size()
                                                    : 0);

    for (const auto& candidate : candidates) {
        if (isCancelled(options)) {
            result.cancelled = true;
            return result;
        }

        const auto& leafA = result.leaves[candidate.first];
        const auto& leafB = result.leaves[candidate.second];
        if (!leafA.shapeValid || !leafB.shapeValid) {
            ++current;
            if (options.progress) {
                options.progress(current, total);
            }
            continue;
        }

        InterferencePairResult pairResult;
        pairResult.leafIndexA = candidate.first;
        pairResult.leafIndexB = candidate.second;
        pairResult.detection =
            Part::classifyInterference(leafA.worldShape, leafB.worldShape, detection);

        const bool sourcesExcluded =
            isExcludedBySourceId(leafA.sourceId, leafB.sourceId, excluded);

        switch (pairResult.detection.kind) {
            case Part::InterferenceKind::Clear:
                result.counts.clearPairs += 1;
                break;
            case Part::InterferenceKind::ClearanceViolation:
                pairResult.excluded = sourcesExcluded;
                if (pairResult.excluded) {
                    result.counts.excludedViolations += 1;
                }
                else {
                    result.counts.clearanceViolations += 1;
                }
                result.pairs.push_back(pairResult);
                break;
            case Part::InterferenceKind::Contact:
                pairResult.excluded = sourcesExcluded;
                if (pairResult.excluded) {
                    result.counts.excludedViolations += 1;
                }
                else {
                    result.counts.contacts += 1;
                }
                result.pairs.push_back(pairResult);
                break;
            case Part::InterferenceKind::Penetration:
                pairResult.excluded = sourcesExcluded;
                if (pairResult.excluded) {
                    result.counts.excludedViolations += 1;
                }
                else {
                    result.counts.penetrations += 1;
                }
                result.pairs.push_back(pairResult);
                break;
            case Part::InterferenceKind::InvalidInput:
                result.counts.invalidInputs += 1;
                result.pairs.push_back(pairResult);
                break;
            case Part::InterferenceKind::Inconclusive:
                result.counts.inconclusivePairs += 1;
                result.pairs.push_back(pairResult);
                break;
            case Part::InterferenceKind::Cancelled:
                result.cancelled = true;
                return result;
        }

        ++current;
        if (options.progress) {
            options.progress(current, total);
        }
    }

    result.complete = !result.cancelled && result.counts.invalidInputs == 0
        && result.counts.inconclusivePairs == 0;
    return result;
}

}  // namespace Assembly
