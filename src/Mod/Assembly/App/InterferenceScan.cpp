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
#include <App/Part.h>
#include <App/Datums.h>
#include <App/PropertyUnits.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Tools.h>
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
             && !freecad_cast<App::Link*>(occurrence)
             && !occurrence->isDerivedFrom<App::GeoFeature>()) {
        return false;
    }

    const bool visible = isPathVisible(visibilityPath);
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

    auto* rootPart = freecad_cast<const App::Part*>(root);
    if (!rootPart) {
        return false;
    }

    auto names = Base::Tools::splitSubName(subName);
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

    if (!sawRoot) {
        // Selection rooted at a descendant (e.g. tree-click on a nested feature).
        // Map it to the first component occurrence directly under the root that contains it.
        if (selObj == root) {
            return false;
        }
        App::DocumentObject* owner = nullptr;
        for (auto* child : childrenOf(const_cast<App::DocumentObject*>(root))) {
            if (!child) {
                continue;
            }
            if (child->isDerivedFrom<App::DocumentObjectGroup>() && !freecad_cast<App::Part*>(child)
                && !freecad_cast<AssemblyObject*>(child) && !freecad_cast<AssemblyLink*>(child)) {
                for (auto* nested : childrenOf(child)) {
                    if (!nested || isHelperOrNonPhysical(nested) || nested->isLinkGroup()) {
                        continue;
                    }
                    if (nested == selObj) {
                        owner = nested;
                        break;
                    }
                    if (auto* part = freecad_cast<App::Part*>(nested)) {
                        if (part->hasObject(selObj, true)) {
                            owner = nested;
                            break;
                        }
                    }
                    if (auto* link = freecad_cast<App::Link*>(nested)) {
                        if (link == selObj || link->getLinkedObject(true) == selObj) {
                            owner = nested;
                            break;
                        }
                    }
                }
                if (owner) {
                    break;
                }
                continue;
            }
            if (child->isLinkGroup() || isHelperOrNonPhysical(child)) {
                continue;
            }
            if (child == selObj) {
                owner = child;
                break;
            }
            if (auto* part = freecad_cast<App::Part*>(child)) {
                if (part->hasObject(selObj, true)) {
                    owner = child;
                    break;
                }
            }
            if (auto* link = freecad_cast<App::Link*>(child)) {
                if (link == selObj || link->getLinkedObject(true) == selObj) {
                    owner = child;
                    break;
                }
            }
            if (auto* asmLink = freecad_cast<AssemblyLink*>(child)) {
                if (asmLink == selObj || asmLink->hasObject(selObj, true)) {
                    owner = child;
                    break;
                }
            }
        }
        if (!owner || !owner->getNameInDocument()) {
            return false;
        }
        out.component = owner;
        out.occurrencePrefix = std::string(owner->getNameInDocument()) + ".";
        out.displayPath = owner->Label.getValue();
        return true;
    }

    auto isDigits = [](const std::string& token) {
        return !token.empty()
            && std::all_of(token.begin(), token.end(), [](unsigned char ch) {
                   return std::isdigit(ch) != 0;
               });
    };

    App::DocumentObject* component = nullptr;
    std::string prefix;
    bool acceptArrayIndex = false;

    for (std::size_t i = start; i < names.size(); ++i) {
        const std::string& name = names[i];
        if (name.empty()) {
            continue;
        }

        if (component) {
            if (acceptArrayIndex && isDigits(name)) {
                prefix += name;
                prefix += '.';
                acceptArrayIndex = false;
            }
            break;
        }

        if (!doc) {
            break;
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
        if (cur->isDerivedFrom<App::DocumentObjectGroup>() && !freecad_cast<App::Part*>(cur)
            && !freecad_cast<AssemblyObject*>(cur) && !freecad_cast<AssemblyLink*>(cur)) {
            continue;
        }
        if (cur->isLinkGroup()) {
            continue;
        }
        if (isHelperOrNonPhysical(cur)) {
            continue;
        }

        component = cur;
        prefix = std::string(cur->getNameInDocument()) + ".";
        acceptArrayIndex = freecad_cast<App::Link*>(cur) != nullptr;
    }

    if (!component || prefix.empty()) {
        return false;
    }

    // Component must be a first-level (non-group) occurrence under the root.
    bool direct = false;
    for (auto* child : childrenOf(const_cast<App::DocumentObject*>(root))) {
        if (!child) {
            continue;
        }
        if (child->isDerivedFrom<App::DocumentObjectGroup>() && !freecad_cast<App::Part*>(child)
            && !freecad_cast<AssemblyObject*>(child) && !freecad_cast<AssemblyLink*>(child)) {
            for (auto* nested : childrenOf(child)) {
                if (nested == component) {
                    direct = true;
                    break;
                }
            }
            if (direct) {
                break;
            }
            continue;
        }
        if (child->isLinkGroup()) {
            continue;
        }
        if (child == component) {
            direct = true;
            break;
        }
    }
    if (!direct) {
        return false;
    }

    out.component = component;
    out.occurrencePrefix = std::move(prefix);
    out.displayPath = component->Label.getValue();
    return true;
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
