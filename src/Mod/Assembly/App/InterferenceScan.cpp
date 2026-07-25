// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"
#ifndef _PreComp_
# include <BRepBndLib.hxx>
# include <BRepCheck_Analyzer.hxx>
# include <Bnd_Box.hxx>
# include <TopExp_Explorer.hxx>
# include <algorithm>
# include <cmath>
# include <limits>
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
#include <App/ElementNamingUtils.h>
#include <App/GeoFeature.h>
#include <App/GroupExtension.h>
#include <App/Link.h>
#include <App/Part.h>
#include <App/Datums.h>
#include <App/PropertyUnits.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Tools.h>
#include <algorithm>
#include <functional>
#include <Mod/Part/App/BodyBase.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/PartDesign/App/Body.h>
#include <Precision.hxx>
#include <cmath>
#include <cctype>
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

bool isPlainOrganizerGroup(const App::DocumentObject* obj)
{
    return obj && obj->isDerivedFrom<App::DocumentObjectGroup>()
        && !freecad_cast<const App::Part*>(obj) && !freecad_cast<const AssemblyObject*>(obj)
        && !freecad_cast<const AssemblyLink*>(obj) && !obj->isLinkGroup()
        && !isHelperOrNonPhysical(obj);
}

bool isDigitsToken(const std::string& token)
{
    return !token.empty()
        && std::all_of(token.begin(), token.end(), [](unsigned char ch) {
               return std::isdigit(ch) != 0;
           });
}

/** Parse a non-negative int without throwing on overflow/malformed input. */
bool tryParseNonNegativeInt(const std::string& token, int& out)
{
    if (!isDigitsToken(token)) {
        return false;
    }
    unsigned long long value = 0;
    for (unsigned char ch : token) {
        value = value * 10ull + static_cast<unsigned long long>(ch - '0');
        if (value > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
            return false;
        }
    }
    out = static_cast<int>(value);
    return true;
}

bool expandedLinkElementOwns(
    App::DocumentObject* element,
    App::DocumentObject* selObj
)
{
    if (!element || !selObj) {
        return false;
    }
    if (element == selObj) {
        return true;
    }
    if (auto* part = freecad_cast<App::Part*>(element)) {
        return part->hasObject(selObj, true);
    }
    if (element->isLink()) {
        if (auto* linked = element->getLinkedObject(true)) {
            if (linked == selObj) {
                return true;
            }
            if (auto* part = freecad_cast<App::Part*>(linked)) {
                return part->hasObject(selObj, true);
            }
        }
    }
    if (auto* group = element->getExtensionByType<App::GroupExtension>(true)) {
        return group->hasObject(selObj, true);
    }
    return false;
}

