// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 The FreeCAD Project Association AISBL              *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include <exception>

#include <App/Application.h>
#include <App/Document.h>
#include <App/FeaturePythonPyImp.h>
#include <App/GeoFeature.h>
#include <App/GroupExtension.h>
#include <App/Link.h>
#include <App/Part.h>
#include <App/PropertyGeo.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Placement.h>
#include <Base/Tools.h>
#include <fastsignals/signal.h>

#include <Mod/Part/App/PartFeature.h>
#include <TopoDS_Shape.hxx>

#include "AssemblyObject.h"
#include "AssemblyUtils.h"
#include "Groups.h"
#include "ReviewNote.h"
#include "ReviewNotePy.h"


using namespace Assembly;

const char* ReviewNote::JointSideEnums[] = {"None", "Reference1", "Reference2", nullptr};

namespace
{

bool isPlacementLikeProperty(const App::Property& prop)
{
    const char* name = prop.getName();
    if (!name) {
        return false;
    }
    return strcmp(name, "Placement") == 0 || strcmp(name, "LinkPlacement") == 0
        || strcmp(name, "Placement1") == 0 || strcmp(name, "Placement2") == 0
        || strcmp(name, "Reference1") == 0 || strcmp(name, "Reference2") == 0
        || strcmp(name, "ObjectToGround") == 0 || strcmp(name, "Group") == 0
        || strcmp(name, "Shape") == 0;
}

App::DocumentObject* shapeOwnerFor(App::DocumentObject* obj)
{
    if (!obj) {
        return nullptr;
    }
    if (auto* linked = obj->getLinkedObject(/*recursive=*/true)) {
        return linked;
    }
    return obj;
}

bool isGeometryElementName(const std::string& name)
{
    static const char* prefixes[] = {"Face", "Edge", "Vertex"};
    for (const char* prefix : prefixes) {
        const size_t len = std::strlen(prefix);
        if (name.size() <= len || name.compare(0, len, prefix) != 0) {
            continue;
        }
        return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(len), name.end(), ::isdigit);
    }
    return false;
}

bool geometrySubExists(App::DocumentObject* obj, const std::string& sub)
{
    if (!obj) {
        return false;
    }
    if (sub.empty() || sub == "Main") {
        return true;
    }

    // Occurrence-relative paths under Links/Parts (e.g. Spool_GearTipRelief.Face6).
    // getSubObject() can return a non-null whole shape for missing FaceN, so resolve the
    // path to a shape owner + element name and validate the element on that shape.
    if (sub.find('.') != std::string::npos) {
        const char* element = nullptr;
        std::string childName;
        App::DocumentObject* parent = nullptr;
        App::DocumentObject* resolved =
            obj->resolve(sub.c_str(), &parent, &childName, &element);
        if (!resolved) {
            return false;
        }
        const auto dot = sub.find_last_of('.');
        const std::string last = sub.substr(dot + 1);
        if (isGeometryElementName(last)) {
            if (!element || last != element) {
                return false;
            }
            return geometrySubExists(resolved, last);
        }
        // Path ends at a nested object (optional trailing '.').
        return resolved != obj || !childName.empty();
    }

    App::DocumentObject* owner = shapeOwnerFor(obj);
    if (auto* feat = freecad_cast<Part::Feature*>(owner)) {
        const Part::TopoShape& shape = feat->Shape.getShape();
        if (shape.isNull()) {
            return false;
        }
        const TopoDS_Shape subShape = shape.getSubShape(sub.c_str(), /*silent=*/true);
        return !subShape.IsNull();
    }

    try {
        return obj->getSubObject(sub.c_str()) != nullptr;
    }
    catch (...) {
        return false;
    }
}

