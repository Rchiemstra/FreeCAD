// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2016 WandererFan <wandererfan@gmail.com>                *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/


# include <cstdlib>
# include <iomanip>
# include <sstream>

# include <QCoreApplication>
# include <QTimer>


#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <App/MainThreadSignal.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/Tools.h>

#include "DrawViewDraft.h"


using namespace TechDraw;

//===========================================================================
// DrawViewDraft
//===========================================================================

PROPERTY_SOURCE(TechDraw::DrawViewDraft, TechDraw::DrawViewSymbol)


DrawViewDraft::DrawViewDraft()
{
    static const char *group = "Draft view";

    ADD_PROPERTY_TYPE(Source ,(nullptr), group, App::Prop_None, "Draft object for this view");
    Source.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(LineWidth, (0.35), group, App::Prop_None, "Line width of this view. If Override Style is false, this value multiplies the object line width");
    ADD_PROPERTY_TYPE(FontSize, (12.0), group, App::Prop_None, "Text size for this view");
    ADD_PROPERTY_TYPE(Direction ,(0, 0,1.0), group, App::Prop_None, "Projection direction. The direction you are looking from.");
    ADD_PROPERTY_TYPE(Color, (0.0f, 0.0f, 0.0f), group, App::Prop_None, "The default color of text and lines");
    ADD_PROPERTY_TYPE(LineStyle, ("Solid") ,group, App::Prop_None, "A line style to use for this view. Can be Solid, Dashed, Dashdot, Dot or a SVG pattern like 0.20, 0.20");
    ADD_PROPERTY_TYPE(LineSpacing, (1.0f), group, App::Prop_None, "The spacing between lines to use for multiline texts");
    ADD_PROPERTY_TYPE(OverrideStyle, (false), group, App::Prop_None, "If True, line color, width and style of this view will override those of rendered objects");
    ScaleType.setValue("Custom");
}

DrawViewDraft::~DrawViewDraft()
{
    m_stableConnection.disconnect();
}

short DrawViewDraft::mustExecute() const
{
    if (!isRestoring()) {
        if(Source.isTouched() ||
            LineWidth.isTouched() ||
            FontSize.isTouched() ||
            Direction.isTouched() ||
            Color.isTouched() ||
            LineStyle.isTouched() ||
            LineSpacing.isTouched() ||
            OverrideStyle.isTouched()) {
            return true;
        }
    }
    return DrawViewSymbol::mustExecute();
}

void DrawViewDraft::updateSymbolFromDraft()
{
    App::DocumentObject* sourceObj = Source.getValue();
    if (!sourceObj) {
        return;
    }

    const App::Document* document = getDocument();
    if (!document) {
        return;
    }

    // Draft.get_svg for angular dimensions reads ViewObject state. Never run
    // that (or assign Symbol) off the GUI/main thread — TechDraw QGraphics
    // items are main-thread only.
    if (App::MainThreadSignalConfig::hasHooks()
        && !App::MainThreadSignalConfig::isMainThread()) {
        Base::Console().warning(
            "DrawViewDraft::updateSymbolFromDraft() skipped off the main thread for {}\n",
            getNameInDocument() ? getNameInDocument() : "?");
        return;
    }

    const std::string documentName = document->getName();
    const std::string svgHead = getSVGHead();
    const std::string svgTail = getSVGTail();
    const std::string featName = getNameInDocument();
    const std::string sourceName = sourceObj->getNameInDocument();

    std::stringstream paramStr;
    Base::Color col = Color.getValue();
    paramStr << ", scale=" << getScale()
             << ", linewidth=" << LineWidth.getValue()
             << ", fontsize=" << FontSize.getValue()
             << ", direction=FreeCAD.Vector(" << Direction.getValue().x << ", "
             << Direction.getValue().y << ", " << Direction.getValue().z << ")"
             << ", linestyle=\"" << Base::Tools::escapeEncodeString(LineStyle.getStrValue()) << "\""
             << ", color=\"" << col.asHexString() << "\""
             << ", linespacing=" << LineSpacing.getValue()
             << ", techdraw=True"
             << ", override=" << (OverrideStyle.getValue() ? "True" : "False");
    const std::string paramString = paramStr.str();

    Base::Interpreter().runString("import Draft");
    Base::Interpreter().runStringArg(
        "svgBody = Draft.get_svg(App.getDocument(\"%s\").%s %s)",
        documentName.c_str(),
        sourceName.c_str(),
        paramString.c_str());
    Base::Interpreter().runStringArg(
        "App.getDocument(\"%s\").%s.Symbol = '%s' + svgBody + '%s'",
        documentName.c_str(),
        featName.c_str(),
        svgHead.c_str(),
        svgTail.c_str());
}

