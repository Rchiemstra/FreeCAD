// SPDX - License - Identifier: LGPL - 2.1 - or -later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2025 Pierre-Louis Boyer                                  *
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

#include <vector>

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Gui/Application.h>
#include <Gui/CommandT.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Tree.h>

#include <Mod/Assembly/App/AssemblyLink.h>
#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/AssemblyUtils.h>
#include <Mod/Assembly/App/InterferenceScan.h>

#include "Commands.h"
#include "ViewProviderAssembly.h"
#include "TaskInterferenceCheck.h"


using namespace Assembly;
using namespace AssemblyGui;

// Helper function to get the active AssemblyObject in edit mode
static AssemblyObject* getActiveAssembly()
{
    Gui::Document* doc = Gui::Application::Instance->activeDocument();
    if (!doc) {
        return nullptr;
    }

    auto* vp = doc->getInEdit();
    if (auto* assemblyVP = freecad_cast<ViewProviderAssembly*>(vp)) {
        return assemblyVP->getObject<AssemblyObject>();
    }

    return nullptr;
}

static std::vector<InterferenceSelectionHandle> currentInterferenceSelectionHandles()
{
    std::vector<InterferenceSelectionHandle> handles;
    auto selection = Gui::Selection().getSelectionEx(
        "",
        App::DocumentObject::getClassTypeId(),
        Gui::ResolveMode::NoResolve
    );
    for (auto& sel : selection) {
        App::DocumentObject* obj = sel.getObject();
        if (!obj) {
            continue;
        }
        const auto subs = sel.getSubNames();
        if (subs.empty()) {
            handles.push_back({obj, {}});
            continue;
        }
        for (const auto& sub : subs) {
            handles.push_back({obj, sub});
        }
    }
    return handles;
}

/** Active assembly, else a selected App::Part interference root (e.g. plain Part).
 * Explicit selection-backed roots take priority over unrelated edit-mode assemblies.
 */
static App::DocumentObject* getInterferenceHost()
{
    const auto handles = currentInterferenceSelectionHandles();
    return resolveInterferenceHostFromHandles(handles, getActiveAssembly());
}

static InterferenceSelectedPairRequest currentSelectedPairRequest()
{
    return resolveInterferenceSelectedPairRequest(
        currentInterferenceSelectionHandles(),
        getActiveAssembly()
    );
}

void selectObjects(const std::vector<App::DocumentObject*>& objectsToSelect)
{
    if (objectsToSelect.empty()) {
        return;
    }

    Gui::Selection().clearSelection();
    for (App::DocumentObject* obj : objectsToSelect) {
        Gui::Selection().addSelection(obj->getDocument()->getName(), obj->getNameInDocument());
    }
}

void selectObjectsByName(AssemblyObject* assembly, const std::vector<std::string>& names)
{
    if (!assembly || names.empty()) {
        return;
    }

    std::vector<App::DocumentObject*> objectsToSelect;
    App::Document* doc = assembly->getDocument();

    for (const auto& name : names) {
        if (auto* obj = doc->getObject(name.c_str())) {
            objectsToSelect.push_back(obj);
        }
    }

    selectObjects(objectsToSelect);
}

// ================================================================================
// Go to Linked Assembly
// ================================================================================

DEF_STD_CMD_A(CmdAssemblyLinkSelectLinked)

CmdAssemblyLinkSelectLinked::CmdAssemblyLinkSelectLinked()
    : Command("Assembly_LinkSelectLinked")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Go to Linked Assembly");
    sToolTipText = QT_TR_NOOP("Selects the linked assembly and switches to its original document");
    sWhatsThis = "Assembly_LinkSelectLinked";
    sStatusTip = sToolTipText;
    eType = AlterSelection;
    sPixmap = "LinkSelect";
    sAccel = "S, G";
}

void CmdAssemblyLinkSelectLinked::activated(int iMsg)
{
    Q_UNUSED(iMsg);

    std::vector<Gui::SelectionObject> selection = Gui::Selection().getSelectionEx();
    if (selection.size() != 1) {
        return;
    }

    auto* asmLink = dynamic_cast<Assembly::AssemblyLink*>(selection[0].getObject());

    if (!asmLink) {
        return;
    }

    // Get the linked object (usually an AssemblyObject in another doc)
    App::DocumentObject* linkedObj = asmLink->getLinkedAssembly();
    if (!linkedObj) {
        return;
    }

    Gui::Selection().clearSelection();
    Gui::Selection().addSelection(linkedObj->getDocument()->getName(), linkedObj->getNameInDocument());

    // Switch view/tab
    Gui::Document* guiDoc = Gui::Application::Instance->getDocument(linkedObj->getDocument());
    if (guiDoc) {
        // Try to activate the view containing the object
        Gui::ViewProvider* vp = guiDoc->getViewProvider(linkedObj);
        if (auto vpDoc = dynamic_cast<Gui::ViewProviderDocumentObject*>(vp)) {
            guiDoc->setActiveView(vpDoc);
        }
    }
}