bool objectInOwner(App::Part* owner, App::DocumentObject* obj)
{
    if (!owner || !obj) {
        return false;
    }
    if (obj == owner) {
        return true;
    }
    if (owner->hasObject(obj, true)) {
        return true;
    }
    // hasObject does not detect LinkElements (FreeCAD#16113).
    if (auto* linkEl = freecad_cast<App::LinkElement*>(obj)) {
        if (auto* linkGroup = linkEl->getLinkGroup()) {
            return owner->hasObject(linkGroup, true);
        }
    }
    return false;
}

bool groupContainsObject(const App::DocumentObject& container, const App::DocumentObject* obj)
{
    if (!obj) {
        return false;
    }
    auto* group = container.getExtensionByType<App::GroupExtension>(true);
    return group && group->hasObject(obj, true);
}

App::Link* linkGroupOf(App::DocumentObject* obj)
{
    if (auto* linkEl = freecad_cast<App::LinkElement*>(obj)) {
        return linkEl->getLinkGroup();
    }
    return nullptr;
}

bool noteDependsOnObject(const ReviewNote& note, const App::DocumentObject& obj)
{
    auto* target = note.Target.getValue();
    if (!target) {
        return false;
    }
    if (target == &obj) {
        return true;
    }
    if (shapeOwnerFor(target) == &obj) {
        return true;
    }
    // Nested/container placements between the owning assembly and the target.
    if (groupContainsObject(obj, target) || groupContainsObject(obj, shapeOwnerFor(target))) {
        return true;
    }
    // Link-array parent: LinkElements are not GroupExtension children (FreeCAD#16113).
    if (auto* linkGroup = linkGroupOf(target)) {
        if (linkGroup == &obj) {
            return true;
        }
    }
    if (auto* linkGroup = linkGroupOf(shapeOwnerFor(target))) {
        if (linkGroup == &obj) {
            return true;
        }
    }

    if (note.JointSide.getValue() != static_cast<long>(ReviewNoteJointSide::None)) {
        const char* refName = note.JointSide.getValue()
                == static_cast<long>(ReviewNoteJointSide::Reference1)
            ? "Reference1"
            : "Reference2";
        auto* ref = dynamic_cast<App::PropertyXLinkSub*>(target->getPropertyByName(refName));
        if (ref) {
            if (ref->getValue() == &obj) {
                return true;
            }
            if (auto* moving = getMovingPartFromRef(ref)) {
                if (moving == &obj) {
                    return true;
                }
            }
            // Intermediate containers that move a joint reference/moving part.
            if (groupContainsObject(obj, ref->getValue())
                || groupContainsObject(obj, getMovingPartFromRef(ref))) {
                return true;
            }
            if (auto* linkGroup = linkGroupOf(ref->getValue())) {
                if (linkGroup == &obj) {
                    return true;
                }
            }
            if (auto* linkGroup = linkGroupOf(getMovingPartFromRef(ref))) {
                if (linkGroup == &obj) {
                    return true;
                }
            }
        }
    }

    return false;
}

Base::Placement placementOfObjectInOwner(App::Part* owner, App::DocumentObject* obj)
{
    if (!owner || !obj) {
        return {};
    }

    auto parents = obj->getParents();
    for (const auto& parent : parents) {
        if (parent.first == owner) {
            Base::Placement full = owner->getPlacementOf(parent.second, obj);
            if (auto* ownerPlc = owner->getPlacementProperty()) {
                return ownerPlc->getValue().inverse() * full;
            }
            return full;
        }
    }

    if (owner->hasObject(obj, false)) {
        if (auto* plcProp = obj->getPlacementProperty()) {
            return plcProp->getValue();
        }
    }

    if (auto* plcProp = obj->getPlacementProperty()) {
        return plcProp->getValue();
    }
    return {};
}

/// One observer per owning App::Part / AssemblyObject.
class ReviewNoteOwnerTracker
{
public:
    explicit ReviewNoteOwnerTracker(App::Part* owner)
        : owner(owner)
    {
        connChanged = App::GetApplication().signalChangedObject.connect(
            [this](const App::DocumentObject& obj, const App::Property& prop) {
                onChangedObject(obj, prop);
            }
        );
        if (App::Document* doc = owner->getDocument()) {
            connRecomputed = doc->signalRecomputedObject.connect(
                [this](const App::DocumentObject& obj) {
                    onRecomputedObject(obj);
                }
            );
        }
    }