App::DocumentObject* findExpandedLinkElement(
    App::Link* link,
    const std::string& name
)
{
    if (!link || name.empty()) {
        return nullptr;
    }
    for (auto* child : link->ElementList.getValues()) {
        if (child && child->getNameInDocument() && name == child->getNameInDocument()) {
            return child;
        }
    }
    return nullptr;
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

/**
 * GeoFeatureGroup containers (Assembly / Part) also claim nested objects into
 * their flat Group list. Prefer the hierarchical path through organizer groups
 * and nested Parts so Folder.Component. (and Component.Nested.Solid) wins over
 * a flat duplicate claim.
 */
bool isOwnedBySiblingStructuralGroup(
    App::DocumentObject* obj,
    App::DocumentObject* container
)
{
    if (!obj || !container) {
        return false;
    }
    for (auto* sibling : childrenOf(container)) {
        if (!sibling || sibling == obj || isHelperOrNonPhysical(sibling)) {
            continue;
        }
        if (auto* link = freecad_cast<App::Link*>(sibling)) {
            if (link->ElementCount.getValue() > 0) {
                for (auto* element : link->ElementList.getValues()) {
                    if (element == obj) {
                        return true;
                    }
                }
            }
        }
        if (!(isPlainOrganizerGroup(sibling) || freecad_cast<App::Part*>(sibling)
              || freecad_cast<AssemblyObject*>(sibling)
              || freecad_cast<AssemblyLink*>(sibling))) {
            continue;
        }
        if (auto* group = sibling->getExtensionByType<App::GroupExtension>(true)) {
            if (group->hasObject(obj, true)) {
                return true;
            }
        }
    }
    return false;
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
    const App::DocumentObject* root,
    App::DocumentObject* occurrence,
    const std::string& occurrenceSubName
)
{
    if (!root || !occurrence || occurrenceSubName.empty()) {
        return {};
    }

    // Resolve exclusively through the root/subobject path so nested
    // App::Part / Link / AssemblyLink placements are included. No manual
    // placement composition fallback.
    return Part::Feature::getShape(
        const_cast<App::DocumentObject*>(root),
        Part::ShapeOption::ResolveLink | Part::ShapeOption::Transform,
        occurrenceSubName.c_str()
    );
}

bool tryMakeLeaf(
    const App::DocumentObject* root,
    App::DocumentObject* occurrence,
    const std::vector<App::DocumentObject*>& pathFromRoot,
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
             && !occurrence->isLink()
             && !freecad_cast<App::Link*>(occurrence)
             && !occurrence->isDerivedFrom<App::GeoFeature>()) {
        // App::Link with ElementCount>0 reports isLink()==false (isLinkGroup);
        // still accept it via freecad_cast<App::Link*>.
        return false;
    }

    const bool pathVisible = isPathVisible(visibilityPath);
    bool visible = pathVisible;
    if (arrayIndex >= 0 && occurrence) {
        const std::string element = std::to_string(arrayIndex);
        const int elementVis = occurrence->isElementVisible(element.c_str());
        if (elementVis == 0) {
            visible = false;
        }
    }
    if (!includeHidden && !visible) {
        return false;
    }

    std::string subName = makeOccurrenceSubName(pathFromRoot);
    if (arrayIndex >= 0) {
        subName += std::to_string(arrayIndex);
        subName += '.';
    }
    std::string display = makeDisplayPath(pathFromRoot);
    if (arrayIndex >= 0) {
        display += '.';
        display += std::to_string(arrayIndex);
    }

    TopoDS_Shape shape;
    try {
        shape = resolveWorldShape(root, occurrence, subName);
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
    // Shape extraction requires DocumentObject access (GUI/safe thread). Defer
    // BRepCheck_Analyzer to the worker via validateOwnedLeafGeometry().
    leaf.shapeValid = leaf.worldBoundBox.IsValid();
    if (!leaf.shapeValid) {
        leaf.diagnostic = "Missing or invalid world bounds";
    }

    leaves.push_back(std::move(leaf));
    return true;
}

void collectRecursively(
    const App::DocumentObject* root,
    App::DocumentObject* obj,
    std::vector<App::DocumentObject*> pathFromRoot,
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

    pathFromRoot.push_back(obj);
    visibilityPath.push_back(obj);

    if (isContainerToDescend(obj)) {
        visiting.insert(obj);
        for (auto* child : childrenOf(obj)) {
            if (isOwnedBySiblingStructuralGroup(child, obj)) {
                continue;
            }
            collectRecursively(root,
                child,
                pathFromRoot,
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
        tryMakeLeaf(root, body, pathFromRoot, visibilityPath, includeHidden, leaves);
        return;
    }

    // Expanded App::LinkElement (and other non-App::Link link objects).
    if (obj->isLink() && !freecad_cast<App::Link*>(obj)) {
        auto* linked = obj->getLinkedObject(true);
        if (linked && isContainerToDescend(linked) && !freecad_cast<Part::BodyBase*>(linked)) {
            visiting.insert(obj);
            for (auto* child : childrenOf(linked)) {
                if (isOwnedBySiblingStructuralGroup(child, linked)) {
                    continue;
                }
                collectRecursively(root,
                    child,
                    pathFromRoot,
                    visibilityPath,
                    includeHidden,
                    visiting,
                    leaves
                );
            }
            visiting.erase(obj);
        }
        else {
            tryMakeLeaf(root, obj, pathFromRoot, visibilityPath, includeHidden, leaves);
        }
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
                    collectRecursively(root,
                        child,
                        pathFromRoot,
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
                    tryMakeLeaf(root,
                        link,
                        pathFromRoot,
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
                if (isOwnedBySiblingStructuralGroup(child, linked)) {
                    continue;
                }
                collectRecursively(root,
                    child,
                    pathFromRoot,
                    visibilityPath,
                    includeHidden,
                    visiting,
                    leaves
                );
            }
            visiting.erase(obj);
        }
        else {
            tryMakeLeaf(root, link, pathFromRoot, visibilityPath, includeHidden, leaves);
        }
        return;
    }

    tryMakeLeaf(root, obj, pathFromRoot, visibilityPath, includeHidden, leaves);
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

bool isInterferenceRoot(const App::DocumentObject* obj)
{
    if (!obj) {
        return false;
    }
    // App::Part containers including AssemblyObject / AssemblyLink; exclude Bodies.
    return obj->isDerivedFrom<App::Part>() && !obj->isDerivedFrom<Part::BodyBase>();
}

namespace
{

std::string sourceIdentity(const App::DocumentObject* obj)
{
    if (!obj || !obj->isAttachedToDocument()) {
        return {};
    }
    return std::string(obj->getDocument()->getName()) + "#" + obj->getNameInDocument();
}

std::string xlinkIdentity(const App::PropertyXLinkSub& link)
{
    if (auto* obj = link.getValue(); obj && obj->isAttachedToDocument()) {
        return sourceIdentity(obj);
    }
    const char* objectName = link.getObjectName();
    if (!objectName || *objectName == '\0') {
        return {};
    }
    const char* docPath = link.getDocumentPath();
    if (docPath && *docPath != '\0') {
        return std::string(docPath) + "#" + objectName;
    }
    if (auto* doc = link.getDocument()) {
        return std::string(doc->getName()) + "#" + objectName;
    }
    if (auto* owner = freecad_cast<App::DocumentObject*>(link.getContainer())) {
        if (owner->getDocument()) {
            return std::string(owner->getDocument()->getName()) + "#" + objectName;
        }
    }
    return std::string("#") + objectName;
}

std::pair<App::DocumentObject*, App::DocumentObject*> canonicalPair(
    App::DocumentObject* first,
    App::DocumentObject* second
)
{
    if (!first || !second) {
        return {first, second};
    }
    if (sourceIdentity(first) <= sourceIdentity(second)) {
        return {first, second};
    }
    return {second, first};
}

std::vector<InterferenceExclusionRule> rulesFromProperty(const App::PropertyXLinkSubList& prop)
{
    std::vector<InterferenceExclusionRule> rules;
    const auto& links = prop.getSubListValues();
    std::vector<const App::PropertyXLinkSub*> endpoints;
    endpoints.reserve(links.size());
    for (const auto& link : links) {
        endpoints.push_back(&link);
    }

    if (endpoints.size() % 2 != 0) {
        InterferenceExclusionRule malformed;
        malformed.valid = false;
        malformed.diagnostic = "Stored exclusion list has an odd number of endpoints";
        if (!endpoints.empty()) {
            malformed.first = endpoints.back()->getValue();
            malformed.firstIdentity = xlinkIdentity(*endpoints.back());
        }
        rules.push_back(malformed);
        if (!endpoints.empty()) {
            endpoints.pop_back();
        }
    }

    for (std::size_t i = 0; i + 1 < endpoints.size(); i += 2) {
        InterferenceExclusionRule rule;
        rule.first = endpoints[i]->getValue();
        rule.second = endpoints[i + 1]->getValue();
        rule.firstIdentity = xlinkIdentity(*endpoints[i]);
        rule.secondIdentity = xlinkIdentity(*endpoints[i + 1]);
        if (!rule.first || !rule.first->isAttachedToDocument() || !rule.second
            || !rule.second->isAttachedToDocument()) {
            rule.valid = false;
            rule.diagnostic = "Unresolved or deleted exclusion endpoint";
        }
        rules.push_back(rule);
    }
    return rules;
}

App::PropertyLength* ensureClearanceProperty(App::DocumentObject* host)
{
    if (auto* assembly = freecad_cast<AssemblyObject*>(host)) {
        return &assembly->InterferenceClearance;
    }
    if (!host) {
        return nullptr;
    }
    if (auto* existing = dynamic_cast<App::PropertyLength*>(host->getPropertyByName("InterferenceClearance"))) {
        return existing;
    }
    auto* prop = dynamic_cast<App::PropertyLength*>(host->addDynamicProperty(
        "App::PropertyLength",
        "InterferenceClearance",
        "Interference",
        "Minimum clearance for interference checks (0 still reports contact/penetration)",
        App::Prop_NoRecompute
    ));
    if (prop) {
        prop->setValue(0.0);
    }
    return prop;
}

App::PropertyXLinkSubList* ensureExclusionProperty(App::DocumentObject* host)
{
    if (auto* assembly = freecad_cast<AssemblyObject*>(host)) {
        return &assembly->InterferenceExcludedSources;
    }
    if (!host) {
        return nullptr;
    }
    if (auto* existing =
            dynamic_cast<App::PropertyXLinkSubList*>(host->getPropertyByName("InterferenceExcludedSources"))) {
        return existing;
    }
    return dynamic_cast<App::PropertyXLinkSubList*>(host->addDynamicProperty(
        "App::PropertyXLinkSubList",
        "InterferenceExcludedSources",
        "Interference",
        "Alternating source-definition endpoints for excluded unordered pairs",
        App::Prop_Hidden | App::Prop_NoRecompute
    ));
}

const App::PropertyLength* clearanceProperty(const App::DocumentObject* host)
{
    if (auto* assembly = freecad_cast<const AssemblyObject*>(host)) {
        return &assembly->InterferenceClearance;
    }
    if (!host) {
        return nullptr;
    }
    return dynamic_cast<const App::PropertyLength*>(host->getPropertyByName("InterferenceClearance"));
}

const App::PropertyXLinkSubList* exclusionProperty(const App::DocumentObject* host)
{
    if (auto* assembly = freecad_cast<const AssemblyObject*>(host)) {
        return &assembly->InterferenceExcludedSources;
    }
    if (!host) {
        return nullptr;
    }
    return dynamic_cast<const App::PropertyXLinkSubList*>(
        host->getPropertyByName("InterferenceExcludedSources")
    );
}

}  // namespace

std::vector<InterferenceLeaf> collectInterferenceLeaves(
    const App::DocumentObject* root,
    bool includeHidden
)
{
    std::vector<InterferenceLeaf> leaves;
    if (!isInterferenceRoot(root)) {
        return leaves;
    }

    auto* mutableRoot = const_cast<App::DocumentObject*>(root);
    std::unordered_set<App::DocumentObject*> visiting;
    visiting.insert(mutableRoot);
    std::vector<App::DocumentObject*> visibilityRoot {mutableRoot};
    for (auto* child : childrenOf(mutableRoot)) {
        if (isOwnedBySiblingStructuralGroup(child, mutableRoot)) {
            continue;
        }
        collectRecursively(
            root,
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

bool resolveInterferenceComponentOccurrence(
    const App::DocumentObject* root,
    App::DocumentObject* selObj,
    const std::string& subName,
    InterferenceComponentOccurrence& out
)
{
    out = {};
    if (!isInterferenceRoot(root) || !selObj || !root->isAttachedToDocument()) {
        return false;
    }

    const std::string normalizedSub = normalizeInterferenceSubName(selObj, subName);
    auto names = Base::Tools::splitSubName(normalizedSub);
    if (selObj->getNameInDocument()) {
        names.insert(names.begin(), selObj->getNameInDocument());
    }

    App::Document* doc = selObj->getDocument();
    if (!doc) {
        doc = root->getDocument();
    }

    std::size_t start = 0;
    bool sawRoot = false;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i].empty() || !doc) {
            continue;
        }
        App::DocumentObject* cur = doc->getObject(names[i].c_str());
        if (!cur) {
            continue;
        }
        if (cur->isLink()) {
            if (auto* linked = cur->getLinkedObject()) {
                if (linked->getDocument()) {
                    doc = linked->getDocument();
                }
            }
        }
        if (cur == root) {
            sawRoot = true;
            start = i + 1;
            doc = root->getDocument();
            break;
        }
    }

    auto emitOccurrence =
        [&](App::DocumentObject* component,
            const std::vector<App::DocumentObject*>& path,
            int arrayIndex) {
            if (!component || !component->getNameInDocument()) {
                return false;
            }
            out.component = component;
            std::ostringstream prefix;
            std::ostringstream display;
            bool firstLabel = true;
            for (auto* obj : path) {
                if (!obj || !obj->getNameInDocument()) {
                    continue;
                }
                prefix << obj->getNameInDocument() << '.';
                if (!firstLabel) {
                    display << '.';
                }
                firstLabel = false;
                display << obj->Label.getValue();
            }
            if (arrayIndex >= 0) {
                prefix << arrayIndex << '.';
                display << '.' << arrayIndex;
            }
            out.occurrencePrefix = prefix.str();
            out.displayPath = display.str();
            return !out.occurrencePrefix.empty();
        };

    // Path-based resolution when the root appears in the selection subname.
    if (sawRoot) {
        std::vector<App::DocumentObject*> path;
        App::DocumentObject* component = nullptr;
        int arrayIndex = -1;
        bool acceptArrayIndex = false;
        doc = root->getDocument();

        for (std::size_t i = start; i < names.size(); ++i) {
            const std::string& name = names[i];
            if (name.empty()) {
                continue;
            }
            if (component) {
                if (acceptArrayIndex) {
                    if (auto* link = freecad_cast<App::Link*>(component)) {
                        const bool expanded = link->ShowElement.getValue()
                            && !link->ElementList.getValues().empty();
                        if (expanded) {
                            if (auto* element = findExpandedLinkElement(link, name)) {
                                path.push_back(element);
                                component = element;
                                arrayIndex = -1;
                                acceptArrayIndex = false;
                            }
                            else {
                                return false;
                            }
                        }
                        else {
                            int parsedIndex = -1;
                            if (tryParseNonNegativeInt(name, parsedIndex)
                                && parsedIndex < link->ElementCount.getValue()) {
                                arrayIndex = parsedIndex;
                                acceptArrayIndex = false;
                            }
                            else {
                                return false;
                            }
                        }
                    }
                    else {
                        return false;
                    }
                }
                break;
            }
            if (!doc) {
                break;
            }
            if (isDigitsToken(name)) {
                continue;
            }
            App::DocumentObject* cur = doc->getObject(name.c_str());
            if (!cur) {
                continue;
            }
            if (cur->isLink()) {
                if (auto* linked = cur->getLinkedObject()) {
                    if (linked->getDocument()) {
                        doc = linked->getDocument();
                    }
                }
            }
            if (isHelperOrNonPhysical(cur)) {
                continue;
            }
            if (isPlainOrganizerGroup(cur)) {
                path.push_back(cur);
                continue;
            }
            if (cur->isLinkGroup() && freecad_cast<App::Link*>(cur)
                && freecad_cast<App::Link*>(cur)->ElementCount.getValue() <= 0) {
                continue;
            }

            component = cur;
            path.push_back(cur);
            acceptArrayIndex = freecad_cast<App::Link*>(cur)
                && freecad_cast<App::Link*>(cur)->ElementCount.getValue() > 0;
        }

        if (!component) {
            return false;
        }
        // Array links require an element index or expanded LinkElement token.
        if (acceptArrayIndex) {
            return false;
        }
        return emitOccurrence(component, path, arrayIndex);
    }

    // Selection rooted at a descendant: find the top-level occurrence that owns it.
    if (selObj == root) {
        return false;
    }

    std::function<bool(
        App::DocumentObject*,
        std::vector<App::DocumentObject*>,
        std::vector<App::DocumentObject*>
    )>
        search;
    search = [&](App::DocumentObject* obj,
                 std::vector<App::DocumentObject*> path,
                 std::vector<App::DocumentObject*> vis) -> bool {
        if (!obj || isHelperOrNonPhysical(obj)) {
            return false;
        }
        path.push_back(obj);
        vis.push_back(obj);

        if (isPlainOrganizerGroup(obj)) {
            for (auto* child : childrenOf(obj)) {
                if (isOwnedBySiblingStructuralGroup(child, obj)) {
                    continue;
                }
                if (search(child, path, vis)) {
                    return true;
                }
            }
            return false;
        }

        if (auto* link = freecad_cast<App::Link*>(obj)) {
            const int elementCount = link->ElementCount.getValue();
            if (elementCount > 0) {
                const bool expanded =
                    link->ShowElement.getValue() && !link->ElementList.getValues().empty();
                if (expanded) {
                    for (auto* child : link->ElementList.getValues()) {
                        if (!child) {
                            continue;
                        }
                        if (expandedLinkElementOwns(child, selObj)) {
                            std::vector<App::DocumentObject*> elementPath = path;
                            elementPath.push_back(child);
                            return emitOccurrence(child, elementPath, -1);
                        }
                    }
                    return false;
                }
                // Collapsed array: ownership is the link itself (any element).
                if (link == selObj || link->getLinkedObject(true) == selObj) {
                    return emitOccurrence(link, path, 0);
                }
                return false;
            }
        }

        bool owns = (obj == selObj);
        if (!owns) {
            if (auto* part = freecad_cast<App::Part*>(obj)) {
                owns = part->hasObject(selObj, true);
            }
            else if (auto* link = freecad_cast<App::Link*>(obj)) {
                owns = link->getLinkedObject(true) == selObj;
            }
            else if (auto* asmLink = freecad_cast<AssemblyLink*>(obj)) {
                owns = asmLink->hasObject(selObj, true);
            }
        }
        if (owns) {
            return emitOccurrence(obj, path, -1);
        }
        return false;
    };

    std::vector<App::DocumentObject*> empty;
    auto* mutableRoot = const_cast<App::DocumentObject*>(root);
    std::vector<App::DocumentObject*> visRoot {mutableRoot};
    for (auto* child : childrenOf(mutableRoot)) {
        if (isOwnedBySiblingStructuralGroup(child, mutableRoot)) {
            continue;
        }
        if (search(child, empty, visRoot)) {
            return true;
        }
    }
    return false;
}


std::vector<InterferenceLeaf> collectInterferenceLeavesUnderPrefix(
    const App::DocumentObject* root,
    const std::string& occurrencePrefix,
    bool includeHidden
)
{
    std::vector<InterferenceLeaf> filtered;
    if (occurrencePrefix.empty()) {
        return filtered;
    }
    auto leaves = collectInterferenceLeaves(root, includeHidden);
    filtered.reserve(leaves.size());
    for (auto& leaf : leaves) {
        if (leaf.occurrenceSubName.rfind(occurrencePrefix, 0) == 0) {
            filtered.push_back(std::move(leaf));
        }
    }
    return filtered;
}

namespace
{

std::string joinObjectPathNames(const std::vector<App::DocumentObject*>& path)
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

std::string joinObjectPathLabels(const std::vector<App::DocumentObject*>& path)
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

bool pathVisibleIncludingElement(
    const std::vector<App::DocumentObject*>& visibilityPath,
    App::DocumentObject* elementOwner,
    int arrayIndex
)
{
    if (!isPathVisible(visibilityPath)) {
        return false;
    }
    if (arrayIndex < 0 || !elementOwner) {
        return true;
    }
    const std::string element = std::to_string(arrayIndex);
    return elementOwner->isElementVisible(element.c_str()) != 0;
}

void appendComponentOccurrence(
    std::vector<InterferenceComponentOccurrence>& out,
    App::DocumentObject* component,
    const std::vector<App::DocumentObject*>& pathFromRoot,
    const std::vector<App::DocumentObject*>& visibilityPath,
    int arrayIndex,
    bool includeHidden
)
{
    if (!component || !component->getNameInDocument()) {
        return;
    }
    if (!includeHidden
        && !pathVisibleIncludingElement(visibilityPath, component, arrayIndex)) {
        return;
    }

    InterferenceComponentOccurrence occ;
    occ.component = component;
    occ.occurrencePrefix = joinObjectPathNames(pathFromRoot);
    if (arrayIndex >= 0) {
        occ.occurrencePrefix += std::to_string(arrayIndex);
        occ.occurrencePrefix += '.';
    }
    occ.displayPath = joinObjectPathLabels(pathFromRoot);
    if (arrayIndex >= 0) {
        occ.displayPath += '.';
        occ.displayPath += std::to_string(arrayIndex);
    }
    out.push_back(std::move(occ));
}

void listComponentsRecursively(
    const App::DocumentObject* root,
    App::DocumentObject* obj,
    std::vector<App::DocumentObject*> pathFromRoot,
    std::vector<App::DocumentObject*> visibilityPath,
    bool includeHidden,
    std::unordered_set<App::DocumentObject*>& visiting,
    std::vector<InterferenceComponentOccurrence>& out
)
{
    if (!obj || !root) {
        return;
    }
    if (visiting.contains(obj)) {
        return;
    }
    if (isHelperOrNonPhysical(obj)) {
        return;
    }

    pathFromRoot.push_back(obj);
    visibilityPath.push_back(obj);

    if (isPlainOrganizerGroup(obj)) {
        visiting.insert(obj);
        for (auto* child : childrenOf(obj)) {
            if (isOwnedBySiblingStructuralGroup(child, obj)) {
                continue;
            }
            listComponentsRecursively(
                root,
                child,
                pathFromRoot,
                visibilityPath,
                includeHidden,
                visiting,
                out
            );
        }
        visiting.erase(obj);
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
                    if (!child || isHelperOrNonPhysical(child)) {
                        continue;
                    }
                    // Each expanded element is its own top-level occurrence.
                    std::vector<App::DocumentObject*> elementPath = pathFromRoot;
                    std::vector<App::DocumentObject*> elementVis = visibilityPath;
                    elementPath.push_back(child);
                    elementVis.push_back(child);
                    appendComponentOccurrence(
                        out,
                        child,
                        elementPath,
                        elementVis,
                        -1,
                        includeHidden
                    );
                }
                visiting.erase(obj);
            }
            else {
                for (int i = 0; i < elementCount; ++i) {
                    appendComponentOccurrence(
                        out,
                        link,
                        pathFromRoot,
                        visibilityPath,
                        i,
                        includeHidden
                    );
                }
            }
            return;
        }
    }

    // Part, Body, Feature, non-array Link, AssemblyLink, etc.
    appendComponentOccurrence(out, obj, pathFromRoot, visibilityPath, -1, includeHidden);
}

bool optionsInvalid(const InterferenceScanOptions& options)
{
    return options.clearance < 0.0 || !std::isfinite(options.clearance)
        || options.detectionOptions.linearTolerance < 0.0
        || !std::isfinite(options.detectionOptions.linearTolerance);
}

/**
 * Worker-safe validation of already-owned leaf shapes: bounds normalization and
 * BRepCheck_Analyzer. Must not touch DocumentObject pointers.
 */
void validateOwnedLeafGeometry(
    std::vector<InterferenceLeaf>& leaves,
    InterferenceScanResult& result,
    const InterferenceScanOptions& options
)
{
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        if (isCancelled(options)) {
            result.cancelled = true;
            return;
        }
        auto& leaf = leaves[i];
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
            continue;
        }
        try {
            BRepCheck_Analyzer analyzer(leaf.worldShape);
            if (!analyzer.IsValid()) {
                leaf.shapeValid = false;
                leaf.diagnostic = "BRepCheck_Analyzer reported invalid geometry";
                InterferenceComponentIssue issue;
                issue.leafIndex = i;
                issue.diagnostic = leaf.diagnostic;
                result.componentIssues.push_back(issue);
                result.counts.invalidInputs += 1;
            }
        }
        catch (const Standard_Failure& exc) {
            leaf.shapeValid = false;
            leaf.diagnostic =
                exc.GetMessageString() ? exc.GetMessageString() : "BRepCheck_Analyzer failed";
            InterferenceComponentIssue issue;
            issue.leafIndex = i;
            issue.diagnostic = leaf.diagnostic;
            result.componentIssues.push_back(issue);
            result.counts.invalidInputs += 1;
        }
        catch (...) {
            leaf.shapeValid = false;
            leaf.diagnostic = "Unknown failure validating leaf geometry";
            InterferenceComponentIssue issue;
            issue.leafIndex = i;
            issue.diagnostic = leaf.diagnostic;
            result.componentIssues.push_back(issue);
            result.counts.invalidInputs += 1;
        }
    }
}

}  // namespace

