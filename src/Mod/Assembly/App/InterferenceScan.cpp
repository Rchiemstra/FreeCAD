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
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <App/Range.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Quantity.h>
#include <Base/Tools.h>
#include <Base/Unit.h>
#include <algorithm>
#include <functional>
#include <Mod/Part/App/BodyBase.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShape.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/Spreadsheet/App/Cell.h>
#include <Mod/Spreadsheet/App/Sheet.h>
#include <Precision.hxx>
#include <cmath>
#include <cctype>
#include <limits>
#include <Standard_Failure.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

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

/** Array index token for link arrays: decimal digits only, no leading zeros except "0". */
bool tryParseStrictArrayIndex(const std::string& token, int& out)
{
    if (token.empty() || (token.size() > 1 && token.front() == '0')) {
        return false;
    }
    return tryParseNonNegativeInt(token, out);
}

bool isFilteredNonCollectableLeaf(const App::DocumentObject* obj)
{
    if (!obj || isHelperOrNonPhysical(obj)) {
        return true;
    }
    if (obj->getTypeId().isDerivedFrom(Base::Type::fromName("PartDesign::Feature"))
        && !obj->isDerivedFrom<Part::BodyBase>()) {
        return true;
    }
    return false;
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

struct InterferenceStructuralLeafSite
{
    std::string occurrenceSubName;
    App::DocumentObject* occurrence = nullptr;
    std::vector<App::DocumentObject*> pathFromRoot;
    int arrayIndex = -1;
};

bool tryRecordStructuralLeafSite(
    App::DocumentObject* occurrence,
    const std::vector<App::DocumentObject*>& pathFromRoot,
    const std::vector<App::DocumentObject*>& visibilityPath,
    bool includeHidden,
    std::vector<InterferenceStructuralLeafSite>& sites,
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

    InterferenceStructuralLeafSite site;
    site.occurrenceSubName = std::move(subName);
    site.occurrence = occurrence;
    site.pathFromRoot = pathFromRoot;
    site.arrayIndex = arrayIndex;
    sites.push_back(std::move(site));
    return true;
}

void collectStructuralLeafSitesRecursively(
    const App::DocumentObject* root,
    App::DocumentObject* obj,
    std::vector<App::DocumentObject*> pathFromRoot,
    std::vector<App::DocumentObject*> visibilityPath,
    bool includeHidden,
    std::unordered_set<App::DocumentObject*>& visiting,
    std::vector<InterferenceStructuralLeafSite>& sites
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
            collectStructuralLeafSitesRecursively(
                root,
                child,
                pathFromRoot,
                visibilityPath,
                includeHidden,
                visiting,
                sites
            );
        }
        visiting.erase(obj);
        return;
    }

    if (auto* body = freecad_cast<Part::BodyBase*>(obj)) {
        tryRecordStructuralLeafSite(
            body,
            pathFromRoot,
            visibilityPath,
            includeHidden,
            sites
        );
        return;
    }

    if (obj->isLink() && !freecad_cast<App::Link*>(obj)) {
        auto* linked = obj->getLinkedObject(true);
        if (linked && isContainerToDescend(linked) && !freecad_cast<Part::BodyBase*>(linked)) {
            visiting.insert(obj);
            for (auto* child : childrenOf(linked)) {
                if (isOwnedBySiblingStructuralGroup(child, linked)) {
                    continue;
                }
                collectStructuralLeafSitesRecursively(
                    root,
                    child,
                    pathFromRoot,
                    visibilityPath,
                    includeHidden,
                    visiting,
                    sites
                );
            }
            visiting.erase(obj);
        }
        else {
            tryRecordStructuralLeafSite(
                obj,
                pathFromRoot,
                visibilityPath,
                includeHidden,
                sites
            );
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
                    collectStructuralLeafSitesRecursively(
                        root,
                        child,
                        pathFromRoot,
                        visibilityPath,
                        includeHidden,
                        visiting,
                        sites
                    );
                }
                visiting.erase(obj);
            }
            else {
                for (int i = 0; i < elementCount; ++i) {
                    tryRecordStructuralLeafSite(
                        link,
                        pathFromRoot,
                        visibilityPath,
                        includeHidden,
                        sites,
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
                collectStructuralLeafSitesRecursively(
                    root,
                    child,
                    pathFromRoot,
                    visibilityPath,
                    includeHidden,
                    visiting,
                    sites
                );
            }
            visiting.erase(obj);
        }
        else {
            tryRecordStructuralLeafSite(
                link,
                pathFromRoot,
                visibilityPath,
                includeHidden,
                sites
            );
        }
        return;
    }

    tryRecordStructuralLeafSite(
        obj,
        pathFromRoot,
        visibilityPath,
        includeHidden,
        sites
    );
}