    App::Part* getOwner() const
    {
        return owner;
    }

    App::Document* getDocument() const
    {
        return owner ? owner->getDocument() : nullptr;
    }

    void onChangedObject(const App::DocumentObject& Obj, const App::Property& Prop)
    {
        if (!owner || !owner->isAttachedToDocument()) {
            return;
        }
        App::Document* doc = owner->getDocument();
        if (!doc || Obj.getDocument() != doc) {
            return;
        }
        if (Obj.isDerivedFrom(ReviewNote::getClassTypeId())) {
            return;
        }
        if (!isPlacementLikeProperty(Prop)) {
            return;
        }
        if (!Obj.isAttachedToDocument()) {
            return;
        }
        if (&Obj != owner && !objectInOwner(owner, const_cast<App::DocumentObject*>(&Obj))) {
            return;
        }

        const char* propName = Prop.getName();
        const bool isGroup = propName && strcmp(propName, "Group") == 0;

        for (auto* obj : doc->getObjectsOfType(ReviewNote::getClassTypeId())) {
            auto* note = freecad_cast<ReviewNote*>(obj);
            if (!note || !note->isAttachedToDocument() || note->getOwnerPart() != owner) {
                continue;
            }
            if (noteDependsOnObject(*note, Obj)
                || (isGroup && (&Obj == owner || &Obj == note->getGroup()))) {
                note->refreshBasePosition();
            }
        }
    }

    void onRecomputedObject(const App::DocumentObject& Obj)
    {
        if (!owner || !owner->isAttachedToDocument()) {
            return;
        }
        App::Document* doc = owner->getDocument();
        if (!doc || Obj.getDocument() != doc) {
            return;
        }
        if (Obj.isDerivedFrom(ReviewNote::getClassTypeId())) {
            return;
        }
        if (!Obj.isAttachedToDocument()) {
            return;
        }
        if (&Obj != owner && !objectInOwner(owner, const_cast<App::DocumentObject*>(&Obj))) {
            return;
        }

        for (auto* obj : doc->getObjectsOfType(ReviewNote::getClassTypeId())) {
            auto* note = freecad_cast<ReviewNote*>(obj);
            if (!note || !note->isAttachedToDocument() || note->getOwnerPart() != owner) {
                continue;
            }
            if (noteDependsOnObject(*note, Obj)) {
                note->refreshBasePosition();
            }
        }
    }

private:
    App::Part* owner = nullptr;
    fastsignals::scoped_connection connChanged;
    fastsignals::scoped_connection connRecomputed;
};

std::map<App::Part*, std::unique_ptr<ReviewNoteOwnerTracker>> g_ownerTrackers;
fastsignals::scoped_connection g_docDeleteHook;
fastsignals::scoped_connection g_deletedObjectHook;
fastsignals::scoped_connection g_undoHook;
fastsignals::scoped_connection g_redoHook;
bool g_lifecycleHooksInstalled = false;

void revokeTrackersForDocument(const App::Document& doc)
{
    for (auto it = g_ownerTrackers.begin(); it != g_ownerTrackers.end();) {
        App::Document* trackerDoc = it->second ? it->second->getDocument() : nullptr;
        if (!it->second || !it->second->getOwner() || trackerDoc == &doc) {
            it = g_ownerTrackers.erase(it);
        }
        else {
            ++it;
        }
    }
}

void reinstallTrackersForDocument(const App::Document& doc)
{
    for (auto* obj : doc.getObjectsOfType(ReviewNote::getClassTypeId())) {
        auto* note = freecad_cast<ReviewNote*>(obj);
        if (!note || !note->isAttachedToDocument()) {
            continue;
        }
        ReviewNote::ensureOwnerObserver(note->getOwnerPart());
    }
}

