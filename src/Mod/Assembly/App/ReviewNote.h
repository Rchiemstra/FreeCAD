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
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/PropertyGeo.h>

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
    App::PropertyEnumeration JointSide;
    App::PropertyBool Resolved;
    /// Computed: true when Target is missing or invalid (drives icon refresh).
    App::PropertyBool AttachmentBroken;

    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderReviewNote";
    }

    PyObject* getPyObject() override;

    App::DocumentObjectExecReturn* execute() override;
    void onDocumentRestored() override;

    /// Recompute BasePosition from Target + LocalAnchor (assembly-local).
    void refreshBasePosition();

    /// True when Target is missing or no longer resolves.
    bool isAttachmentBroken() const;

    /// Sync AttachmentBroken output property from isAttachmentBroken().
    void updateAttachmentState();

    AssemblyObject* getAssembly() const;
    ReviewNoteGroup* getGroup() const;

    /// First non-whitespace LabelText line (for tree display); empty clears Label.
    std::string firstTextLine() const;

    /// Ensure a document-level observer exists (legacy entry; delegates per-Assembly).
    static void ensureDocumentObserver(App::Document* doc);

    /// Ensure a Placement/reference observer for notes owned by this Assembly.
    static void ensureAssemblyObserver(AssemblyObject* assembly);

    /// Drop the observer when an Assembly is torn down.
    static void revokeAssemblyObserver(AssemblyObject* assembly);

protected:
    void onChanged(const App::Property* prop) override;

    void updateLabelFromText();

private:
    static const char* JointSideEnums[];
    bool updatingLabel = false;
    bool updatingAttachment = false;
    bool refreshing = false;
};

}  // namespace Assembly
