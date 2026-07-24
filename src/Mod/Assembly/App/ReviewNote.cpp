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

#include <cstring>
#include <map>
#include <memory>
#include <string>

#include <App/Application.h>
#include <App/Document.h>
#include <App/FeaturePythonPyImp.h>
#include <App/GeoFeature.h>
#include <App/GroupExtension.h>
#include <App/Link.h>
#include <App/PropertyGeo.h>
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

bool geometrySubExists(App::DocumentObject* obj, const std::string& sub)
{
    if (!obj) {
        return false;
    }
    if (sub.empty() || sub == "Main") {
        return true;
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

bool objectInAssembly(AssemblyObject* assembly, App::DocumentObject* obj)
{
    if (!assembly || !obj) {
        return false;
    }
    if (obj == assembly) {
        return true;
    }
    return assembly->hasObject(obj, true);
}

bool groupContainsObject(const App::DocumentObject& container, const App::DocumentObject* obj)
{
    if (!obj) {
        return false;
    }
    auto* group = container.getExtensionByType<App::GroupExtension>(true);
    return group && group->hasObject(obj, true);
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
        }
    }

    return false;
}

Base::Placement placementOfObjectInAssembly(AssemblyObject* assembly, App::DocumentObject* obj)
{
    if (!assembly || !obj) {
        return {};
    }

    auto parents = obj->getParents();
    for (const auto& parent : parents) {
        if (parent.first == assembly) {
            Base::Placement full = assembly->getPlacementOf(parent.second, obj);
            if (auto* asmPlc = assembly->getPlacementProperty()) {
                return asmPlc->getValue().inverse() * full;
            }
            return full;
        }
    }

    if (assembly->hasObject(obj, false)) {
        if (auto* plcProp = obj->getPlacementProperty()) {
            return plcProp->getValue();
        }
    }

    if (auto* plcProp = obj->getPlacementProperty()) {
        return plcProp->getValue();
    }
    return {};
}

/// One observer per Assembly: only refreshes notes owned by that Assembly.
class ReviewNoteAssemblyTracker
{
public:
    explicit ReviewNoteAssemblyTracker(AssemblyObject* assembly)
        : assembly(assembly)
    {
        connChanged = App::GetApplication().signalChangedObject.connect(
            [this](const App::DocumentObject& obj, const App::Property& prop) {
                onChangedObject(obj, prop);
            }
        );
        if (App::Document* doc = assembly->getDocument()) {
            // Plan: also refresh on recompute (covers paths where Placement is not the
            // only updated state, and document restore recomputes).
            connRecomputed = doc->signalRecomputedObject.connect(
                [this](const App::DocumentObject& obj) {
                    onRecomputedObject(obj);
                }
            );
        }
    }

    AssemblyObject* getAssembly() const
    {
        return assembly;
    }

    App::Document* getDocument() const
    {
        return assembly ? assembly->getDocument() : nullptr;
    }

    void onChangedObject(const App::DocumentObject& Obj, const App::Property& Prop)
    {
        if (!assembly || !assembly->isAttachedToDocument()) {
            return;
        }
        App::Document* doc = assembly->getDocument();
        if (!doc || Obj.getDocument() != doc) {
            return;
        }
        if (Obj.isDerivedFrom(ReviewNote::getClassTypeId())) {
            return;
        }
        if (!isPlacementLikeProperty(Prop)) {
            return;
        }
        // During Assembly teardown, Group changes can fire after unsetup begins.
        if (!Obj.isAttachedToDocument()) {
            return;
        }
        if (&Obj != assembly && !assembly->hasObject(&Obj, true)) {
            return;
        }

        const char* propName = Prop.getName();
        const bool isGroup = propName && strcmp(propName, "Group") == 0;

        for (auto* obj : doc->getObjectsOfType(ReviewNote::getClassTypeId())) {
            auto* note = freecad_cast<ReviewNote*>(obj);
            if (!note || !note->isAttachedToDocument() || note->getAssembly() != assembly) {
                continue;
            }
            // Shape and placement-like props: only notes that depend on Obj.
            // Group: assembly / note-group membership can orphan or rehome targets.
            if (noteDependsOnObject(*note, Obj)
                || (isGroup && (&Obj == assembly || &Obj == note->getGroup()))) {
                note->refreshBasePosition();
            }
        }
    }