std::vector<InterferenceComponentOccurrence> listInterferenceComponentOccurrences(
    const App::DocumentObject* root,
    bool includeHidden
)
{
    std::vector<InterferenceComponentOccurrence> result;
    if (!isInterferenceRoot(root)) {
        return result;
    }

    std::unordered_set<App::DocumentObject*> visiting;
    visiting.insert(const_cast<App::DocumentObject*>(root));
    std::vector<App::DocumentObject*> emptyPath;
    auto* mutableRoot = const_cast<App::DocumentObject*>(root);
    std::vector<App::DocumentObject*> visibilityRoot {mutableRoot};
    for (auto* child : childrenOf(mutableRoot)) {
        if (isOwnedBySiblingStructuralGroup(child, mutableRoot)) {
            continue;
        }
        listComponentsRecursively(
            root,
            child,
            emptyPath,
            visibilityRoot,
            includeHidden,
            visiting,
            result
        );
    }
    return result;
}

InterferenceComponentScanSnapshot prepareInterferenceComponentScanSnapshot(
    const App::DocumentObject* root,
    bool includeHidden,
    const InterferenceScanOptions& options,
    const std::function<void()>& testBarrier
)
{
    InterferenceComponentScanSnapshot snap;
    if (isCancelled(options)) {
        snap.cancelled = true;
        return snap;
    }
    if (!isInterferenceRoot(root) || !root->isAttachedToDocument()) {
        return snap;
    }

    snap.components = listInterferenceComponentOccurrences(root, includeHidden);
    if (isCancelled(options)) {
        snap.cancelled = true;
        return snap;
    }
    if (testBarrier) {
        testBarrier();
    }
    // After the barrier the host DocumentObject may have been destroyed. Do not
    // touch root again unless the scan is still live; callers that destroy the
    // host must set cancelFlag inside the barrier before deleting.
    if (isCancelled(options)) {
        snap.cancelled = true;
        snap.components.clear();
        return snap;
    }
    if (!root->isAttachedToDocument()) {
        snap.cancelled = true;
        snap.components.clear();
        return snap;
    }

    snap.leaves = collectInterferenceLeaves(root, includeHidden);
    snap.componentIndexOfLeaf.assign(snap.leaves.size(), std::string::npos);
    for (std::size_t li = 0; li < snap.leaves.size(); ++li) {
        std::size_t best = std::string::npos;
        std::size_t bestLen = 0;
        for (std::size_t ci = 0; ci < snap.components.size(); ++ci) {
            const auto& prefix = snap.components[ci].occurrencePrefix;
            if (prefix.empty()) {
                continue;
            }
            if (snap.leaves[li].occurrenceSubName.rfind(prefix, 0) == 0
                && prefix.size() > bestLen) {
                best = ci;
                bestLen = prefix.size();
            }
        }
        snap.componentIndexOfLeaf[li] = best;
    }
    return snap;
}