void ensureLifecycleHooks()
{
    if (g_lifecycleHooksInstalled) {
        return;
    }
    g_docDeleteHook = App::GetApplication().signalDeleteDocument.connect(
        [](const App::Document& doc) {
            revokeTrackersForDocument(doc);
        }
    );
    // unsetupObject is skipped while undoing/rolling back; revoke on delete instead.
    g_deletedObjectHook = App::GetApplication().signalDeletedObject.connect(
        [](const App::DocumentObject& obj) {
            if (obj.isDerivedFrom(App::Part::getClassTypeId())) {
                g_ownerTrackers.erase(
                    static_cast<App::Part*>(const_cast<App::DocumentObject*>(&obj))
                );
            }
        }
    );
    g_undoHook = App::GetApplication().signalUndoDocument.connect(
        [](const App::Document& doc) {
            reinstallTrackersForDocument(doc);
        }
    );
    g_redoHook = App::GetApplication().signalRedoDocument.connect(
        [](const App::Document& doc) {
            reinstallTrackersForDocument(doc);
        }
    );
    g_lifecycleHooksInstalled = true;
}

}  // namespace


PROPERTY_SOURCE(Assembly::ReviewNote, App::AnnotationLabel)

ReviewNote::ReviewNote()
{
    ADD_PROPERTY_TYPE(
        Target,
        (nullptr),
        "ReviewNote",
        App::Prop_None,
        "Component-rooted geometry target or linked joint"
    );
    ADD_PROPERTY_TYPE(
        LocalAnchor,
        (Base::Vector3d()),
        "ReviewNote",
        App::Prop_None,
        "Picked point in target-local coordinates"
    );
    ADD_PROPERTY_TYPE(
        JointSide,
        (static_cast<long>(ReviewNoteJointSide::None)),
        "ReviewNote",
        App::Prop_None,
        "Which joint connector side this note anchors to"
    );
    JointSide.setEnums(JointSideEnums);
    ADD_PROPERTY_TYPE(
        Resolved,
        (false),
        "ReviewNote",
        App::Prop_None,
        "Whether this review note is marked resolved"
    );
    ADD_PROPERTY_TYPE(
        AttachmentBroken,
        (false),
        "ReviewNote",
        static_cast<App::PropertyType>(App::Prop_Output | App::Prop_ReadOnly | App::Prop_Hidden),
        "Computed: true when the attachment target is missing or invalid"
    );
    ADD_PROPERTY_TYPE(
        LeaderPort,
        (-1.0),
        "ReviewNote",
        App::Prop_None,
        "Legacy LeaderPort [0,1] kept for FCStd/undo; display always nearest-border auto-attach"
    );

    BasePosition.setStatus(App::Property::Output, false);
    TextPosition.setStatus(App::Property::Output, false);
    LabelText.setStatus(App::Property::Output, false);
}

ReviewNote::~ReviewNote() = default;

