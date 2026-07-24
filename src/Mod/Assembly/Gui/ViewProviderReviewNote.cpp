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

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/GeoFeature.h>
#include <Base/Placement.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>

#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/ReviewNote.h>

#include "ViewProviderReviewNote.h"

using namespace AssemblyGui;

PROPERTY_SOURCE(AssemblyGui::ViewProviderReviewNote, Gui::ViewProviderAnnotationLabel)

QIcon ViewProviderReviewNote::getIcon() const
{
    auto* note = getObject<Assembly::ReviewNote>();
    if (!note) {
        return Gui::BitmapFactory().pixmap("Assembly_ReviewNote.svg");
    }

    // Prefer the persisted AttachmentBroken output so icon changes track property notifies.
    if (note->AttachmentBroken.getValue() || note->isAttachmentBroken()) {
        return Gui::BitmapFactory().pixmap("Assembly_ReviewNoteBroken.svg");
    }
    if (note->Resolved.getValue()) {
        return Gui::BitmapFactory().pixmap("Assembly_ReviewNoteResolved.svg");
    }
    return Gui::BitmapFactory().pixmap("Assembly_ReviewNote.svg");
}

bool ViewProviderReviewNote::doubleClicked()
{
    std::string obj_name = getObject()->getNameInDocument();
    std::string doc_name = getObject()->getDocument()->getName();

    std::string pythonCommand =
        "import CommandReviewNote\n"
        "obj = App.getDocument('"
        + doc_name + "').getObject('" + obj_name
        + "')\n"
          "CommandReviewNote.edit_review_note(obj)\n";

    Gui::Command::runCommand(Gui::Command::App, pythonCommand.c_str());
    return true;
}

void ViewProviderReviewNote::updateData(const App::Property* prop)
{
    ViewProviderAnnotationLabel::updateData(prop);

    auto* note = getObject<Assembly::ReviewNote>();
    if (!note || !prop) {
        return;
    }

    if (prop == &note->Resolved || prop == &note->AttachmentBroken || prop == &note->Target
        || prop == &note->JointSide || prop == &note->LabelText || prop == &note->BasePosition
        || prop == &note->LocalAnchor) {
        signalChangeIcon();
    }
}

Base::Vector3d ViewProviderReviewNote::worldToAnnotationPoint(const Base::Vector3d& world) const
{
    auto* note = getObject<Assembly::ReviewNote>();
    if (!note) {
        return world;
    }
    if (auto* owner = note->getOwnerPart()) {
        Base::Vector3d local;
        App::GeoFeature::getGlobalPlacement(owner).inverse().multVec(world, local);
        return local;
    }
    return world;
}