bool CmdAssemblyLinkSelectLinked::isActive()
{
    std::vector<Gui::SelectionObject> selection = Gui::Selection().getSelectionEx();
    return (
        selection.size() == 1 && selection[0].getObject()
        && selection[0].getObject()->isDerivedFrom(Assembly::AssemblyLink::getClassTypeId())
    );
}


// ================================================================================
// Select Conflicting Constraints
// ================================================================================

DEF_STD_CMD_A(CmdAssemblySelectConflictingConstraints)

CmdAssemblySelectConflictingConstraints::CmdAssemblySelectConflictingConstraints()
    : Command("Assembly_SelectConflictingConstraints")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Select Conflicting Constraints");
    sToolTipText = QT_TR_NOOP("Selects conflicting joints in the active assembly");
    sWhatsThis = "Assembly_SelectConflictingConstraints";
    sStatusTip = sToolTipText;
    eType = ForEdit;
}

void CmdAssemblySelectConflictingConstraints::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    AssemblyObject* assembly = getActiveAssembly();
    if (!assembly) {
        return;
    }

    // NOTE: The solver currently reports conflicting constraints as redundant.
    // This uses the redundant list until the solver provides a separate conflicting list.
    selectObjectsByName(assembly, assembly->getLastRedundant());
}

bool CmdAssemblySelectConflictingConstraints::isActive()
{
    return getActiveAssembly() != nullptr;
}

// ================================================================================
// Select Redundant Constraints
// ================================================================================

DEF_STD_CMD_A(CmdAssemblySelectRedundantConstraints)

CmdAssemblySelectRedundantConstraints::CmdAssemblySelectRedundantConstraints()
    : Command("Assembly_SelectRedundantConstraints")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Select Redundant Constraints");
    sToolTipText = QT_TR_NOOP("Selects redundant joints in the active assembly");
    sWhatsThis = "Assembly_SelectRedundantConstraints";
    sStatusTip = sToolTipText;
    eType = ForEdit;
}

void CmdAssemblySelectRedundantConstraints::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    AssemblyObject* assembly = getActiveAssembly();
    if (!assembly) {
        return;
    }

    selectObjectsByName(assembly, assembly->getLastRedundant());
}

bool CmdAssemblySelectRedundantConstraints::isActive()
{
    return getActiveAssembly() != nullptr;
}

// ================================================================================
// Select Malformed Constraints
// ================================================================================

DEF_STD_CMD_A(CmdAssemblySelectMalformedConstraints)

CmdAssemblySelectMalformedConstraints::CmdAssemblySelectMalformedConstraints()
    : Command("Assembly_SelectMalformedConstraints")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Select Malformed Constraints");
    sToolTipText = QT_TR_NOOP("Selects malformed joints in the active assembly");
    sWhatsThis = "Assembly_SelectMalformedConstraints";
    sStatusTip = sToolTipText;
    eType = ForEdit;
}

void CmdAssemblySelectMalformedConstraints::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    AssemblyObject* assembly = getActiveAssembly();
    if (!assembly) {
        return;
    }

    selectObjectsByName(assembly, assembly->getLastMalformed());
}

bool CmdAssemblySelectMalformedConstraints::isActive()
{
    return getActiveAssembly() != nullptr;
}


// ================================================================================
// Select Components with Degrees of Freedom
// ================================================================================

DEF_STD_CMD_A(CmdAssemblySelectComponentsWithDoFs)

CmdAssemblySelectComponentsWithDoFs::CmdAssemblySelectComponentsWithDoFs()
    : Command("Assembly_SelectComponentsWithDoFs")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Select Components With DoFs");
    sToolTipText = QT_TR_NOOP("Selects unconstrained components in the active assembly");
    sWhatsThis = "Assembly_SelectComponentsWithDoFs";
    sStatusTip = sToolTipText;
    eType = ForEdit;
}

void CmdAssemblySelectComponentsWithDoFs::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    AssemblyObject* assembly = getActiveAssembly();
    if (!assembly) {
        return;
    }

    std::vector<App::DocumentObject*> objectsToSelect;
    std::vector<App::DocumentObject*> allParts = getAssemblyComponents(assembly);

    // Iterate through all collected parts and check their connectivity
    for (App::DocumentObject* part : allParts) {
        if (!assembly->isPartConnected(part)) {
            objectsToSelect.push_back(part);
        }
    }

    selectObjects(objectsToSelect);
}

bool CmdAssemblySelectComponentsWithDoFs::isActive()
{
    return getActiveAssembly() != nullptr;
}

// ================================================================================
// Select Joints of Component
// ================================================================================