std::vector<InterferenceStructuralLeafSite> listInterferenceStructuralLeafSites(
    const App::DocumentObject* root,
    bool includeHidden
)
{
    std::vector<InterferenceStructuralLeafSite> sites;
    if (!isInterferenceRoot(root)) {
        return sites;
    }

    auto* mutableRoot = const_cast<App::DocumentObject*>(root);
    std::unordered_set<App::DocumentObject*> visiting;
    visiting.insert(mutableRoot);
    std::vector<App::DocumentObject*> emptyPath;
    std::vector<App::DocumentObject*> visibilityRoot {mutableRoot};
    for (auto* child : childrenOf(mutableRoot)) {
        if (isOwnedBySiblingStructuralGroup(child, mutableRoot)) {
            continue;
        }
        collectStructuralLeafSitesRecursively(
            root,
            child,
            emptyPath,
            visibilityRoot,
            includeHidden,
            visiting,
            sites
        );
    }
    return sites;
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

App::PropertyLink* ensureClearanceSheetProperty(App::DocumentObject* host)
{
    if (auto* assembly = freecad_cast<AssemblyObject*>(host)) {
        return &assembly->InterferenceClearanceSheet;
    }
    if (!host) {
        return nullptr;
    }
    if (auto* existing =
            dynamic_cast<App::PropertyLink*>(host->getPropertyByName("InterferenceClearanceSheet"))) {
        return existing;
    }
    return dynamic_cast<App::PropertyLink*>(host->addDynamicProperty(
        "App::PropertyLink",
        "InterferenceClearanceSheet",
        "Interference",
        "Optional spreadsheet of face-specific design clearances",
        App::Prop_NoRecompute
    ));
}

const App::PropertyLink* clearanceSheetProperty(const App::DocumentObject* host)
{
    if (auto* assembly = freecad_cast<const AssemblyObject*>(host)) {
        return &assembly->InterferenceClearanceSheet;
    }
    if (!host) {
        return nullptr;
    }
    return dynamic_cast<const App::PropertyLink*>(host->getPropertyByName("InterferenceClearanceSheet"));
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
                            if (tryParseStrictArrayIndex(name, parsedIndex)
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
    if (!isInterferenceRoot(root) || occurrencePrefix.empty() || !root->isAttachedToDocument()) {
        return filtered;
    }

    auto* mutableRoot = const_cast<App::DocumentObject*>(root);
    App::Document* doc = root->getDocument();
    if (!doc) {
        return filtered;
    }

    auto names = Base::Tools::splitSubName(occurrencePrefix);
    std::vector<App::DocumentObject*> branchPath;
    bool hasArrayToken = false;
    for (const auto& name : names) {
        if (name.empty()) {
            continue;
        }
        if (isDigitsToken(name)) {
            hasArrayToken = true;
            break;
        }
        App::DocumentObject* cur = doc->getObject(name.c_str());
        if (!cur) {
            return filtered;
        }
        branchPath.push_back(cur);
    }

    // Collapsed array elements are virtual occurrences. Resolve them through the
    // structural traversal, then extract only matching occurrence shapes.
    if (hasArrayToken) {
        const auto sites = listInterferenceStructuralLeafSites(root, includeHidden);
        for (const auto& site : sites) {
            if (site.occurrenceSubName.rfind(occurrencePrefix, 0) != 0) {
                continue;
            }
            std::vector<App::DocumentObject*> visibilityPath {mutableRoot};
            visibilityPath.insert(
                visibilityPath.end(),
                site.pathFromRoot.begin(),
                site.pathFromRoot.end()
            );
            tryMakeLeaf(
                root,
                site.occurrence,
                site.pathFromRoot,
                visibilityPath,
                includeHidden,
                filtered,
                site.arrayIndex
            );
        }
        return filtered;
    }
    if (branchPath.empty()) {
        return filtered;
    }

    // Prefix-scoped: traverse only the selected occurrence branch.
    App::DocumentObject* startObj = branchPath.back();
    branchPath.pop_back();
    std::vector<App::DocumentObject*> visibilityPath {mutableRoot};
    for (auto* obj : branchPath) {
        visibilityPath.push_back(obj);
    }

    std::unordered_set<App::DocumentObject*> visiting;
    visiting.insert(mutableRoot);
    for (auto* obj : branchPath) {
        visiting.insert(obj);
    }

    std::vector<InterferenceLeaf> leaves;
    collectRecursively(
        root,
        startObj,
        branchPath,
        visibilityPath,
        includeHidden,
        visiting,
        leaves
    );

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
            issue.kind = InterferenceComponentIssue::Kind::InvalidLeaf;
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
            issue.kind = InterferenceComponentIssue::Kind::InvalidLeaf;
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
            issue.kind = InterferenceComponentIssue::Kind::InvalidLeaf;
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
                issue.kind = InterferenceComponentIssue::Kind::InvalidLeaf;
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
            issue.kind = InterferenceComponentIssue::Kind::InvalidLeaf;
            issue.leafIndex = i;
            issue.diagnostic = leaf.diagnostic;
            result.componentIssues.push_back(issue);
            result.counts.invalidInputs += 1;
        }
        catch (...) {
            leaf.shapeValid = false;
            leaf.diagnostic = "Unknown failure validating leaf geometry";
            InterferenceComponentIssue issue;
            issue.kind = InterferenceComponentIssue::Kind::InvalidLeaf;
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
    // Every whole-object or subelement selection with a live object is an endpoint.
    std::vector<const InterferenceSelectionHandle*> endpoints;
    endpoints.reserve(handles.size());
    for (const auto& handle : handles) {
        if (!handle.object || !handle.object->isAttachedToDocument()) {
            continue;
        }
        endpoints.push_back(&handle);
    }

    auto candidateRootsFor = [](App::DocumentObject* obj) {
        std::vector<App::DocumentObject*> roots;
        if (!obj || !obj->isAttachedToDocument()) {
            return roots;
        }
        if (isInterferenceRoot(obj)) {
            roots.push_back(obj);
        }
        for (const auto& parent : obj->getParents()) {
            if (parent.first && isInterferenceRoot(parent.first)
                && parent.first->isAttachedToDocument()) {
                roots.push_back(parent.first);
            }
        }
        return roots;
    };

    auto sameDocAsEdit = [&](App::DocumentObject* root) {
        return !editModeAssemblyOrNull || !editModeAssemblyOrNull->getDocument()
            || (root->getDocument() == editModeAssemblyOrNull->getDocument());
    };

    // Exact two endpoints: try a shared interference root (may be a common ancestor).
    if (endpoints.size() == 2) {
        const auto& ha = *endpoints[0];
        const auto& hb = *endpoints[1];
        auto rootsA = candidateRootsFor(ha.object);
        auto rootsB = candidateRootsFor(hb.object);

        App::DocumentObject* bestRoot = nullptr;
        int bestDepth = -1;
        for (auto* root : rootsA) {
            if (!root || !sameDocAsEdit(root)) {
                continue;
            }
            if (std::find(rootsB.begin(), rootsB.end(), root) == rootsB.end()) {
                continue;
            }
            const auto scope = resolveInterferenceSelectionScope(root, handles);
            if (scope.mode != InterferenceScanScopeMode::SelectedPair
                || scope.subelementHandleCount != 2
                || scope.distinctOccurrenceCount != 2) {
                continue;
            }
            // Prefer the deepest common root (most parents).
            const int depth = static_cast<int>(root->getParents().size());
            if (depth > bestDepth) {
                bestDepth = depth;
                bestRoot = root;
            }
        }
        if (bestRoot) {
            return bestRoot;
        }
    }

    if (editModeAssemblyOrNull && isInterferenceRoot(editModeAssemblyOrNull)
        && editModeAssemblyOrNull->isAttachedToDocument()) {
        return editModeAssemblyOrNull;
    }

    // No edit-mode assembly: keep selected-root support for general scans.
    for (const auto& handle : handles) {
        if (handle.object && !handle.subName.empty() && isInterferenceRoot(handle.object)
            && handle.object->isAttachedToDocument()) {
            return handle.object;
        }
    }
    for (const auto& handle : handles) {
        if (handle.object && handle.subName.empty() && isInterferenceRoot(handle.object)
            && handle.object->isAttachedToDocument()) {
            return handle.object;
        }
    }
    return nullptr;
}

InterferenceSelectedPairRequest resolveInterferenceSelectedPairRequest(
    const std::vector<InterferenceSelectionHandle>& handles,
    App::DocumentObject* editModeAssemblyOrNull
)
{
    InterferenceSelectedPairRequest request;
    // Reuse host resolution's pair gate, then require SelectedPair on that host.
    // Do not accept an edit-mode / all-components fallback host.
    std::vector<const InterferenceSelectionHandle*> endpoints;
    for (const auto& handle : handles) {
        if (!handle.object || !handle.object->isAttachedToDocument()) {
            continue;
        }
        endpoints.push_back(&handle);
    }
    if (endpoints.size() != 2) {
        return request;
    }

    auto* host = resolveInterferenceHostFromHandles(handles, editModeAssemblyOrNull);
    if (!host) {
        return request;
    }
    // If host came from edit/all-components fallback, scope will not be SelectedPair
    // when endpoints belong to another root — still verify explicitly.
    const auto scope = resolveInterferenceSelectionScope(host, handles);
    if (scope.mode != InterferenceScanScopeMode::SelectedPair
        || scope.subelementHandleCount != 2
        || scope.distinctOccurrenceCount != 2) {
        return request;
    }
    request.host = host;
    request.first = scope.first;
    request.second = scope.second;
    return request;
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
        if (!handle.object || !handle.object->isAttachedToDocument()) {
            continue;
        }
        // Whole-object and subelement picks both count as pair endpoints.
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

// Defined below; used from anonymous-namespace leaf-pair evaluation.
InterferenceClearanceLookup lookupInterferenceClearance(
    const InterferenceClearanceRuleTable& table,
    const std::string& facePathA,
    const std::string& facePathB,
    double assemblyClearanceMm
);

namespace
{

std::string leafFacePath(const InterferenceLeaf& leaf, int faceIndex)
{
    if (faceIndex <= 0) {
        return {};
    }
    std::string base = leaf.occurrenceSubName;
    return base + "Face" + std::to_string(faceIndex);
}

Base::BoundBox3d shapeBoundBoxLocal(const TopoDS_Shape& shape)
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
    double xmin = 0;
    double ymin = 0;
    double zmin = 0;
    double xmax = 0;
    double ymax = 0;
    double zmax = 0;
    bnd.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    box.MinX = xmin;
    box.MinY = ymin;
    box.MinZ = zmin;
    box.MaxX = xmax;
    box.MaxY = ymax;
    box.MaxZ = zmax;
    return box;
}

bool ensureLeafFacesCached(
    InterferenceLeaf& leaf,
    const std::atomic<bool>* cancelFlag
)
{
    if (leaf.facesCached) {
        return true;
    }
    leaf.cachedFaces.clear();
    if (!leaf.worldShape.IsNull()) {
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(leaf.worldShape, TopAbs_FACE, map);
        leaf.cachedFaces.reserve(static_cast<std::size_t>(map.Extent()));
        for (int i = 1; i <= map.Extent(); ++i) {
            if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) {
                return false;
            }
            InterferenceCachedFace ref;
            ref.index = i;
            ref.face = map(i);
            ref.box = shapeBoundBoxLocal(ref.face);
            if (ref.box.IsValid()) {
                leaf.cachedFaces.push_back(std::move(ref));
            }
        }
    }
    leaf.facesCached = true;
    return true;
}

bool boxesWithinClearance(const Base::BoundBox3d& a, const Base::BoundBox3d& b, double clearance)
{
    if (!a.IsValid() || !b.IsValid()) {
        return false;
    }
    Base::BoundBox3d enlarged = a;
    enlarged.MinX -= clearance;
    enlarged.MinY -= clearance;
    enlarged.MinZ -= clearance;
    enlarged.MaxX += clearance;
    enlarged.MaxY += clearance;
    enlarged.MaxZ += clearance;
    return boxesOverlap(enlarged, b);
}

double aabbSeparationHint(const Base::BoundBox3d& a, const Base::BoundBox3d& b)
{
    const double dx = std::max(0.0, std::max(a.MinX - b.MaxX, b.MinX - a.MaxX));
    const double dy = std::max(0.0, std::max(a.MinY - b.MaxY, b.MinY - a.MaxY));
    const double dz = std::max(0.0, std::max(a.MinZ - b.MaxZ, b.MinZ - a.MaxZ));
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Part::InterferenceResult classifyFaceGap(
    const TopoDS_Shape& faceA,
    const TopoDS_Shape& faceB,
    double designClearance,
    double linearTolerance,
    const std::atomic<bool>* cancelFlag
)
{
    Part::InterferenceOptions opts;
    opts.clearance = designClearance;
    opts.linearTolerance = linearTolerance;
    opts.cancelFlag = cancelFlag;
    opts.skipGeometryValidation = true;
    return Part::classifyInterference(faceA, faceB, opts);
}

bool isExcludableViolation(Part::InterferenceKind kind)
{
    return kind == Part::InterferenceKind::Penetration
        || kind == Part::InterferenceKind::Contact
        || kind == Part::InterferenceKind::ClearanceViolation;
}

int interferenceKindRank(Part::InterferenceKind kind)
{
    switch (kind) {
        case Part::InterferenceKind::Penetration:
            return 5;
        case Part::InterferenceKind::ClearanceViolation:
            return 4;
        case Part::InterferenceKind::Contact:
            return 3;
        case Part::InterferenceKind::InvalidInput:
            return 2;
        case Part::InterferenceKind::Inconclusive:
            return 1;
        default:
            return 0;
    }
}

Part::InterferenceKind worstFaceKind(const std::vector<InterferenceFaceHit>& hits)
{
    Part::InterferenceKind worst = Part::InterferenceKind::Clear;
    for (const auto& hit : hits) {
        if (interferenceKindRank(hit.classification) > interferenceKindRank(worst)) {
            worst = hit.classification;
        }
    }
    return worst;
}

std::size_t governingFaceHitIndex(
    const std::vector<InterferenceFaceHit>& hits,
    Part::InterferenceKind governingKind
)
{
    std::size_t selected = static_cast<std::size_t>(-1);
    double selectedDistance = -1.0;
    for (std::size_t i = 0; i < hits.size(); ++i) {
        const auto& hit = hits[i];
        if (hit.classification != governingKind) {
            continue;
        }
        const bool distanceValid = hit.distance >= 0.0 && std::isfinite(hit.distance);
        if (selected == static_cast<std::size_t>(-1)
            || (distanceValid
                && (selectedDistance < 0.0 || hit.distance < selectedDistance))) {
            selected = i;
            selectedDistance = distanceValid ? hit.distance : -1.0;
        }
    }
    return selected;
}

struct FinalizedPairDetection
{
    Part::InterferenceResult detection;
    std::size_t governingFaceHitIndex = static_cast<std::size_t>(-1);
};

FinalizedPairDetection finalizePairDetection(
    const Part::InterferenceResult& solid,
    const std::vector<InterferenceFaceHit>& faceHits,
    bool faceEnumerationCapped,
    const std::string& faceEnumerationDiagnostic
)
{
    FinalizedPairDetection finalized;
    finalized.detection = solid;

    // Whole-solid outcomes have precedence over every face-level result.
    if (solid.kind == Part::InterferenceKind::Penetration
        || solid.kind == Part::InterferenceKind::InvalidInput) {
        return finalized;
    }
    if (solid.kind == Part::InterferenceKind::Inconclusive) {
        finalized.detection.kind = Part::InterferenceKind::Inconclusive;
        if (finalized.detection.diagnostic.empty() && !faceEnumerationDiagnostic.empty()) {
            finalized.detection.diagnostic = faceEnumerationDiagnostic;
        }
        return finalized;
    }

    if (!faceHits.empty()) {
        finalized.detection.kind = worstFaceKind(faceHits);
        finalized.governingFaceHitIndex =
            governingFaceHitIndex(faceHits, finalized.detection.kind);
        if (finalized.governingFaceHitIndex != static_cast<std::size_t>(-1)) {
            const double distance =
                faceHits[finalized.governingFaceHitIndex].distance;
            finalized.detection.minimumDistance =
                distance >= 0.0 && std::isfinite(distance) ? distance : -1.0;
        }
    }
    if (faceEnumerationCapped
        && finalized.detection.kind == Part::InterferenceKind::Clear) {
        finalized.detection.kind = Part::InterferenceKind::Inconclusive;
        finalized.governingFaceHitIndex = static_cast<std::size_t>(-1);
        finalized.detection.minimumDistance = -1.0;
        if (finalized.detection.diagnostic.empty()) {
            finalized.detection.diagnostic = faceEnumerationDiagnostic;
        }
    }
    return finalized;
}

}  // namespace

Part::InterferenceResult finalizeInterferencePairDetection(
    const Part::InterferenceResult& solid,
    const std::vector<InterferenceFaceHit>& faceHits,
    bool faceEnumerationCapped,
    const std::string& faceEnumerationDiagnostic
)
{
    return finalizePairDetection(
               solid,
               faceHits,
               faceEnumerationCapped,
               faceEnumerationDiagnostic
    )
        .detection;
}

namespace
{

void appendClearanceRuleIssues(
    InterferenceScanResult& result,
    const InterferenceClearanceRuleTable& table
)
{
    result.counts.invalidRules += table.invalidRuleCount;
    for (const auto& diag : table.diagnostics) {
        InterferenceComponentIssue issue;
        issue.kind = InterferenceComponentIssue::Kind::InvalidRule;
        issue.diagnostic = diag;
        result.componentIssues.push_back(issue);
    }
    for (const auto& rule : table.rules) {
        if (!rule.enabled || rule.valid) {
            continue;
        }
        InterferenceComponentIssue issue;
        issue.kind = InterferenceComponentIssue::Kind::InvalidRule;
        issue.diagnostic = rule.diagnostic.empty()
            ? ("Invalid clearance rule in row " + std::to_string(rule.spreadsheetRow))
            : rule.diagnostic;
        result.componentIssues.push_back(issue);
    }
}

void recordPairCounts(
    InterferenceScanResult& result,
    InterferencePairResult& pairResult,
    bool sourcesExcluded
)
{
    bool hasExcludable = isExcludableViolation(pairResult.detection.kind);
    for (auto& hit : pairResult.faceHits) {
        hit.suppressedByExclusion = false;
        if (sourcesExcluded && isExcludableViolation(hit.classification)) {
            hit.suppressedByExclusion = true;
            hasExcludable = true;
        }
        switch (hit.classification) {
            case Part::InterferenceKind::Clear:
                result.counts.clearFaceHits += 1;
                break;
            case Part::InterferenceKind::ClearanceViolation:
                hasExcludable = true;
                if (!hit.suppressedByExclusion) {
                    result.counts.clearanceViolations += 1;
                }
                break;
            case Part::InterferenceKind::Contact:
                hasExcludable = true;
                if (!hit.suppressedByExclusion) {
                    result.counts.contacts += 1;
                }
                break;
            case Part::InterferenceKind::InvalidInput:
                result.counts.invalidInputs += 1;
                break;
            case Part::InterferenceKind::Inconclusive:
                // Pair-level Inconclusive is counted once below; avoid double-counting
                // face-level Inconclusive hits on the same component pair.
                if (pairResult.detection.kind != Part::InterferenceKind::Inconclusive) {
                    result.counts.inconclusivePairs += 1;
                }
                break;
            default:
                break;
        }
    }

    switch (pairResult.detection.kind) {
        case Part::InterferenceKind::Clear: {
            const auto worst = worstFaceKind(pairResult.faceHits);
            if (pairResult.faceHits.empty() || worst == Part::InterferenceKind::Clear) {
                result.counts.clearPairs += 1;
            }
            break;
        }
        case Part::InterferenceKind::Penetration:
            hasExcludable = true;
            if (!sourcesExcluded) {
                result.counts.penetrations += 1;
            }
            break;
        case Part::InterferenceKind::InvalidInput:
            if (pairResult.faceHits.empty()) {
                result.counts.invalidInputs += 1;
            }
            break;
        case Part::InterferenceKind::Inconclusive:
            // Exactly one component-pair inconclusive count (solid or aggregated).
            result.counts.inconclusivePairs += 1;
            break;
        case Part::InterferenceKind::ClearanceViolation:
        case Part::InterferenceKind::Contact:
            if (pairResult.faceHits.empty()) {
                hasExcludable = true;
                if (!sourcesExcluded) {
                    if (pairResult.detection.kind == Part::InterferenceKind::Contact) {
                        result.counts.contacts += 1;
                    }
                    else {
                        result.counts.clearanceViolations += 1;
                    }
                }
            }
            break;
        case Part::InterferenceKind::Cancelled:
            return;
    }

    // Exclusions suppress only Penetration / Contact / ClearanceViolation.
    // InvalidInput / Inconclusive stay visible and never set hit.suppressedByExclusion.
    if (sourcesExcluded && hasExcludable) {
        pairResult.excluded = true;
        result.counts.excludedViolations += 1;
    }
    else {
        pairResult.excluded = false;
    }

    if (pairResult.detection.kind != Part::InterferenceKind::Clear || !pairResult.faceHits.empty()
        || hasExcludable
        || pairResult.detection.kind == Part::InterferenceKind::InvalidInput
        || pairResult.detection.kind == Part::InterferenceKind::Inconclusive
        || !pairResult.faceEnumerationDiagnostic.empty()) {
        result.pairs.push_back(std::move(pairResult));
    }
}

bool evaluateLeafPairWithClearanceRules(
    InterferenceScanResult& result,
    std::size_t indexA,
    std::size_t indexB,
    const InterferenceScanOptions& options,
    double maxDesignClearance,
    double linearTolerance,
    const std::set<std::pair<std::string, std::string>>& excluded,
    int pairIndex,
    int pairTotal
)
{
    auto reportProgress = [&](int withinPairNumerator, int withinPairDenominator) {
        if (!options.progress || pairTotal <= 0) {
            return;
        }
        const int scale = 1000;
        const int globalTotal = pairTotal * scale;
        int sub = 0;
        if (withinPairDenominator > 0) {
            sub = (withinPairNumerator * scale) / withinPairDenominator;
            if (sub > scale) {
                sub = scale;
            }
        }
        int current = pairIndex * scale + sub;
        if (current > globalTotal) {
            current = globalTotal;
        }
        options.progress(current, globalTotal);
    };

    auto& leafA = result.leaves[indexA];
    auto& leafB = result.leaves[indexB];
    const bool sourcesExcluded = isExcludedBySourceId(leafA.sourceId, leafB.sourceId, excluded);

    Part::InterferenceResult solid;
    if (options.solidOverride) {
        solid = options.solidOverride->result;
    }
    else {
        Part::InterferenceOptions solidOpts = options.detectionOptions;
        solidOpts.clearance = maxDesignClearance;
        solidOpts.cancelFlag = options.cancelFlag;
        solidOpts.skipGeometryValidation = true;
        solid = Part::classifyInterference(leafA.worldShape, leafB.worldShape, solidOpts);
    }
    if (solid.kind == Part::InterferenceKind::Cancelled) {
        result.cancelled = true;
        return false;
    }

    // Penetration / invalid solid geometry: one component-pair result, no faceHits.
    if (solid.kind == Part::InterferenceKind::Penetration
        || solid.kind == Part::InterferenceKind::InvalidInput) {
        InterferencePairResult pairResult;
        pairResult.leafIndexA = indexA;
        pairResult.leafIndexB = indexB;
        pairResult.detection = solid;
        recordPairCounts(result, pairResult, sourcesExcluded);
        reportProgress(1, 1);
        return true;
    }

    // Always enumerate proximate faces for non-penetrating pairs (including
    // solid Inconclusive — face hits are diagnostics only; kind stays Inconclusive).
    if (!ensureLeafFacesCached(leafA, options.cancelFlag)
        || !ensureLeafFacesCached(leafB, options.cancelFlag)) {
        result.cancelled = true;
        return false;
    }
    const double probe = maxDesignClearance + linearTolerance;

    struct Candidate
    {
        std::size_t ia = 0;
        std::size_t ib = 0;
        double separation = 0.0;
    };
    std::vector<Candidate> candidates;
    for (std::size_t ia = 0; ia < leafA.cachedFaces.size(); ++ia) {
        if (options.cancelFlag && options.cancelFlag->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            return false;
        }
        for (std::size_t ib = 0; ib < leafB.cachedFaces.size(); ++ib) {
            if (options.cancelFlag && options.cancelFlag->load(std::memory_order_relaxed)) {
                result.cancelled = true;
                return false;
            }
            const auto& fa = leafA.cachedFaces[ia];
            const auto& fb = leafB.cachedFaces[ib];
            if (!boxesWithinClearance(fa.box, fb.box, probe)) {
                continue;
            }
            Candidate c;
            c.ia = ia;
            c.ib = ib;
            c.separation = aabbSeparationHint(fa.box, fb.box);
            candidates.push_back(c);
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.separation != b.separation) {
            return a.separation < b.separation;
        }
        if (a.ia != b.ia) {
            return a.ia < b.ia;
        }
        return a.ib < b.ib;
    });

    InterferencePairResult pairResult;
    pairResult.leafIndexA = indexA;
    pairResult.leafIndexB = indexB;
    pairResult.detection = solid;

    const std::size_t cap = options.maxFacePairCandidates > 0 ? options.maxFacePairCandidates
                                                              : candidates.size();
    bool capped = false;
    if (candidates.size() > cap) {
        capped = true;
        pairResult.faceEnumerationDiagnostic =
            "Face-pair candidate set capped at " + std::to_string(cap) + " of "
            + std::to_string(candidates.size())
            + " proximate pairs (nearest AABB first); remaining pairs not evaluated";
        InterferenceComponentIssue issue;
        issue.kind = InterferenceComponentIssue::Kind::FaceEnumerationCapped;
        issue.leafIndex = indexA;
        issue.leafIndexB = indexB;
        issue.diagnostic = pairResult.faceEnumerationDiagnostic;
        result.componentIssues.push_back(issue);
        candidates.resize(cap);
    }

    const int faceTotal = static_cast<int>(candidates.size());
    for (int fi = 0; fi < faceTotal; ++fi) {
        if (options.cancelFlag && options.cancelFlag->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            return false;
        }
        const auto& c = candidates[static_cast<std::size_t>(fi)];
        const auto& fa = leafA.cachedFaces[c.ia];
        const auto& fb = leafB.cachedFaces[c.ib];
        const std::string pathA = leafFacePath(leafA, fa.index);
        const std::string pathB = leafFacePath(leafB, fb.index);
        const auto lookup = lookupInterferenceClearance(
            options.clearanceRules,
            pathA,
            pathB,
            options.clearance
        );
        auto gap = classifyFaceGap(
            fa.face,
            fb.face,
            lookup.clearanceMm,
            linearTolerance,
            options.cancelFlag
        );
        if (gap.kind == Part::InterferenceKind::Cancelled) {
            result.cancelled = true;
            return false;
        }

        InterferenceFaceHit hit;
        hit.facePathA = pathA;
        hit.facePathB = pathB;
        hit.distance = gap.minimumDistance;
        hit.appliedClearance = lookup.clearanceMm;
        hit.classification = gap.kind;
        hit.ruleKind = lookup.kind;
        hit.sourceRows = lookup.sourceRows;
        hit.sourceComments = lookup.sourceComments;
        hit.diagnostic = lookup.diagnostic.empty() ? gap.diagnostic : lookup.diagnostic;
        hit.closestPointsValid = gap.minimumDistance >= 0.0
            && std::isfinite(gap.minimumDistance)
            && std::isfinite(gap.pointOnFirst.x) && std::isfinite(gap.pointOnFirst.y)
            && std::isfinite(gap.pointOnFirst.z) && std::isfinite(gap.pointOnSecond.x)
            && std::isfinite(gap.pointOnSecond.y) && std::isfinite(gap.pointOnSecond.z);
        if (hit.closestPointsValid) {
            hit.pointOnFirst = gap.pointOnFirst;
            hit.pointOnSecond = gap.pointOnSecond;
        }
        hit.commonShape = std::move(gap.commonShape);
        // Invalid enabled spreadsheet rules must not silently fall back to a Clear
        // AssemblyGlobal classification (e.g. typo "0.5 garbage" rejected → host 0).
        if (options.clearanceRules.invalidRuleCount > 0
            && hit.ruleKind == InterferenceClearanceRuleKind::AssemblyGlobal
            && hit.classification == Part::InterferenceKind::Clear) {
            hit.classification = Part::InterferenceKind::Inconclusive;
            if (hit.diagnostic.empty()) {
                hit.diagnostic =
                    "Clearance inconclusive while spreadsheet contains invalid enabled rules";
            }
        }
        pairResult.faceHits.push_back(std::move(hit));
        reportProgress(fi + 1, std::max(1, faceTotal));
    }

    if (faceTotal == 0) {
        reportProgress(1, 1);
    }

    auto finalized = finalizePairDetection(
        solid,
        pairResult.faceHits,
        capped,
        pairResult.faceEnumerationDiagnostic
    );
    pairResult.detection = std::move(finalized.detection);
    pairResult.governingFaceHitIndex = finalized.governingFaceHitIndex;

    recordPairCounts(result, pairResult, sourcesExcluded);
    return true;
}

}  // namespace

double conservativeMaxDesignClearance(
    const InterferenceClearanceRuleTable& table,
    double hostClearanceMm
)
{
    double maxC = table.maxEnabledClearance;
    if (table.hasDefaultStar) {
        maxC = std::max(maxC, table.defaultStarClearance);
    }
    // Host fallback remains reachable for unmatched faces when no * default exists.
    if (!table.hasDefaultStar) {
        maxC = std::max(maxC, hostClearanceMm);
    }
    // Empty sheet → host only.
    if (table.rules.empty()) {
        maxC = std::max(maxC, hostClearanceMm);
    }
    if (!std::isfinite(maxC) || maxC < 0.0) {
        return 0.0;
    }
    return maxC;
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
    const double maxDesignClearance =
        conservativeMaxDesignClearance(options.clearanceRules, options.clearance);
    appendClearanceRuleIssues(result, options.clearanceRules);
    const auto candidates =
        broadPhaseCandidatePairs(result.leaves, maxDesignClearance, tolerance);
    const int total = static_cast<int>(candidates.size());
    int current = 0;
    bool sawCap = false;

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
            if (options.progress && total > 0) {
                options.progress(current * 1000, total * 1000);
            }
            continue;
        }

        const std::size_t pairsBefore = result.pairs.size();
        if (!evaluateLeafPairWithClearanceRules(
                result,
                candidate.first,
                candidate.second,
                options,
                maxDesignClearance,
                tolerance,
                excluded,
                current,
                std::max(1, total)
            )) {
            return result;
        }
        if (result.pairs.size() > pairsBefore
            && !result.pairs.back().faceEnumerationDiagnostic.empty()) {
            sawCap = true;
        }

        ++current;
    }

    result.complete = !result.cancelled && result.counts.invalidInputs == 0
        && result.counts.inconclusivePairs == 0 && !sawCap
        && result.counts.invalidRules == 0;
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
    const double maxDesignClearance =
        conservativeMaxDesignClearance(options.clearanceRules, options.clearance);
    const double margin = 0.5 * maxDesignClearance + tolerance;
    const std::size_t offsetB = leavesA.size();

    appendClearanceRuleIssues(result, options.clearanceRules);

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

    result.counts.clearPairs = static_cast<int>(
        (validA * validB) > candidates.size() ? (validA * validB) - candidates.size() : 0
    );

    const int total = static_cast<int>(candidates.size());
    int current = 0;
    bool sawCap = false;
    for (const auto& candidate : candidates) {
        if (isCancelled(options)) {
            result.cancelled = true;
            return result;
        }

        const auto& leafA = result.leaves[candidate.first];
        const auto& leafB = result.leaves[candidate.second];
        if (!leafA.shapeValid || !leafB.shapeValid) {
            ++current;
            if (options.progress && total > 0) {
                options.progress(current * 1000, total * 1000);
            }
            continue;
        }

        const std::size_t pairsBefore = result.pairs.size();
        if (!evaluateLeafPairWithClearanceRules(
                result,
                candidate.first,
                candidate.second,
                options,
                maxDesignClearance,
                tolerance,
                excluded,
                current,
                std::max(1, total)
            )) {
            return result;
        }
        if (result.pairs.size() > pairsBefore
            && !result.pairs.back().faceEnumerationDiagnostic.empty()) {
            sawCap = true;
        }
        ++current;
    }

    result.complete = !result.cancelled && result.counts.invalidInputs == 0
        && result.counts.inconclusivePairs == 0 && !sawCap
        && result.counts.invalidRules == 0;
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
        issue.kind = InterferenceComponentIssue::Kind::Other;
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
    const double maxDesignClearance =
        conservativeMaxDesignClearance(options.clearanceRules, options.clearance);
    const double margin = 0.5 * maxDesignClearance + tolerance;
    const auto& componentOfLeaf = snapshot.componentIndexOfLeaf;

    appendClearanceRuleIssues(acc, options.clearanceRules);

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

    // Deterministic candidate order.
    std::sort(candidates.begin(), candidates.end());

    acc.counts.clearPairs = static_cast<int>(
        validCrossPairs > candidates.size() ? validCrossPairs - candidates.size() : 0
    );

    const int total = static_cast<int>(candidates.size());
    int current = 0;
    bool sawCap = false;
    for (const auto& candidate : candidates) {
        if (isCancelled(options)) {
            acc.cancelled = true;
            return acc;
        }
        const auto& leafA = acc.leaves[candidate.first];
        const auto& leafB = acc.leaves[candidate.second];
        if (!leafA.shapeValid || !leafB.shapeValid) {
            ++current;
            if (options.progress && total > 0) {
                options.progress(current * 1000, total * 1000);
            }
            continue;
        }

        const std::size_t pairsBefore = acc.pairs.size();
        if (!evaluateLeafPairWithClearanceRules(
                acc,
                candidate.first,
                candidate.second,
                options,
                maxDesignClearance,
                tolerance,
                excluded,
                current,
                std::max(1, total)
            )) {
            return acc;
        }
        if (acc.pairs.size() > pairsBefore
            && !acc.pairs.back().faceEnumerationDiagnostic.empty()) {
            sawCap = true;
        }
        ++current;
    }

    acc.complete = !acc.cancelled && acc.counts.invalidInputs == 0
        && acc.counts.inconclusivePairs == 0 && !sawCap
        && acc.counts.invalidRules == 0;
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