void DrawViewDraft::scheduleDeferredSymbolUpdate()
{
    App::Document* document = getDocument();
    if (!document) {
        return;
    }

    m_symbolUpdatePending = true;

    if (!m_stableConnection.connected()) {
        m_stableConnection = document->signalBecameStable.connect(
            [this](const App::Document& changed) {
                onDocumentBecameStable(changed);
            });
    }

    // If the document is already outside a commit barrier, becameStable for
    // this recompute still fires when the barrier finishes. No immediate fill.
}

void DrawViewDraft::runDeferredSymbolUpdate()
{
    App::Document* document = getDocument();
    if (!document || !m_symbolUpdatePending) {
        return;
    }

    if (std::getenv("FREECAD_OFF_GUI_RECOMPUTE")) {
        // Test/lane helper recompute_off_gui_thread fills Symbol after the worker joins.
        return;
    }

    const bool documentBusy = document->mustExecute()
        || document->testStatus(App::Document::Recomputing)
        || (document->executionLane() && !document->executionLane()->isIdle());
    if (documentBusy) {
        if (!QCoreApplication::instance()) {
            return;
        }
        const std::string documentName = document->getName();
        const char* objectNamePtr = getNameInDocument();
        if (!objectNamePtr) {
            return;
        }
        const std::string objectName = objectNamePtr;
        QTimer::singleShot(0, qApp, [documentName, objectName]() {
            App::Document* doc = App::GetApplication().getDocument(documentName.c_str());
            if (!doc) {
                return;
            }
            App::DocumentObject* object = doc->getObject(objectName.c_str());
            auto* draftView = freecad_cast<DrawViewDraft*>(object);
            if (draftView) {
                draftView->runDeferredSymbolUpdate();
            }
        });
        return;
    }

    updateSymbolFromDraft();
    m_symbolUpdatePending = false;
}

void DrawViewDraft::onDocumentBecameStable(const App::Document& document)
{
    if (getDocument() != &document || !m_symbolUpdatePending) {
        return;
    }

    // signalBecameStable is a ResilientMainThreadSignal: this slot already runs
    // on the GUI thread, but still inside the owner's blocking marshal. Do not
    // touch Symbol / Draft.get_svg here — defer with a zero-delay timer so the
    // document lock and commit barrier can fully unwind (AutoSaver pattern).
    if (!QCoreApplication::instance()) {
        return;
    }

    const std::string documentName = document.getName();
    const char* objectNamePtr = getNameInDocument();
    if (!objectNamePtr) {
        return;
    }
    const std::string objectName = objectNamePtr;

    QTimer::singleShot(0, qApp, [documentName, objectName]() {
        App::Document* doc = App::GetApplication().getDocument(documentName.c_str());
        if (!doc) {
            return;
        }
        App::DocumentObject* object = doc->getObject(objectName.c_str());
        auto* draftView = freecad_cast<DrawViewDraft*>(object);
        if (draftView) {
            draftView->runDeferredSymbolUpdate();
        }
    });
}

App::DocumentObjectExecReturn *DrawViewDraft::execute()
{
    App::DocumentObject* sourceObj = Source.getValue();
    if (!sourceObj) {
        return App::DocumentObject::StdReturn;
    }

    if (!keepUpdated()) {
        return App::DocumentObject::StdReturn;
    }

    if (App::MainThreadSignalConfig::hasHooks()
        && !App::MainThreadSignalConfig::isMainThread()) {
        // Draft.get_svg needs ViewObject state on the GUI thread. Blocking
        // marshalling deadlocks while recompute holds the document lock.
        // Queue a post-stable GUI fill instead; do not touch Symbol here.
        scheduleDeferredSymbolUpdate();
        overrideKeepUpdated(false);
        return App::DocumentObject::StdReturn;
    }

    updateSymbolFromDraft();
    overrideKeepUpdated(false);
    return DrawView::execute();
}

std::string DrawViewDraft::getSVGHead()
{
    return std::string("<svg\\n") +
           std::string("	xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\"\\n") +
           std::string("	xmlns:freecad=\"https://www.freecad.org/wiki/index.php?title=Svg_Namespace\">\\n");
}

std::string DrawViewDraft::getSVGTail()
{
    return "\\n</svg>";
}

// Python Drawing feature ---------------------------------------------------------

namespace App {
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(TechDraw::DrawViewDraftPython, TechDraw::DrawViewDraft)
template<> const char* TechDraw::DrawViewDraftPython::getViewProviderName() const {
    return "TechDrawGui::ViewProviderDraft";
}
/// @endcond

// explicit template instantiation
template class TechDrawExport FeaturePythonT<TechDraw::DrawViewDraft>;
}
