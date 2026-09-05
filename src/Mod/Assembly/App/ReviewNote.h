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
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#pragma once

#include <App/Annotation.h>
#include <App/Part.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyGeo.h>
#include <Base/Vector3D.h>
#include <fastsignals/signal.h>

#include <Mod/Assembly/AssemblyGlobal.h>


namespace Assembly
{

class AssemblyObject;
class ReviewNoteGroup;

enum class ReviewNoteJointSide
{
    None = 0,
    Reference1 = 1,
    Reference2 = 2,
};

class AssemblyExport ReviewNote: public App::AnnotationLabel
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::ReviewNote);

public:
    ReviewNote();
    ~ReviewNote() override;

    App::PropertyXLinkSub Target;
    App::PropertyVector LocalAnchor;
    /**
     * Stable source-definition identity (document#object) captured when the note
     * is created. Geometry notes use an object-only Target; this string and
     * AnchorSubelement retain provenance without making FaceN a link dependency.
     */
    App::PropertyString AnchorSourceIdentity;
    /** Informational picked subelement/path; never interpreted as a live link. */
    App::PropertyString AnchorSubelement;
    /** Optional unordered source pair when the note came from an interference result. */
    App::PropertyString InterferenceSourceA;
    App::PropertyString InterferenceSourceB;
    App::PropertyEnumeration JointSide;
    App::PropertyBool Resolved;
    /// Computed: true when Target is missing or invalid (drives icon refresh).
    App::PropertyBool AttachmentBroken;
    /// Legacy: stored for FCStd/undo compatibility. Display always uses nearest-border auto-attach.
    App::PropertyFloat LeaderPort;

    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderReviewNote";
    }

    PyObject* getPyObject() override;

    App::DocumentObjectExecReturn* execute() override;
    void onDocumentRestored() override;

    /// Recompute BasePosition from Target + LocalAnchor (owner-Part-local).
    void refreshBasePosition();

    /// True when Target is missing or no longer resolves.
    bool isAttachmentBroken() const;

    /// Sync AttachmentBroken output property from isAttachmentBroken().
    void updateAttachmentState();

    /// Nearest owning App::Part (or AssemblyObject) that holds the Review Notes group.
    App::Part* getOwnerPart() const;

    /// Owning AssemblyObject when the owner is an assembly; otherwise nullptr.
    AssemblyObject* getAssembly() const;

    ReviewNoteGroup* getGroup() const;

    /// First non-whitespace LabelText line (for tree display); empty clears Label.
    std::string firstTextLine() const;

    /// Fired from onChanged(TextPosition) *before* document observers run, so the
    /// ViewProvider can publish LeaderEnd atomically with the new text position.
    fastsignals::signal<void(const Base::Vector3d&)> signalSyncLeaderVisual;

    /// Ensure observers for every note owner in the document.
    static void ensureDocumentObserver(App::Document* doc);

    /// Ensure a Placement/reference observer for notes owned by this Part/Assembly.
    static void ensureOwnerObserver(App::Part* owner);

    /// Compatibility wrapper when the owner is an AssemblyObject.
    static void ensureAssemblyObserver(AssemblyObject* assembly);

    /// Drop the observer when an owning Part/Assembly is torn down.
    static void revokeOwnerObserver(App::Part* owner);

    /// Compatibility wrapper when the owner is an AssemblyObject.
    static void revokeAssemblyObserver(AssemblyObject* assembly);

    /// Recompute every ReviewNote Label in `group` to `review_note_N` (1-based,
    /// in group order). Called when the group's membership/order changes.
    static void renumberGroup(ReviewNoteGroup* group);

protected:
    void onChanged(const App::Property* prop) override;

    /// Set this note's Label to `review_note_N` based on its position in its group.
    /// Independent of LabelText, so editing text never renames the note.
    void updateReviewNoteLabel();

private:
    /// Compute this note's `review_note_N` label from its 1-based position among
    /// ReviewNote children of its group.
    std::string computeReviewNoteLabel() const;

    static const char* JointSideEnums[];
    bool updatingLabel = false;
    bool updatingAttachment = false;
    bool refreshing = false;
};

}  // namespace Assembly