App::DocumentObject* getInterferenceClearanceSheet(const App::DocumentObject* host)
{
    if (const auto* prop = clearanceSheetProperty(host)) {
        return prop->getValue();
    }
    return nullptr;
}

void setInterferenceClearanceSheet(App::DocumentObject* host, App::DocumentObject* sheetOrNull)
{
    auto* prop = ensureClearanceSheetProperty(host);
    if (!prop) {
        throw Base::ValueError("Interference clearance sheet host must be an App::Part root");
    }
    if (sheetOrNull && !freecad_cast<Spreadsheet::Sheet*>(sheetOrNull)) {
        throw Base::ValueError("Interference clearance sheet must be a Spreadsheet::Sheet");
    }
    prop->setValue(sheetOrNull);
}

namespace
{

std::string sheetCellText(const Spreadsheet::Sheet* sheet, int row, int col)
{
    if (!sheet) {
        return {};
    }
    const Spreadsheet::Cell* cell = sheet->getCell(App::CellAddress(row, col));
    if (!cell) {
        return {};
    }
    std::string text;
    cell->getStringContent(text);
    if (!text.empty() && text.front() == '\'') {
        text.erase(0, 1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(0, 1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

bool sheetCellNumber(const Spreadsheet::Sheet* sheet, int row, int col, double& out, std::string* failReason)
{
    if (!sheet) {
        if (failReason) {
            *failReason = "Missing sheet";
        }
        return false;
    }
    const App::CellAddress addr(row, col);
    const std::string name = addr.toString();
    if (auto* prop = dynamic_cast<App::PropertyQuantity*>(sheet->getPropertyByName(name.c_str()))) {
        if (prop->getUnit() != Base::Unit() && prop->getUnit() != Base::Unit::Length) {
            if (failReason) {
                *failReason = "Tolerance is not a length quantity";
            }
            return false;
        }
        out = prop->getValue();
        if (!std::isfinite(out)) {
            if (failReason) {
                *failReason = "Tolerance is non-finite";
            }
            return false;
        }
        return true;
    }
    if (auto* prop = dynamic_cast<App::PropertyFloat*>(sheet->getPropertyByName(name.c_str()))) {
        out = prop->getValue();
        if (!std::isfinite(out)) {
            if (failReason) {
                *failReason = "Tolerance is non-finite";
            }
            return false;
        }
        return true;
    }
    const std::string text = sheetCellText(sheet, row, col);
    if (text.empty()) {
        if (failReason) {
            *failReason = "Tolerance is missing";
        }
        return false;
    }
    try {
        Base::Quantity quantity = Base::Quantity::parse(text);
        if (quantity.getUnit() != Base::Unit() && quantity.getUnit() != Base::Unit::Length) {
            if (failReason) {
                *failReason = "Tolerance is not a length quantity";
            }
            return false;
        }
        out = quantity.getValueAs(Base::Quantity::MilliMetre);
        if (!std::isfinite(out)) {
            if (failReason) {
                *failReason = "Tolerance is non-finite";
            }
            return false;
        }
        return true;
    }
    catch (...) {
        // Fall back to strict numeric parse with no trailing garbage.
    }
    try {
        size_t idx = 0;
        out = std::stod(text, &idx);
        while (idx < text.size() && std::isspace(static_cast<unsigned char>(text[idx]))) {
            ++idx;
        }
        if (idx != text.size()) {
            if (failReason) {
                *failReason = "Tolerance contains trailing garbage";
            }
            return false;
        }
        if (!std::isfinite(out)) {
            if (failReason) {
                *failReason = "Tolerance is non-finite";
            }
            return false;
        }
        return true;
    }
    catch (...) {
        if (failReason) {
            *failReason = "Tolerance is not a number";
        }
        return false;
    }
}

bool parseEnabledFlag(const std::string& text, bool& enabled)
{
    std::string lower = text;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower.empty() || lower == "1" || lower == "true" || lower == "yes" || lower == "y") {
        enabled = true;
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no" || lower == "n") {
        enabled = false;
        return true;
    }
    return false;
}

std::string canonicalFaceKey(std::string face)
{
    while (!face.empty() && std::isspace(static_cast<unsigned char>(face.front()))) {
        face.erase(0, 1);
    }
    while (!face.empty() && std::isspace(static_cast<unsigned char>(face.back()))) {
        face.pop_back();
    }
    if (face == "*") {
        return face;
    }
    return normalizeInterferenceSubName(nullptr, face);
}

bool isValidFaceElementToken(const std::string& token)
{
    if (token.size() < 5 || token.compare(0, 4, "Face") != 0) {
        return false;
    }
    const std::string num = token.substr(4);
    if (num.empty() || (num.size() > 1 && num.front() == '0')) {
        return false;
    }
    // Reject absurdly large tokens without throwing / overflowing int parsers.
    if (num.size() > 9) {
        return false;
    }
    for (char c : num) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    try {
        const long value = std::stol(num);
        return value >= 1 && value <= static_cast<long>(std::numeric_limits<int>::max());
    }
    catch (...) {
        return false;
    }
}

struct InterferenceClearanceHostValidator
{
    const App::DocumentObject* root = nullptr;
    std::vector<InterferenceStructuralLeafSite> sites;
    const std::vector<InterferenceLeaf>* preparedLeaves = nullptr;
    std::unordered_map<std::string, const InterferenceLeaf*> preparedBySubName;
    struct OccurrenceFaceCacheEntry
    {
        TopoDS_Shape shape;
        bool resolved = false;
        bool shapeValid = false;
        int faceCount = 0;
    };
    std::unordered_map<std::string, OccurrenceFaceCacheEntry> mutable occurrenceCache;
    InterferenceClearanceSheetParseStats* parseStats = nullptr;

    InterferenceClearanceHostValidator(
        const App::DocumentObject* host,
        const std::vector<InterferenceLeaf>* prepared,
        InterferenceClearanceSheetParseStats* stats
    )
        : root(host)
        , preparedLeaves(prepared)
        , parseStats(stats)
    {
        if (!host || !isInterferenceRoot(host)) {
            return;
        }
        if (parseStats) {
            ++parseStats->structuralOccurrenceTraversalPasses;
        }
        sites = listInterferenceStructuralLeafSites(host, /*includeHidden=*/true);
        if (preparedLeaves) {
            for (const auto& leaf : *preparedLeaves) {
                preparedBySubName.emplace(leaf.occurrenceSubName, &leaf);
            }
        }
    }

    int faceIndexFromElement(const std::string& faceElement) const
    {
        if (!isValidFaceElementToken(faceElement)) {
            return -1;
        }
        try {
            return std::stoi(faceElement.substr(4));
        }
        catch (...) {
            return -1;
        }
    }

    const OccurrenceFaceCacheEntry& ensureOccurrenceFaceCache(const InterferenceStructuralLeafSite& site) const
    {
        auto& cache = occurrenceCache[site.occurrenceSubName];
        if (cache.resolved) {
            if (parseStats) {
                ++parseStats->occurrenceValidationCacheHits;
            }
            return cache;
        }
        cache.resolved = true;
        const auto foundPrepared = preparedBySubName.find(site.occurrenceSubName);
        if (foundPrepared != preparedBySubName.end()) {
            const InterferenceLeaf* leaf = foundPrepared->second;
            if (leaf && leaf->shapeValid && !leaf->worldShape.IsNull()) {
                cache.shape = leaf->worldShape;
                cache.shapeValid = true;
                if (parseStats) {
                    ++parseStats->occurrenceValidationCacheHits;
                }
            }
        }
        if (!cache.shapeValid) {
            if (parseStats) {
                ++parseStats->leafShapeExtractions;
                ++parseStats->lazyOccurrenceValidations;
            }
            try {
                cache.shape = resolveWorldShape(root, site.occurrence, site.occurrenceSubName);
            }
            catch (...) {
                cache.shapeValid = false;
                cache.faceCount = 0;
                return cache;
            }
        }
        try {
            cache.shapeValid = !cache.shape.IsNull() && shapeHasSolid(cache.shape);
            if (cache.shapeValid) {
                if (parseStats) {
                    ++parseStats->faceEnumerationPasses;
                }
                TopTools_IndexedMapOfShape map;
                TopExp::MapShapes(cache.shape, TopAbs_FACE, map);
                cache.faceCount = map.Extent();
            }
        }
        catch (const Base::Exception&) {
            cache.shapeValid = false;
            cache.faceCount = 0;
        }
        catch (const Standard_Failure&) {
            cache.shapeValid = false;
            cache.faceCount = 0;
        }
        catch (...) {
            cache.shapeValid = false;
            cache.faceCount = 0;
        }
        return cache;
    }

    bool faceExistsOnSite(const InterferenceStructuralLeafSite& site, const std::string& faceElement) const
    {
        const int faceIndex = faceIndexFromElement(faceElement);
        if (faceIndex < 1) {
            return false;
        }
        const auto& cache = ensureOccurrenceFaceCache(site);
        if (!cache.shapeValid) {
            return false;
        }
        return faceIndex <= cache.faceCount;
    }

    bool emittedFacePathExists(const std::string& facePath) const
    {
        if (facePath.empty()) {
            return false;
        }
        auto parts = Base::Tools::splitSubName(facePath);
        while (!parts.empty() && parts.back().empty()) {
            parts.pop_back();
        }
        if (parts.empty()) {
            return false;
        }
        const std::string faceElement = parts.back();
        for (const auto& site : sites) {
            const std::string emitted = site.occurrenceSubName + faceElement;
            if (emitted == facePath && faceExistsOnSite(site, faceElement)) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> findSuffixAliasFacePathMatches(const std::string& candidateFacePath) const
    {
        std::vector<std::string> matches;
        if (candidateFacePath.empty()) {
            return matches;
        }
        auto parts = Base::Tools::splitSubName(candidateFacePath);
        while (!parts.empty() && parts.back().empty()) {
            parts.pop_back();
        }
        if (parts.empty()) {
            return matches;
        }
        const std::string faceElement = parts.back();
        for (const auto& site : sites) {
            const std::string emitted = site.occurrenceSubName + faceElement;
            if (emitted == candidateFacePath) {
                if (faceExistsOnSite(site, faceElement)) {
                    matches.push_back(emitted);
                }
                continue;
            }
            if (emitted.size() > candidateFacePath.size()
                && emitted.compare(
                       emitted.size() - candidateFacePath.size(),
                       std::string::npos,
                       candidateFacePath
                   )
                    == 0
                && emitted[emitted.size() - candidateFacePath.size() - 1] == '.') {
                if (faceExistsOnSite(site, faceElement)) {
                    matches.push_back(emitted);
                }
            }
        }
        std::sort(matches.begin(), matches.end());
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
        return matches;
    }
};

App::DocumentObject* findNamedChildInContainer(
    App::DocumentObject* container,
    const std::string& name
)
{
    if (!container || name.empty()) {
        return nullptr;
    }
    for (auto* child : childrenOf(container)) {
        if (isOwnedBySiblingStructuralGroup(child, container)) {
            continue;
        }
        if (child && child->getNameInDocument() && name == child->getNameInDocument()) {
            if (isFilteredNonCollectableLeaf(child)) {
                return nullptr;
            }
            return child;
        }
    }
    return nullptr;
}

App::DocumentObject* findOccurrenceChildByName(
    App::DocumentObject* parent,
    const std::string& name
)
{
    if (!parent || name.empty()) {
        return nullptr;
    }
    if (auto* link = freecad_cast<App::Link*>(parent)) {
        if (link->ElementCount.getValue() > 0) {
            return nullptr;
        }
        if (auto* linked = link->getLinkedObject(true)) {
            if (isContainerToDescend(linked) && !freecad_cast<Part::BodyBase*>(linked)) {
                return findNamedChildInContainer(linked, name);
            }
        }
    }
    return findNamedChildInContainer(parent, name);
}

enum class OccurrenceStepOutcome
{
    Descended,
    CollapsedArrayIndex,
    Failed
};

/**
 * One strict occurrence-path step aligned with collectRecursively() descent.
 * Does not call DocumentObject::resolve() or other generic parsers on tokens.
 */
OccurrenceStepOutcome stepInterferenceOccurrencePath(
    App::DocumentObject* current,
    const std::string& token,
    App::DocumentObject*& outChild,
    int& arrayIndexAtEnd,
    std::string& diagnostic
)
{
    outChild = nullptr;
    if (!current || token.empty()) {
        diagnostic = "Unresolved occurrence component";
        return OccurrenceStepOutcome::Failed;
    }

    if (auto* link = freecad_cast<App::Link*>(current)) {
        const int elementCount = link->ElementCount.getValue();
        if (elementCount > 0) {
            const bool expanded =
                link->ShowElement.getValue() && !link->ElementList.getValues().empty();
            if (expanded) {
                App::DocumentObject* element = findExpandedLinkElement(link, token);
                if (!element) {
                    int parsedIndex = -1;
                    if (tryParseStrictArrayIndex(token, parsedIndex)) {
                        if (parsedIndex >= 0 && parsedIndex < link->ElementList.getSize()) {
                            element = link->ElementList.getValues()[parsedIndex];
                        }
                        else {
                            diagnostic = "Array index out of range: " + token;
                            return OccurrenceStepOutcome::Failed;
                        }
                    }
                }
                if (!element || isFilteredNonCollectableLeaf(element)) {
                    diagnostic = "Unknown link array element: " + token;
                    return OccurrenceStepOutcome::Failed;
                }
                outChild = element;
                return OccurrenceStepOutcome::Descended;
            }

            int parsedIndex = -1;
            if (tryParseStrictArrayIndex(token, parsedIndex)) {
                if (parsedIndex < 0 || parsedIndex >= elementCount) {
                    diagnostic = "Array index out of range: " + token;
                    return OccurrenceStepOutcome::Failed;
                }
                arrayIndexAtEnd = parsedIndex;
                return OccurrenceStepOutcome::CollapsedArrayIndex;
            }

            int matchIndex = -1;
            for (int i = 0; i < link->ElementList.getSize(); ++i) {
                auto* element = link->ElementList.getValues()[i];
                if (element && element->getNameInDocument() && token == element->getNameInDocument()) {
                    if (matchIndex >= 0) {
                        diagnostic = "Ambiguous link array element alias: " + token;
                        return OccurrenceStepOutcome::Failed;
                    }
                    matchIndex = i;
                }
            }
            if (matchIndex < 0) {
                diagnostic = "Unknown collapsed link array element: " + token;
                return OccurrenceStepOutcome::Failed;
            }
            arrayIndexAtEnd = matchIndex;
            return OccurrenceStepOutcome::CollapsedArrayIndex;
        }

        if (auto* linked = link->getLinkedObject(true)) {
            if (isContainerToDescend(linked) && !freecad_cast<Part::BodyBase*>(linked)) {
                outChild = findNamedChildInContainer(linked, token);
                if (outChild) {
                    return OccurrenceStepOutcome::Descended;
                }
            }
        }
    }

    if (current->isLink() && !freecad_cast<App::Link*>(current)) {
        if (auto* linked = current->getLinkedObject(true)) {
            if (isContainerToDescend(linked) && !freecad_cast<Part::BodyBase*>(linked)) {
                outChild = findNamedChildInContainer(linked, token);
                if (outChild) {
                    return OccurrenceStepOutcome::Descended;
                }
            }
        }
        diagnostic = "Extra path after occurrence leaf: " + token;
        return OccurrenceStepOutcome::Failed;
    }

    if (isPlainOrganizerGroup(current) || isContainerToDescend(current)) {
        outChild = findOccurrenceChildByName(current, token);
        if (outChild) {
            return OccurrenceStepOutcome::Descended;
        }
        diagnostic = "Unresolved occurrence component: " + token;
        return OccurrenceStepOutcome::Failed;
    }

    diagnostic = "Extra path after occurrence leaf: " + token;
    return OccurrenceStepOutcome::Failed;
}

bool descendCanonicalOccurrencePath(
    App::DocumentObject* host,
    std::vector<App::DocumentObject*>& pathFromRoot,
    const std::vector<std::string>& tokens,
    std::size_t& tokenIndex,
    int& arrayIndexAtEnd,
    std::string& diagnostic
)
{
    (void)host;
    while (tokenIndex < tokens.size()) {
        App::DocumentObject* current = pathFromRoot.back();
        const std::string& tok = tokens[tokenIndex];
        App::DocumentObject* child = nullptr;
        const OccurrenceStepOutcome outcome =
            stepInterferenceOccurrencePath(current, tok, child, arrayIndexAtEnd, diagnostic);
        if (outcome == OccurrenceStepOutcome::Failed) {
            return false;
        }
        if (outcome == OccurrenceStepOutcome::CollapsedArrayIndex) {
            ++tokenIndex;
            if (tokenIndex < tokens.size()) {
                diagnostic = "Extra path after collapsed array index: " + tokens[tokenIndex];
                return false;
            }
            return true;
        }
        pathFromRoot.push_back(child);
        ++tokenIndex;
    }
    return true;
}

std::string buildCanonicalClearanceFacePath(
    const std::vector<App::DocumentObject*>& pathFromRoot,
    int arrayIndexAtEnd,
    const std::string& faceElement
)
{
    std::ostringstream ss;
    for (auto* obj : pathFromRoot) {
        if (obj && obj->getNameInDocument()) {
            ss << obj->getNameInDocument() << '.';
        }
    }
    if (arrayIndexAtEnd >= 0) {
        ss << arrayIndexAtEnd << '.';
    }
    ss << faceElement;
    return ss.str();
}

bool canonicalizeInterferenceClearanceFacePath(
    App::DocumentObject* host,
    const std::string& normalizedFacePath,
    const InterferenceClearanceHostValidator* validator,
    std::string& outCanonical,
    std::string& diagnostic
)
{
    outCanonical.clear();
    if (!host) {
        outCanonical = normalizedFacePath;
        return true;
    }

    auto parts = Base::Tools::splitSubName(normalizedFacePath);
    while (!parts.empty() && parts.back().empty()) {
        parts.pop_back();
    }
    if (parts.empty()) {
        diagnostic = "Malformed face path";
        return false;
    }
    const std::string faceElement = parts.back();
    parts.pop_back();

    std::vector<std::string> tokens;
    tokens.reserve(parts.size());
    for (const auto& part : parts) {
        if (!part.empty()) {
            tokens.push_back(part);
        }
    }

    if (tokens.empty()) {
        outCanonical = faceElement;
        if (validator && !validator->emittedFacePathExists(outCanonical)) {
            diagnostic = "Canonical face path does not exist under host: " + outCanonical;
            return false;
        }
        return true;
    }

    if (validator && validator->emittedFacePathExists(normalizedFacePath)) {
        outCanonical = normalizedFacePath;
        return true;
    }

    std::string strictCanonical;
    std::string strictDiag;
    bool strictOk = true;
    App::DocumentObject* firstChild = findOccurrenceChildByName(host, tokens[0]);
    if (!firstChild) {
        strictOk = false;
        strictDiag = "Unresolved occurrence component: " + tokens[0];
    }
    else {
        std::vector<App::DocumentObject*> pathFromRoot {firstChild};
        int arrayIndexAtEnd = -1;
        std::size_t tokenIndex = 1;
        if (!descendCanonicalOccurrencePath(
                host,
                pathFromRoot,
                tokens,
                tokenIndex,
                arrayIndexAtEnd,
                strictDiag
            )) {
            strictOk = false;
        }
        else if (tokenIndex < tokens.size()) {
            strictOk = false;
            strictDiag = "Unresolved occurrence path suffix";
        }
        else {
            strictCanonical =
                buildCanonicalClearanceFacePath(pathFromRoot, arrayIndexAtEnd, faceElement);
        }
    }

    if (strictOk && validator && validator->emittedFacePathExists(strictCanonical)) {
        outCanonical = strictCanonical;
        return true;
    }

    if (validator) {
        const auto suffixMatches =
            validator->findSuffixAliasFacePathMatches(normalizedFacePath);
        if (suffixMatches.size() == 1) {
            outCanonical = suffixMatches.front();
            return true;
        }
        if (suffixMatches.size() > 1) {
            diagnostic = "Ambiguous occurrence face path: " + normalizedFacePath;
            return false;
        }
    }

    if (strictOk) {
        if (validator) {
            diagnostic = "Canonical face path does not exist under host: " + strictCanonical;
            return false;
        }
        outCanonical = strictCanonical;
        return true;
    }

    diagnostic = strictDiag.empty() ? "Unresolved face path" : strictDiag;
    return false;
}

bool validateClearanceFacePath(
    const App::DocumentObject* host,
    std::string& facePath,
    bool allowStar,
    std::string& diagnostic,
    const InterferenceClearanceHostValidator* validator
)
{
    if (facePath.empty()) {
        diagnostic = "Empty Face path";
        return false;
    }
    if (facePath == "*") {
        if (!allowStar) {
            diagnostic = "Wildcard * is not allowed here";
            return false;
        }
        return true;
    }
    if (facePath.find('*') != std::string::npos) {
        diagnostic = "Unsupported * combination in face path";
        return false;
    }

    std::string normalized = canonicalFaceKey(facePath);
    if (normalized.empty()) {
        diagnostic = "Unresolved face path";
        return false;
    }
    // Reject truncated / mapped TNP leftovers.
    if (normalized.find(Data::MISSING_PREFIX) != std::string::npos) {
        diagnostic = "Malformed or truncated TNP face path";
        return false;
    }

    auto parts = Base::Tools::splitSubName(normalized);
    if (parts.empty()) {
        diagnostic = "Malformed face path";
        return false;
    }
    while (!parts.empty() && parts.back().empty()) {
        parts.pop_back();
    }
    if (parts.empty()) {
        diagnostic = "Malformed face path";
        return false;
    }
    const std::string& last = parts.back();
    if (!isValidFaceElementToken(last)) {
        diagnostic = "Final element must be a valid FaceN (N >= 1)";
        return false;
    }

    if (host) {
        auto* mutableHost = const_cast<App::DocumentObject*>(host);
        std::string canonical;
        if (!canonicalizeInterferenceClearanceFacePath(
                mutableHost,
                normalized,
                validator,
                canonical,
                diagnostic
            )) {
            return false;
        }
        normalized = std::move(canonical);
    }
    facePath = std::move(normalized);
    return true;
}

}  // namespace

InterferenceClearanceRuleTable parseInterferenceClearanceSheet(
    const App::DocumentObject* sheetOrNull,
    const App::DocumentObject* hostOrNull,
    InterferenceClearanceSheetParseStats* parseStats,
    const std::vector<InterferenceLeaf>* preparedLeaves
)
{
    InterferenceClearanceRuleTable table;
    auto* sheet = freecad_cast<const Spreadsheet::Sheet*>(sheetOrNull);
    if (!sheet) {
        return table;
    }

    int maxRow = -1;
    int maxCol = -1;
    for (const auto& used : sheet->getUsedCells()) {
        try {
            App::CellAddress addr(used.c_str());
            maxRow = std::max(maxRow, addr.row());
            maxCol = std::max(maxCol, addr.col());
        }
        catch (...) {
        }
    }
    if (maxRow < 0 || maxCol < 0) {
        return table;
    }

    int colEnabled = -1;
    int colFace = -1;
    int colFaceB = -1;
    int colTolerance = -1;
    int colComment = -1;
    int enabledHeaderCount = 0;
    int faceHeaderCount = 0;
    int faceBHeaderCount = 0;
    int toleranceHeaderCount = 0;
    int commentHeaderCount = 0;
    for (int col = 0; col <= maxCol; ++col) {
        std::string header = sheetCellText(sheet, 0, col);
        for (char& c : header) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (header == "enabled") {
            colEnabled = col;
            ++enabledHeaderCount;
        }
        else if (header == "face" || header == "facea") {
            colFace = col;
            ++faceHeaderCount;
        }
        else if (header == "faceb") {
            colFaceB = col;
            ++faceBHeaderCount;
        }
        else if (header == "tolerance" || header == "clearance") {
            colTolerance = col;
            ++toleranceHeaderCount;
        }
        else if (header == "comment") {
            colComment = col;
            ++commentHeaderCount;
        }
    }
    if (colFace < 0 || colTolerance < 0) {
        table.diagnostics.push_back(
            "Clearance spreadsheet requires Face and Tolerance header columns"
        );
        table.invalidRuleCount += 1;
        return table;
    }
    if (enabledHeaderCount > 1 || faceHeaderCount > 1 || faceBHeaderCount > 1
        || toleranceHeaderCount > 1 || commentHeaderCount > 1) {
        table.diagnostics.push_back("Duplicate clearance spreadsheet headers");
        table.invalidRuleCount += 1;
        return table;
    }

    const bool hostCanValidate =
        hostOrNull && isInterferenceRoot(hostOrNull);
    std::vector<std::size_t> pendingHostFaceRuleIndices;

    for (int row = 1; row <= maxRow; ++row) {
        const std::string faceRaw = sheetCellText(sheet, row, colFace);
        const std::string faceBRaw =
            colFaceB >= 0 ? sheetCellText(sheet, row, colFaceB) : std::string();
        const std::string tolRaw =
            colTolerance >= 0 ? sheetCellText(sheet, row, colTolerance) : std::string();
        const std::string enabledRaw =
            colEnabled >= 0 ? sheetCellText(sheet, row, colEnabled) : std::string();
        const std::string commentRaw =
            colComment >= 0 ? sheetCellText(sheet, row, colComment) : std::string();

        const bool rowHasData = !faceRaw.empty() || !faceBRaw.empty() || !tolRaw.empty()
            || !enabledRaw.empty() || !commentRaw.empty();
        if (!rowHasData) {
            continue;
        }

        InterferenceClearanceRule rule;
        rule.spreadsheetRow = row + 1;  // 1-based sheet row including header
        rule.comment = commentRaw;

        if (colEnabled >= 0) {
            if (!parseEnabledFlag(enabledRaw, rule.enabled)) {
                rule.valid = false;
                rule.diagnostic =
                    "Invalid Enabled value in row " + std::to_string(rule.spreadsheetRow);
            }
        }
        // Disabled rows are ignored entirely.
        if (!rule.enabled) {
            continue;
        }

        std::string tolFail;
        if (!sheetCellNumber(sheet, row, colTolerance, rule.clearanceMm, &tolFail)
            || rule.clearanceMm < 0.0) {
            rule.valid = false;
            if (rule.diagnostic.empty()) {
                rule.diagnostic = (tolFail.empty() ? "Invalid Tolerance" : tolFail) + " in row "
                    + std::to_string(rule.spreadsheetRow);
            }
        }

        if (faceRaw.empty()) {
            rule.valid = false;
            if (rule.diagnostic.empty()) {
                rule.diagnostic =
                    "Empty Face path in row " + std::to_string(rule.spreadsheetRow)
                    + " with other populated columns";
            }
        }
        else {
            rule.faceA = canonicalFaceKey(faceRaw);
            std::string faceDiag;
            if (!validateClearanceFacePath(
                    nullptr,
                    rule.faceA,
                    /*allowStar=*/true,
                    faceDiag,
                    nullptr
                )) {
                rule.valid = false;
                if (rule.diagnostic.empty()) {
                    rule.diagnostic = faceDiag + " in row " + std::to_string(rule.spreadsheetRow);
                }
            }
        }

        if (!faceBRaw.empty()) {
            rule.faceB = canonicalFaceKey(faceBRaw);
            if (rule.faceA == "*") {
                rule.valid = false;
                if (rule.diagnostic.empty()) {
                    rule.diagnostic =
                        "Wildcard * cannot be combined with FaceB in row "
                        + std::to_string(rule.spreadsheetRow);
                }
            }
            std::string faceBDiag;
            if (!validateClearanceFacePath(
                    nullptr,
                    rule.faceB,
                    /*allowStar=*/false,
                    faceBDiag,
                    nullptr
                )) {
                rule.valid = false;
                if (rule.diagnostic.empty()) {
                    rule.diagnostic =
                        "Invalid FaceB: " + faceBDiag + " in row "
                        + std::to_string(rule.spreadsheetRow);
                }
            }
        }

        const bool needsHostFaceValidation = hostCanValidate && rule.valid
            && !(rule.faceA == "*" && rule.faceB.empty());
        table.rules.push_back(rule);
        if (!rule.valid) {
            table.invalidRuleCount += 1;
            continue;
        }
        if (needsHostFaceValidation) {
            pendingHostFaceRuleIndices.push_back(table.rules.size() - 1);
        }
    }

    if (!pendingHostFaceRuleIndices.empty()) {
        InterferenceClearanceHostValidator validator(hostOrNull, preparedLeaves, parseStats);
        for (const std::size_t ruleIndex : pendingHostFaceRuleIndices) {
            auto& rule = table.rules[ruleIndex];
            if (!rule.valid) {
                continue;
            }
            bool hostOk = true;
            std::string hostDiag;
            if (rule.faceA != "*") {
                if (!validateClearanceFacePath(
                        hostOrNull,
                        rule.faceA,
                        /*allowStar=*/true,
                        hostDiag,
                        &validator
                    )) {
                    hostOk = false;
                }
            }
            if (hostOk && !rule.faceB.empty()) {
                std::string faceBDiag;
                if (!validateClearanceFacePath(
                        hostOrNull,
                        rule.faceB,
                        /*allowStar=*/false,
                        faceBDiag,
                        &validator
                    )) {
                    hostOk = false;
                    hostDiag = faceBDiag;
                }
            }
            if (!hostOk) {
                rule.valid = false;
                if (rule.diagnostic.empty()) {
                    rule.diagnostic =
                        hostDiag + " in row " + std::to_string(rule.spreadsheetRow);
                }
                ++table.invalidRuleCount;
            }
        }
    }

    for (const auto& rule : table.rules) {
        if (!rule.enabled || !rule.valid) {
            continue;
        }
        table.maxEnabledClearance = std::max(table.maxEnabledClearance, rule.clearanceMm);
        if (rule.faceA == "*" && rule.faceB.empty()) {
            table.hasDefaultStar = true;
            table.defaultStarClearance = rule.clearanceMm;
        }
    }
    return table;
}

InterferenceClearanceRuleTable snapshotInterferenceClearanceRules(
    const App::DocumentObject* host,
    const std::vector<InterferenceLeaf>* preparedLeaves
)
{
    return parseInterferenceClearanceSheet(
        getInterferenceClearanceSheet(host),
        host,
        nullptr,
        preparedLeaves
    );
}

InterferenceClearanceLookup lookupInterferenceClearance(
    const InterferenceClearanceRuleTable& table,
    const std::string& facePathA,
    const std::string& facePathB,
    double assemblyClearanceMm
)
{
    InterferenceClearanceLookup lookup;
    lookup.clearanceMm = assemblyClearanceMm;
    lookup.kind = InterferenceClearanceRuleKind::AssemblyGlobal;

    const std::string a = canonicalFaceKey(facePathA);
    const std::string b = canonicalFaceKey(facePathB);

    // Collect all matching rules at each precedence; apply strictest; keep all rows.
    std::vector<const InterferenceClearanceRule*> exactMatches;
    std::vector<const InterferenceClearanceRule*> individualA;
    std::vector<const InterferenceClearanceRule*> individualB;
    std::vector<const InterferenceClearanceRule*> starMatches;

    for (const auto& rule : table.rules) {
        if (!rule.enabled || !rule.valid) {
            continue;
        }
        if (!rule.faceB.empty()) {
            if ((rule.faceA == a && rule.faceB == b) || (rule.faceA == b && rule.faceB == a)) {
                exactMatches.push_back(&rule);
            }
            continue;
        }
        if (rule.faceA == "*") {
            starMatches.push_back(&rule);
            continue;
        }
        if (rule.faceA == a) {
            individualA.push_back(&rule);
        }
        if (rule.faceA == b) {
            individualB.push_back(&rule);
        }
    }

    auto appendRule = [](InterferenceClearanceLookup& out, const InterferenceClearanceRule& rule) {
        if (rule.spreadsheetRow > 0) {
            out.sourceRows.push_back(rule.spreadsheetRow);
            // Always retain the parallel comment (including empty) for provenance.
            out.sourceComments.push_back(rule.comment);
        }
    };

    auto pickStrictest = [](const std::vector<const InterferenceClearanceRule*>& rules)
        -> const InterferenceClearanceRule* {
        const InterferenceClearanceRule* best = nullptr;
        for (const auto* rule : rules) {
            if (!best || rule->clearanceMm > best->clearanceMm
                || (rule->clearanceMm == best->clearanceMm
                    && rule->spreadsheetRow < best->spreadsheetRow)) {
                best = rule;
            }
        }
        return best;
    };

    if (!exactMatches.empty()) {
        const auto* best = pickStrictest(exactMatches);
        lookup.clearanceMm = best->clearanceMm;
        lookup.kind = InterferenceClearanceRuleKind::ExactPair;
        for (const auto* rule : exactMatches) {
            // Preserve every contributing exact-pair row at the chosen (strictest) clearance.
            if (rule->clearanceMm + 1e-15 >= best->clearanceMm) {
                appendRule(lookup, *rule);
            }
        }
        if (exactMatches.size() > lookup.sourceRows.size()) {
            lookup.diagnostic = "Duplicate exact-pair rules; applied strictest clearance";
        }
        return lookup;
    }

    if (!individualA.empty() || !individualB.empty()) {
        const auto* bestA = pickStrictest(individualA);
        const auto* bestB = pickStrictest(individualB);
        const double ca = bestA ? bestA->clearanceMm : -1.0;
        const double cb = bestB ? bestB->clearanceMm : -1.0;
        lookup.clearanceMm = std::max(ca, cb);
        lookup.kind = InterferenceClearanceRuleKind::MaxIndividual;
        // Preserve every contributing individual rule at the chosen clearance,
        // and when both faces contribute, include both faces' strictest rows.
        auto appendFaceRules =
            [&](const std::vector<const InterferenceClearanceRule*>& rules, double faceClearance) {
                for (const auto* rule : rules) {
                    if (std::abs(rule->clearanceMm - faceClearance) <= 1e-15) {
                        appendRule(lookup, *rule);
                    }
                }
            };
        if (bestA) {
            appendFaceRules(individualA, bestA->clearanceMm);
        }
        if (bestB) {
            appendFaceRules(individualB, bestB->clearanceMm);
        }
        return lookup;
    }

    if (!starMatches.empty() || table.hasDefaultStar) {
        const auto* best = pickStrictest(starMatches);
        lookup.clearanceMm = best ? best->clearanceMm : table.defaultStarClearance;
        lookup.kind = InterferenceClearanceRuleKind::DefaultStar;
        if (best) {
            for (const auto* rule : starMatches) {
                if (std::abs(rule->clearanceMm - best->clearanceMm) <= 1e-15) {
                    appendRule(lookup, *rule);
                }
            }
        }
        return lookup;
    }
    return lookup;
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
    prop->appendPair(canon.first, canon.second);
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