std::string normalizeInterferenceSubName(App::DocumentObject* obj, const std::string& subName)
{
    if (subName.empty()) {
        return subName;
    }

    std::string candidate;
    if (obj) {
        App::ElementNamePair elementName;
        App::GeoFeature::resolveElement(obj, subName.c_str(), elementName, /*append=*/true);
        candidate = elementName.oldName;
    }

    // Reject missing-element markers or residual mapped tokens; fall back to
    // deterministic Data::oldElementName stripping (UtilsAssembly-compatible).
    const char* element = candidate.empty() ? nullptr : Data::findElementName(candidate.c_str());
    const bool unusable = candidate.empty()
        || candidate.find(Data::MISSING_PREFIX) != std::string::npos
        || (element && Data::isMappedElement(element));
    if (unusable) {
        candidate = Data::oldElementName(subName.c_str());
    }
    else {
        const std::string stripped = Data::oldElementName(candidate.c_str());
        if (!stripped.empty()) {
            candidate = stripped;
        }
    }
    return candidate.empty() ? subName : candidate;
}

App::DocumentObject* resolveInterferenceHostFromHandles(
    const std::vector<InterferenceSelectionHandle>& handles,
    App::DocumentObject* editModeAssemblyOrNull
)
{
    // Selection-backed interference roots (SelectionEx object with subelement picks).
    std::vector<App::DocumentObject*> selectionRoots;
    for (const auto& handle : handles) {
        if (!handle.object || handle.subName.empty() || !isInterferenceRoot(handle.object)) {
            continue;
        }
        if (std::find(selectionRoots.begin(), selectionRoots.end(), handle.object)
            == selectionRoots.end()) {
            selectionRoots.push_back(handle.object);
        }
    }

    auto handlesForRoot = [&](App::DocumentObject* root) {
        std::vector<InterferenceSelectionHandle> scoped;
        for (const auto& handle : handles) {
            if (handle.object == root) {
                scoped.push_back(handle);
            }
        }
        return scoped;
    };

    // Prefer a selection root that yields an exact selected-pair scope.
    for (auto* root : selectionRoots) {
        const auto scope = resolveInterferenceSelectionScope(root, handlesForRoot(root));
        if (scope.mode == InterferenceScanScopeMode::SelectedPair) {
            return root;
        }
    }

    // Any selection root that owns at least one resolvable subelement pick.
    for (auto* root : selectionRoots) {
        for (const auto& handle : handlesForRoot(root)) {
            InterferenceComponentOccurrence occ;
            if (resolveInterferenceComponentOccurrence(root, handle.object, handle.subName, occ)) {
                return root;
            }
        }
        // Explicit picks under this root still identify it as the host even if
        // occurrence resolution is incomplete (e.g. mid-edit geometry).
        return root;
    }

    if (editModeAssemblyOrNull && isInterferenceRoot(editModeAssemblyOrNull)) {
        return editModeAssemblyOrNull;
    }

    // Whole-object selection of an interference root.
    for (const auto& handle : handles) {
        if (handle.object && handle.subName.empty() && isInterferenceRoot(handle.object)) {
            return handle.object;
        }
    }
    return nullptr;
}