    void onRecomputedObject(const App::DocumentObject& Obj)
    {
        if (!assembly || !assembly->isAttachedToDocument()) {
            return;
        }
        App::Document* doc = assembly->getDocument();
        if (!doc || Obj.getDocument() != doc) {
            return;
        }
        if (Obj.isDerivedFrom(ReviewNote::getClassTypeId())) {
            return;
        }
        if (!Obj.isAttachedToDocument()) {
            return;
        }
        if (&Obj != assembly && !assembly->hasObject(&Obj, true)) {
            return;
        }

        for (auto* obj : doc->getObjectsOfType(ReviewNote::getClassTypeId())) {
            auto* note = freecad_cast<ReviewNote*>(obj);
            if (!note || !note->isAttachedToDocument() || note->getAssembly() != assembly) {
                continue;
            }
            if (noteDependsOnObject(*note, Obj)) {
                note->refreshBasePosition();
            }
        }
    }

private:
    AssemblyObject* assembly = nullptr;
    fastsignals::scoped_connection connChanged;
    fastsignals::scoped_connection connRecomputed;
};

std::map<AssemblyObject*, std::unique_ptr<ReviewNoteAssemblyTracker>> g_assemblyTrackers;
fastsignals::scoped_connection g_docDeleteHook;
bool g_docDeleteHookInstalled = false;

void revokeTrackersForDocument(const App::Document& doc)
{
    for (auto it = g_assemblyTrackers.begin(); it != g_assemblyTrackers.end();) {
        App::Document* trackerDoc = it->second ? it->second->getDocument() : nullptr;
        // Also drop entries whose Assembly was already destroyed (null / detached).
        if (!it->second || !it->second->getAssembly() || trackerDoc == &doc) {
            it = g_assemblyTrackers.erase(it);
        }
        else {
            ++it;
        }
    }
}

void ensureDocumentDeleteHook()
{
    if (g_docDeleteHookInstalled) {
        return;
    }
    // closeDocument() does not call unsetupObject; tear down observers here instead.
    g_docDeleteHook = App::GetApplication().signalDeleteDocument.connect(
        [](const App::Document& doc) {
            revokeTrackersForDocument(doc);
        }
    );
    g_docDeleteHookInstalled = true;
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
        ensureAssemblyObserver(note->getAssembly());
    }
}

void ReviewNote::ensureAssemblyObserver(AssemblyObject* assembly)
{
    if (!assembly) {
        return;
    }
    ensureDocumentDeleteHook();

    auto& tracker = g_assemblyTrackers[assembly];
    if (!tracker || tracker->getAssembly() != assembly) {
        tracker = std::make_unique<ReviewNoteAssemblyTracker>(assembly);
    }

    for (auto it = g_assemblyTrackers.begin(); it != g_assemblyTrackers.end();) {
        if (!it->second || !it->second->getAssembly()) {
            it = g_assemblyTrackers.erase(it);
        }
        else {
            ++it;
        }
    }
}

void ReviewNote::revokeAssemblyObserver(AssemblyObject* assembly)
{
    if (!assembly) {
        return;
    }
    g_assemblyTrackers.erase(assembly);
}

App::DocumentObjectExecReturn* ReviewNote::execute()
{
    ensureAssemblyObserver(getAssembly());
    refreshBasePosition();
    return App::DocumentObject::StdReturn;
}

void ReviewNote::onDocumentRestored()
{
    App::AnnotationLabel::onDocumentRestored();
    ensureAssemblyObserver(getAssembly());
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
        ensureAssemblyObserver(getAssembly());
    }

    if (prop == &LabelText) {
        updateLabelFromText();
    }
    else if (prop == &Target || prop == &LocalAnchor || prop == &JointSide) {
        if (!isRestoring() && !refreshing) {
            refreshBasePosition();
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

    auto* assembly = getAssembly();
    if (!assembly || !objectInAssembly(assembly, target)) {
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
        if (refObj->getDocument() != getDocument() || !objectInAssembly(assembly, refObj)) {
            return true;
        }
        if (auto* moving = getMovingPartFromRef(ref)) {
            if (moving->getDocument() != getDocument() || !objectInAssembly(assembly, moving)) {
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

AssemblyObject* ReviewNote::getAssembly() const
{
    for (auto* obj : getInList()) {
        if (auto* group = freecad_cast<ReviewNoteGroup*>(obj)) {
            for (auto* parent : group->getInList()) {
                if (auto* assembly = freecad_cast<AssemblyObject*>(parent)) {
                    return assembly;
                }
            }
        }
        if (auto* assembly = freecad_cast<AssemblyObject*>(obj)) {
            return assembly;
        }
    }
    return nullptr;
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

    ensureAssemblyObserver(getAssembly());
    updateAttachmentState();

    if (isAttachmentBroken()) {
        // Keep last valid BasePosition when attachment is broken.
        return;
    }

    auto* target = Target.getValue();
    auto* assembly = getAssembly();
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

        Base::Placement partPlc = placementOfObjectInAssembly(assembly, movingPart);
        targetPlc = partPlc * jcsPlcProp->getValue();
    }
    else {
        targetPlc = placementOfObjectInAssembly(assembly, target);
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