PyObject* ReviewNote::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        PythonObject = Py::Object(new ReviewNotePy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

void ReviewNote::ensureDocumentObserver(App::Document* doc)
{
    if (!doc) {
        return;
    }
    for (auto* obj : doc->getObjectsOfType(ReviewNote::getClassTypeId())) {
        auto* note = freecad_cast<ReviewNote*>(obj);
        if (!note) {
            continue;
        }
        ensureOwnerObserver(note->getOwnerPart());
    }
}

void ReviewNote::ensureOwnerObserver(App::Part* owner)
{
    if (!owner) {
        return;
    }
    ensureLifecycleHooks();

    auto& tracker = g_ownerTrackers[owner];
    if (!tracker || tracker->getOwner() != owner) {
        tracker = std::make_unique<ReviewNoteOwnerTracker>(owner);
    }

    for (auto it = g_ownerTrackers.begin(); it != g_ownerTrackers.end();) {
        if (!it->second || !it->second->getOwner()) {
            it = g_ownerTrackers.erase(it);
        }
        else {
            ++it;
        }
    }
}

void ReviewNote::ensureAssemblyObserver(AssemblyObject* assembly)
{
    ensureOwnerObserver(assembly);
}

void ReviewNote::revokeOwnerObserver(App::Part* owner)
{
    if (!owner) {
        return;
    }
    g_ownerTrackers.erase(owner);
}

void ReviewNote::revokeAssemblyObserver(AssemblyObject* assembly)
{
    revokeOwnerObserver(assembly);
}

App::DocumentObjectExecReturn* ReviewNote::execute()
{
    ensureOwnerObserver(getOwnerPart());
    refreshBasePosition();
    return App::DocumentObject::StdReturn;
}

void ReviewNote::onDocumentRestored()
{
    App::AnnotationLabel::onDocumentRestored();
    ensureOwnerObserver(getOwnerPart());
    refreshBasePosition();
    updateLabelFromText();
}

void ReviewNote::onChanged(const App::Property* prop)
{
    if (!prop) {
        App::AnnotationLabel::onChanged(prop);
        return;
    }

    if (!isRestoring() && getDocument() && !getDocument()->isPerformingTransaction()) {
        ensureOwnerObserver(getOwnerPart());
    }

    if (prop == &LabelText) {
        updateLabelFromText();
    }
    else if (prop == &Target || prop == &LocalAnchor || prop == &JointSide) {
        if (!isRestoring() && !refreshing) {
            refreshBasePosition();
        }
    }
    else if (prop == &TextPosition) {
        // Publish leader sync before DocumentObject observers see TextPosition so
        // no sampled frame can observe a new text box with a stale LeaderEnd.
        // Skip while restoring: the view provider refresh on attach/drawImage owns
        // the first correct frame once the label image and camera exist.
        // FastSignals does not catch slot exceptions; contain any escape so the
        // App::AnnotationLabel::onChanged path below always runs.
        if (!isRestoring()) {
            try {
                signalSyncLeaderVisual(TextPosition.getValue());
            }
            catch (const Base::Exception& e) {
                Base::Console().error(
                    "Assembly::ReviewNote: signalSyncLeaderVisual failed: %s\n",
                    e.what()
                );
            }
            catch (const std::exception& e) {
                Base::Console().error(
                    "Assembly::ReviewNote: signalSyncLeaderVisual failed: %s\n",
                    e.what()
                );
            }
            catch (...) {
                Base::Console().error(
                    "Assembly::ReviewNote: signalSyncLeaderVisual failed "
                    "(unknown exception)\n"
                );
            }
        }
    }

    App::AnnotationLabel::onChanged(prop);
}

void ReviewNote::updateLabelFromText()
{
    if (updatingLabel || isRestoring()) {
        return;
    }
    std::string line = firstTextLine();
    if (Label.getStrValue() == line) {
        return;
    }
    Base::ObjectStatusLocker<App::ObjectStatus, App::DocumentObject> guard(App::NoTouch, this);
    updatingLabel = true;
    Label.setValue(line);
    updatingLabel = false;
}

std::string ReviewNote::firstTextLine() const
{
    for (const auto& line : LabelText.getValues()) {
        if (line.find_first_not_of(" \t\r\n") != std::string::npos) {
            return line;
        }
    }
    return {};
}

bool ReviewNote::isAttachmentBroken() const
{
    auto* target = Target.getValue();
    if (!target || !target->isAttachedToDocument()) {
        return true;
    }

    auto* owner = getOwnerPart();
    if (!owner || !objectInOwner(owner, target)) {
        return true;
    }

    const auto& subs = Target.getSubValues();
    for (const auto& sub : subs) {
        if (sub.empty()) {
            continue;
        }
        if (JointSide.getValue() != static_cast<long>(ReviewNoteJointSide::None)) {
            if (sub != "Main") {
                return true;
            }
            continue;
        }
        if (!geometrySubExists(target, sub)) {
            return true;
        }
    }

    if (JointSide.getValue() != static_cast<long>(ReviewNoteJointSide::None)) {
        const char* refName = JointSide.getValue()
                == static_cast<long>(ReviewNoteJointSide::Reference1)
            ? "Reference1"
            : "Reference2";
        auto* ref = dynamic_cast<App::PropertyXLinkSub*>(target->getPropertyByName(refName));
        if (!ref || !ref->getValue() || !ref->getValue()->isAttachedToDocument()) {
            return true;
        }
        App::DocumentObject* refObj = ref->getValue();
        if (refObj->getDocument() != getDocument() || !objectInOwner(owner, refObj)) {
            return true;
        }
        if (auto* moving = getMovingPartFromRef(ref)) {
            if (moving->getDocument() != getDocument() || !objectInOwner(owner, moving)) {
                return true;
            }
        }
        for (const auto& sub : ref->getSubValues()) {
            if (!sub.empty() && !geometrySubExists(refObj, sub)) {
                return true;
            }
        }
    }

    return false;
}

void ReviewNote::updateAttachmentState()
{
    if (updatingAttachment || isRestoring()) {
        return;
    }
    const bool broken = isAttachmentBroken();
    if (AttachmentBroken.getValue() == broken) {
        return;
    }
    updatingAttachment = true;
    AttachmentBroken.setValue(broken);
    updatingAttachment = false;
}

App::Part* ReviewNote::getOwnerPart() const
{
    for (auto* obj : getInList()) {
        if (auto* group = freecad_cast<ReviewNoteGroup*>(obj)) {
            for (auto* parent : group->getInList()) {
                if (auto* part = freecad_cast<App::Part*>(parent)) {
                    return part;
                }
            }
        }
        if (auto* part = freecad_cast<App::Part*>(obj)) {
            return part;
        }
    }
    return nullptr;
}

AssemblyObject* ReviewNote::getAssembly() const
{
    return freecad_cast<AssemblyObject*>(getOwnerPart());
}

ReviewNoteGroup* ReviewNote::getGroup() const
{
    for (auto* obj : getInList()) {
        if (auto* group = freecad_cast<ReviewNoteGroup*>(obj)) {
            return group;
        }
    }
    return nullptr;
}

void ReviewNote::refreshBasePosition()
{
    if (refreshing || isRestoring()) {
        return;
    }

    auto* owner = getOwnerPart();
    ensureOwnerObserver(owner);
    updateAttachmentState();

    if (isAttachmentBroken()) {
        // Keep last valid BasePosition when attachment is broken.
        return;
    }

    auto* target = Target.getValue();
    Base::Placement targetPlc;

    if (JointSide.getValue() != static_cast<long>(ReviewNoteJointSide::None)) {
        const bool useRef1 =
            JointSide.getValue() == static_cast<long>(ReviewNoteJointSide::Reference1);
        const char* plcName = useRef1 ? "Placement1" : "Placement2";
        const char* refName = useRef1 ? "Reference1" : "Reference2";

        auto* jcsPlcProp =
            dynamic_cast<App::PropertyPlacement*>(target->getPropertyByName(plcName));
        auto* refProp = dynamic_cast<App::PropertyXLinkSub*>(target->getPropertyByName(refName));
        if (!jcsPlcProp || !refProp || !refProp->getValue()) {
            return;
        }

        App::DocumentObject* movingPart = getMovingPartFromRef(refProp);
        if (!movingPart) {
            movingPart = refProp->getValue();
        }

        Base::Placement partPlc = placementOfObjectInOwner(owner, movingPart);
        targetPlc = partPlc * jcsPlcProp->getValue();
    }
    else {
        targetPlc = placementOfObjectInOwner(owner, target);
    }

    Base::Vector3d base;
    targetPlc.multVec(LocalAnchor.getValue(), base);

    if (BasePosition.getValue() == base) {
        return;
    }

    refreshing = true;
    BasePosition.setValue(base);
    refreshing = false;
}