DEF_STD_CMD_A(CmdAssemblySelectJointsOfComponent)

CmdAssemblySelectJointsOfComponent::CmdAssemblySelectJointsOfComponent()
    : Command("Assembly_SelectJointsOfComponent")
{
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Select Component Joints");
    sToolTipText = QT_TR_NOOP("Selects all joints referencing the selected component");
    sWhatsThis = "Assembly_SelectJointsOfComponent";
    sStatusTip = sToolTipText;
    sPixmap = "Assembly_SelectJointsOfComponent";
    eType = ForEdit;
}

void CmdAssemblySelectJointsOfComponent::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    AssemblyObject* assembly = getActiveAssembly();
    if (!assembly) {
        return;
    }

    auto selection = Gui::Selection().getSelectionEx(
        "",
        App::DocumentObject::getClassTypeId(),
        Gui::ResolveMode::NoResolve
    );
    if (selection.empty()) {
        return;
    }

    std::set<App::DocumentObject*> components;

    for (auto& sel : selection) {
        const std::vector<std::string> subs = sel.getSubNames();
        std::string sub = subs.empty() ? "" : subs.front();

        if (App::DocumentObject* comp = getMovingPartFromSel(assembly, sel.getObject(), sub)) {
            components.insert(comp);
        }
    }

    if (components.empty()) {
        return;
    }

    std::vector<App::DocumentObject*> jointsToSelect;
    for (auto* comp : components) {
        std::vector<App::DocumentObject*> partJoints = assembly->getJointsOfPart(comp);
        jointsToSelect.insert(jointsToSelect.end(), partJoints.begin(), partJoints.end());
    }

    selectObjects(jointsToSelect);
}

bool CmdAssemblySelectJointsOfComponent::isActive()
{
    return getActiveAssembly() != nullptr && !Gui::Selection().getSelection().empty();
}


DEF_STD_CMD_A(CmdAssemblyCheckInterference)

CmdAssemblyCheckInterference::CmdAssemblyCheckInterference()
    : Command("Assembly_CheckInterference")
{
    sAppModule = "Assembly";
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Check Interference");
    sToolTipText = QT_TR_NOOP(
        "Check interference for the active App::Part or assembly: with exactly two selected "
        "subelements, check that component pair; otherwise check all applicable component "
        "occurrences (hidden omitted unless Include hidden)"
    );
    sWhatsThis = "Assembly_CheckInterference";
    sStatusTip = sToolTipText;
    sPixmap = "Assembly_CheckInterference";
    eType = AlterDoc;
}

void CmdAssemblyCheckInterference::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    App::DocumentObject* host = getInterferenceHost();
    if (!host) {
        return;
    }
    showInterferenceCheckPanel(host);
}

bool CmdAssemblyCheckInterference::isActive()
{
    return getInterferenceHost() != nullptr;
}


DEF_STD_CMD_A(CmdAssemblyCheckSelectedComponents)

CmdAssemblyCheckSelectedComponents::CmdAssemblyCheckSelectedComponents()
    : Command("Assembly_CheckSelectedComponents")
{
    sAppModule = "Assembly";
    sGroup = QT_TR_NOOP("Assembly");
    sMenuText = QT_TR_NOOP("Check Selected Components");
    sToolTipText = QT_TR_NOOP(
        "Check interference between the two selected component occurrences under the active "
        "part or assembly (selected faces are pick handles only)"
    );
    sWhatsThis = "Assembly_CheckSelectedComponents";
    sStatusTip = sToolTipText;
    sPixmap = "Assembly_CheckInterference";
    eType = AlterDoc;
}

void CmdAssemblyCheckSelectedComponents::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    const auto request = currentSelectedPairRequest();
    if (!request.valid()) {
        return;
    }
    showInterferenceCheckPanel(request.host, request.first, request.second);
}

bool CmdAssemblyCheckSelectedComponents::isActive()
{
    return currentSelectedPairRequest().valid();
}


// ================================================================================
// Command Creation
// ================================================================================

void AssemblyGui::CreateAssemblyCommands()
{
    Gui::CommandManager& rcCmdMgr = Gui::Application::Instance->commandManager();

    rcCmdMgr.addCommand(new CmdAssemblyLinkSelectLinked());
    rcCmdMgr.addCommand(new CmdAssemblySelectConflictingConstraints());
    rcCmdMgr.addCommand(new CmdAssemblySelectRedundantConstraints());
    rcCmdMgr.addCommand(new CmdAssemblySelectMalformedConstraints());
    rcCmdMgr.addCommand(new CmdAssemblySelectComponentsWithDoFs());
    rcCmdMgr.addCommand(new CmdAssemblySelectJointsOfComponent());
    rcCmdMgr.addCommand(new CmdAssemblyCheckInterference());
    rcCmdMgr.addCommand(new CmdAssemblyCheckSelectedComponents());
}