InterferenceSelectionScope resolveInterferenceSelectionScope(
    const App::DocumentObject* root,
    const std::vector<InterferenceSelectionHandle>& handles
)
{
    InterferenceSelectionScope scope;
    if (!isInterferenceRoot(root)) {
        return scope;
    }

    std::vector<InterferenceComponentOccurrence> resolved;
    for (const auto& handle : handles) {
        if (!handle.object) {
            continue;
        }
        // Literal subelement requirement: empty subName is whole-object, not a handle.
        if (handle.subName.empty()) {
            continue;
        }
        scope.subelementHandleCount += 1;
        InterferenceComponentOccurrence occ;
        if (resolveInterferenceComponentOccurrence(root, handle.object, handle.subName, occ)) {
            resolved.push_back(std::move(occ));
        }
    }

    std::vector<InterferenceComponentOccurrence> unique;
    for (auto& occ : resolved) {
        bool seen = false;
        for (const auto& existing : unique) {
            if (existing.occurrencePrefix == occ.occurrencePrefix) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            unique.push_back(std::move(occ));
        }
    }
    scope.distinctOccurrenceCount = static_cast<int>(unique.size());

    if (scope.subelementHandleCount == 2 && unique.size() == 2) {
        scope.mode = InterferenceScanScopeMode::SelectedPair;
        scope.first = std::move(unique[0]);
        scope.second = std::move(unique[1]);
    }
    return scope;
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

    if (isCancelled(options)) {
        result.cancelled = true;
        return result;
    }

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

    // Normalize bounds and validate owned shapes on the worker thread.
    validateOwnedLeafGeometry(result.leaves, result, options);
    if (result.cancelled) {
        return result;
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

InterferenceScanResult runInterferenceScanBetweenLeafSets(
    const std::vector<InterferenceLeaf>& leavesA,
    const std::vector<InterferenceLeaf>& leavesB,
    const InterferenceScanOptions& options,
    const std::vector<std::pair<std::string, std::string>>& excludedSourceIdPairs
)
{
    InterferenceScanResult result;
    result.leaves.reserve(leavesA.size() + leavesB.size());
    result.leaves.insert(result.leaves.end(), leavesA.begin(), leavesA.end());
    result.leaves.insert(result.leaves.end(), leavesB.begin(), leavesB.end());

    if (isCancelled(options)) {
        result.cancelled = true;
        return result;
    }

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

    validateOwnedLeafGeometry(result.leaves, result, options);
    if (result.cancelled) {
        return result;
    }

    if (isCancelled(options)) {
        result.cancelled = true;
        return result;
    }

    const double tolerance = options.detectionOptions.linearTolerance > 0.0
        ? options.detectionOptions.linearTolerance
        : Precision::Confusion();
    const double margin = 0.5 * options.clearance + tolerance;
    const std::size_t offsetB = leavesA.size();

    std::vector<std::pair<std::size_t, std::size_t>> candidates;
    std::size_t validA = 0;
    std::size_t validB = 0;
    for (std::size_t i = 0; i < leavesA.size(); ++i) {
        if (result.leaves[i].shapeValid && !result.leaves[i].worldShape.IsNull()
            && result.leaves[i].worldBoundBox.IsValid()) {
            ++validA;
        }
    }
    for (std::size_t j = 0; j < leavesB.size(); ++j) {
        const std::size_t idx = offsetB + j;
        if (result.leaves[idx].shapeValid && !result.leaves[idx].worldShape.IsNull()
            && result.leaves[idx].worldBoundBox.IsValid()) {
            ++validB;
        }
    }

    for (std::size_t i = 0; i < leavesA.size(); ++i) {
        if (!result.leaves[i].shapeValid || result.leaves[i].worldShape.IsNull()
            || !result.leaves[i].worldBoundBox.IsValid()) {
            continue;
        }
        Base::BoundBox3d boxA = result.leaves[i].worldBoundBox;
        enlargeBoundBox(boxA, margin);
        for (std::size_t j = 0; j < leavesB.size(); ++j) {
            const std::size_t idx = offsetB + j;
            if (!result.leaves[idx].shapeValid || result.leaves[idx].worldShape.IsNull()
                || !result.leaves[idx].worldBoundBox.IsValid()) {
                continue;
            }
            Base::BoundBox3d boxB = result.leaves[idx].worldBoundBox;
            enlargeBoundBox(boxB, margin);
            if (boxesOverlap(boxA, boxB)) {
                candidates.emplace_back(i, idx);
            }
        }
    }

    const int total = static_cast<int>(candidates.size());
    int current = 0;
    const std::size_t allCrossPairs = validA * validB;
    result.counts.clearPairs = static_cast<int>(
        allCrossPairs > candidates.size() ? allCrossPairs - candidates.size() : 0
    );

    Part::InterferenceOptions detection = options.detectionOptions;
    detection.clearance = options.clearance;
    detection.cancelFlag = options.cancelFlag;
    detection.skipGeometryValidation = true;

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

InterferenceScanResult runInterferenceScanAcrossComponents(
    const InterferenceComponentScanSnapshot& snapshot,
    const InterferenceScanOptions& options,
    const std::vector<std::pair<std::string, std::string>>& excludedSourceIdPairs
)
{
    InterferenceScanResult acc;
    acc.leaves = snapshot.leaves;
    if (snapshot.cancelled || isCancelled(options)) {
        acc.cancelled = true;
        return acc;
    }

    if (optionsInvalid(options)) {
        acc.complete = false;
        acc.counts.invalidInputs += 1;
        InterferenceComponentIssue issue;
        issue.diagnostic = "Clearance/tolerance must be finite and nonnegative";
        acc.componentIssues.push_back(issue);
        return acc;
    }

    validateOwnedLeafGeometry(acc.leaves, acc, options);
    if (acc.cancelled) {
        return acc;
    }

    if (isCancelled(options)) {
        acc.cancelled = true;
        return acc;
    }

    std::set<std::pair<std::string, std::string>> excluded;
    for (const auto& pair : excludedSourceIdPairs) {
        auto canon = canonicalSourceIdPair(pair.first, pair.second);
        if (canon.first.empty() || canon.second.empty()) {
            continue;
        }
        excluded.insert(std::move(canon));
    }

    const double tolerance = options.detectionOptions.linearTolerance > 0.0
        ? options.detectionOptions.linearTolerance
        : Precision::Confusion();
    const double margin = 0.5 * options.clearance + tolerance;
    const auto& componentOfLeaf = snapshot.componentIndexOfLeaf;

    std::vector<std::pair<std::size_t, std::size_t>> candidates;
    std::size_t validCrossPairs = 0;
    for (std::size_t i = 0; i < acc.leaves.size(); ++i) {
        if (i >= componentOfLeaf.size() || componentOfLeaf[i] == std::string::npos) {
            continue;
        }
        if (!acc.leaves[i].shapeValid || acc.leaves[i].worldShape.IsNull()
            || !acc.leaves[i].worldBoundBox.IsValid()) {
            continue;
        }
        Base::BoundBox3d boxA = acc.leaves[i].worldBoundBox;
        enlargeBoundBox(boxA, margin);
        for (std::size_t j = i + 1; j < acc.leaves.size(); ++j) {
            if (j >= componentOfLeaf.size() || componentOfLeaf[j] == std::string::npos) {
                continue;
            }
            if (componentOfLeaf[i] == componentOfLeaf[j]) {
                continue;
            }
            if (!acc.leaves[j].shapeValid || acc.leaves[j].worldShape.IsNull()
                || !acc.leaves[j].worldBoundBox.IsValid()) {
                continue;
            }
            ++validCrossPairs;
            Base::BoundBox3d boxB = acc.leaves[j].worldBoundBox;
            enlargeBoundBox(boxB, margin);
            if (boxesOverlap(boxA, boxB)) {
                candidates.emplace_back(i, j);
            }
        }
    }

    acc.counts.clearPairs = static_cast<int>(
        validCrossPairs > candidates.size() ? validCrossPairs - candidates.size() : 0
    );

    Part::InterferenceOptions detection = options.detectionOptions;
    detection.clearance = options.clearance;
    detection.cancelFlag = options.cancelFlag;
    detection.skipGeometryValidation = true;

    const int total = static_cast<int>(candidates.size());
    int current = 0;
    for (const auto& candidate : candidates) {
        if (isCancelled(options)) {
            acc.cancelled = true;
            return acc;
        }
        const auto& leafA = acc.leaves[candidate.first];
        const auto& leafB = acc.leaves[candidate.second];
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
                acc.counts.clearPairs += 1;
                break;
            case Part::InterferenceKind::ClearanceViolation:
                pairResult.excluded = sourcesExcluded;
                if (pairResult.excluded) {
                    acc.counts.excludedViolations += 1;
                }
                else {
                    acc.counts.clearanceViolations += 1;
                }
                acc.pairs.push_back(pairResult);
                break;
            case Part::InterferenceKind::Contact:
                pairResult.excluded = sourcesExcluded;
                if (pairResult.excluded) {
                    acc.counts.excludedViolations += 1;
                }
                else {
                    acc.counts.contacts += 1;
                }
                acc.pairs.push_back(pairResult);
                break;
            case Part::InterferenceKind::Penetration:
                pairResult.excluded = sourcesExcluded;
                if (pairResult.excluded) {
                    acc.counts.excludedViolations += 1;
                }
                else {
                    acc.counts.penetrations += 1;
                }
                acc.pairs.push_back(pairResult);
                break;
            case Part::InterferenceKind::InvalidInput:
                acc.counts.invalidInputs += 1;
                acc.pairs.push_back(pairResult);
                break;
            case Part::InterferenceKind::Inconclusive:
                acc.counts.inconclusivePairs += 1;
                acc.pairs.push_back(pairResult);
                break;
            case Part::InterferenceKind::Cancelled:
                acc.cancelled = true;
                return acc;
        }
        ++current;
        if (options.progress) {
            options.progress(current, total);
        }
    }

    acc.complete = !acc.cancelled && acc.counts.invalidInputs == 0
        && acc.counts.inconclusivePairs == 0;
    return acc;
}

InterferenceScanResult runInterferenceScanAllVisibleComponents(
    const App::DocumentObject* root,
    bool includeHidden,
    const InterferenceScanOptions& options,
    const std::vector<std::pair<std::string, std::string>>& excludedSourceIdPairs
)
{
    auto snapshot = prepareInterferenceComponentScanSnapshot(root, includeHidden, options);
    if (snapshot.cancelled) {
        InterferenceScanResult cancelled;
        cancelled.cancelled = true;
        cancelled.leaves = std::move(snapshot.leaves);
        return cancelled;
    }
    return runInterferenceScanAcrossComponents(snapshot, options, excludedSourceIdPairs);
}

double getInterferenceClearance(const App::DocumentObject* host)
{
    if (const auto* prop = clearanceProperty(host)) {
        return prop->getValue();
    }
    return 0.0;
}

void setInterferenceClearance(App::DocumentObject* host, double clearanceMm)
{
    if (clearanceMm < 0.0 || !std::isfinite(clearanceMm)) {
        throw Base::ValueError("Interference clearance must be finite and nonnegative");
    }
    auto* prop = ensureClearanceProperty(host);
    if (!prop) {
        throw Base::ValueError("Interference clearance host must be an App::Part root");
    }
    prop->setValue(clearanceMm);
}

std::vector<InterferenceExclusionRule> getInterferenceExclusionRules(const App::DocumentObject* host)
{
    if (const auto* prop = exclusionProperty(host)) {
        return rulesFromProperty(*prop);
    }
    return {};
}

bool hasInterferenceExclusion(
    const App::DocumentObject* host,
    App::DocumentObject* first,
    App::DocumentObject* second
)
{
    if (!first || !second) {
        return false;
    }
    const auto want = canonicalPair(first, second);
    for (const auto& rule : getInterferenceExclusionRules(host)) {
        if (!rule.valid) {
            continue;
        }
        const auto have = canonicalPair(rule.first, rule.second);
        if (have.first == want.first && have.second == want.second) {
            return true;
        }
    }
    return false;
}

void addInterferenceExclusion(
    App::DocumentObject* host,
    App::DocumentObject* first,
    App::DocumentObject* second
)
{
    if (!first || !second) {
        throw Base::ValueError("Exclusion endpoints must be valid document objects");
    }
    if (!first->isAttachedToDocument() || !second->isAttachedToDocument()) {
        throw Base::ValueError("Exclusion endpoints must be attached to a document");
    }
    if (hasInterferenceExclusion(host, first, second)) {
        return;
    }
    auto* prop = ensureExclusionProperty(host);
    if (!prop) {
        throw Base::ValueError("Interference exclusion host must be an App::Part root");
    }
    auto canon = canonicalPair(first, second);
    prop->append(canon.first);
    prop->append(canon.second);
}

void removeInterferenceExclusionAt(App::DocumentObject* host, std::size_t ruleIndex)
{
    auto* prop = ensureExclusionProperty(host);
    if (!prop) {
        throw Base::ValueError("Interference exclusion host must be an App::Part root");
    }
    const int size = prop->getSize();
    if (size % 2 != 0) {
        throw Base::ValueError("Stored exclusion list has an odd number of endpoints");
    }
    const auto ruleCount = static_cast<std::size_t>(size / 2);
    if (ruleIndex >= ruleCount) {
        throw Base::ValueError("Exclusion rule index out of range");
    }
    prop->removeIndices(static_cast<int>(ruleIndex * 2), 2);
}

void removeInterferenceExclusion(
    App::DocumentObject* host,
    App::DocumentObject* first,
    App::DocumentObject* second
)
{
    if (!first || !second) {
        return;
    }
    const auto want = canonicalPair(first, second);
    const auto rules = getInterferenceExclusionRules(host);
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const auto& rule = rules[i];
        if (!rule.valid || !rule.first || !rule.second) {
            continue;
        }
        const auto have = canonicalPair(rule.first, rule.second);
        if (have.first == want.first && have.second == want.second) {
            removeInterferenceExclusionAt(host, i);
            return;
        }
    }
}

}  // namespace Assembly
