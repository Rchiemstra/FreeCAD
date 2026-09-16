// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2008 Jürgen Riegel <juergen.riegel@web.de>              *
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


#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/PyObjectBase.h>
#include <App/Document.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Document.h>
#include <Gui/Language/Translator.h>
#include <Gui/View3DInventor.h>
#include <Gui/WidgetFactory.h>

#include <Mod/Sketcher/App/GeoEnum.h>
#include <Mod/Sketcher/App/PropertyConstraintList.h>

#include "EditModeCoinManager.h"
#include "PropertyConstraintListItem.h"
#include "SketcherSettings.h"
#include "SoZoomTranslation.h"
#include "ViewProviderPython.h"
#include "ViewProviderSketch.h"
#include "ViewProviderSketchGeometryExtension.h"
#include "ViewProviderSketchGeometryExtensionPy.h"
#include "Workbench.h"


// create the commands
void CreateSketcherCommands();
void CreateSketcherCommandsCreateGeo();
void CreateSketcherCommandsConstraints();
void CreateSketcherCommandsConstraintAccel();
void CreateSketcherCommandsAlterGeo();
void CreateSketcherCommandsBSpline();
void CreateSketcherCommandsOverlay();
void CreateSketcherCommandsVirtualSpace();

void loadSketcherResource()
{
    // add resources and reloads the translators
    Q_INIT_RESOURCE(Sketcher);
    Q_INIT_RESOURCE(Sketcher_translation);
    Gui::Translator::instance()->refresh();
}

namespace SketcherGui
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("SketcherGui")
    {
        add_varargs_method(
            "getActiveSketchPreselection",
            &Module::getActiveSketchPreselection,
            "getActiveSketchPreselection(tuple(int,int)) -> dictionary or None\n"
            "\n"
            "Return the Sketcher edit-mode preselection candidate at viewport coordinates.\n"
            "The tuple uses the same coordinate system as Gui.ActiveDocument.ActiveView"
            ".getPointOnViewport().\n"
            "ConstraintKind is one of DatumAnnotation, DatumPresentation, Icon, or None.\n"
        );
        initialize("This module is the SketcherGui module.");  // register with Python
    }

    ~Module() override
    {}

private:
    Py::Object getActiveSketchPreselection(const Py::Tuple& args)
    {
        PyObject* object;
        if (!PyArg_ParseTuple(args.ptr(), "O", &object)) {
            throw Py::Exception();
        }

        const Py::Tuple tuple(object);
        Py::Long x(tuple[0]);
        Py::Long y(tuple[1]);

        auto* editDoc = Gui::Application::Instance->editDocument();
        if (!editDoc) {
            return Py::None();
        }

        auto* sketchViewProvider = freecad_cast<SketcherGui::ViewProviderSketch*>(editDoc->getInEdit());
        if (!sketchViewProvider) {
            return Py::None();
        }

        auto* view = dynamic_cast<Gui::View3DInventor*>(
            editDoc->getEditingViewOfViewProvider(sketchViewProvider)
        );
        if (!view) {
            return Py::None();
        }

        const SbVec2s viewportPos(
            static_cast<short>((long)x),
            static_cast<short>((long)y)
        );
        const EditModeCoinManager::PreselectionResult preselection
            = sketchViewProvider->getPreselectionResultAtViewportPos(
                viewportPos,
                view->getViewer()
            );
        if (!preselection.hasWinner() || !preselection.hasPickedPoint()) {
            return Py::None();
        }

        auto* sketchObject = sketchViewProvider->getObject();
        if (!sketchObject) {
            return Py::None();
        }

        std::vector<std::string> subElementNames;
        switch (preselection.Kind) {
            case EditModeCoinManager::PreselectionResult::HitKind::Point:
                subElementNames.emplace_back("Vertex" + std::to_string(preselection.PointIndex + 1));
                break;
            case EditModeCoinManager::PreselectionResult::HitKind::Edge:
                if (preselection.GeoIndex >= 0) {
                    subElementNames.emplace_back("Edge" + std::to_string(preselection.GeoIndex + 1));
                }
                else {
                    subElementNames.emplace_back(
                        "ExternalEdge"
                        + std::to_string(
                            -preselection.GeoIndex + Sketcher::GeoEnum::RefExt + 1
                        )
                    );
                }
                break;
            case EditModeCoinManager::PreselectionResult::HitKind::Axis:
                switch (preselection.Cross) {
                    case EditModeCoinManager::PreselectionResult::Axes::RootPoint:
                        subElementNames.emplace_back("RootPoint");
                        break;
                    case EditModeCoinManager::PreselectionResult::Axes::HorizontalAxis:
                        subElementNames.emplace_back("H_Axis");
                        break;
                    case EditModeCoinManager::PreselectionResult::Axes::VerticalAxis:
                        subElementNames.emplace_back("V_Axis");
                        break;
                    case EditModeCoinManager::PreselectionResult::Axes::None:
                        break;
                }
                break;
            case EditModeCoinManager::PreselectionResult::HitKind::Constraint:
                subElementNames.reserve(preselection.ConstrIndices.size());
                for (int constraintId : preselection.ConstrIndices) {
                    subElementNames.emplace_back(
                        Sketcher::PropertyConstraintList::getConstraintName(constraintId)
                    );
                }
                break;
            case EditModeCoinManager::PreselectionResult::HitKind::None:
                break;
        }

        const char* constraintKind = "None";
        switch (preselection.ConstraintKind) {
            case EditModeCoinManager::PreselectionResult::ConstraintHitKind::Icon:
                constraintKind = "Icon";
                break;
            case EditModeCoinManager::PreselectionResult::ConstraintHitKind::DatumPresentation:
                constraintKind = "DatumPresentation";
                break;
            case EditModeCoinManager::PreselectionResult::ConstraintHitKind::DatumAnnotation:
                constraintKind = "DatumAnnotation";
                break;
            case EditModeCoinManager::PreselectionResult::ConstraintHitKind::None:
            default:
                constraintKind = "None";
                break;
        }

        Py::List pySubElementNames;
        for (const auto& subElementName : subElementNames) {
            pySubElementNames.append(Py::String(subElementName));
        }

        const Base::Vector3d pickedPoint = preselection.pickedPoint();
        Py::List pyPickedPoints;
        Py::Tuple pyPoint(3);
        pyPoint.setItem(0, Py::Float(pickedPoint.x));
        pyPoint.setItem(1, Py::Float(pickedPoint.y));
        pyPoint.setItem(2, Py::Float(pickedPoint.z));
        pyPickedPoints.append(pyPoint);

        Py::Dict result;
        result.setItem("DocumentName", Py::String(sketchObject->getDocument()->getName()));
        result.setItem("ObjectName", Py::String(sketchObject->getNameInDocument()));
        result.setItem("SubElementNames", pySubElementNames);
        result.setItem("PickedPoints", pyPickedPoints);
        result.setItem("ConstraintKind", Py::String(constraintKind));
        return result;
    }
};

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}

}  // namespace SketcherGui

/* Python entry */
PyMOD_INIT_FUNC(SketcherGui)
{
    if (!Gui::Application::Instance) {
        PyErr_SetString(PyExc_ImportError, "Cannot load Gui module in console application.");
        PyMOD_Return(nullptr);
    }
    try {
        Base::Interpreter().runString("import PartGui");
        Base::Interpreter().runString("import Sketcher");
    }
    catch (const Base::Exception& e) {
        PyErr_SetString(PyExc_ImportError, e.what());
        PyMOD_Return(nullptr);
    }

    PyObject* sketcherGuiModule = SketcherGui::initModule();
    Base::Console().log("Loading GUI of Sketcher module… done\n");

    Gui::BitmapFactory().addPath(QStringLiteral(":/icons/constraints"));
    Gui::BitmapFactory().addPath(QStringLiteral(":/icons/elements"));
    Gui::BitmapFactory().addPath(QStringLiteral(":/icons/general"));
    Gui::BitmapFactory().addPath(QStringLiteral(":/icons/geometry"));
    Gui::BitmapFactory().addPath(QStringLiteral(":/icons/pointers"));
    Gui::BitmapFactory().addPath(QStringLiteral(":/icons/splines"));
    Gui::BitmapFactory().addPath(QStringLiteral(":/icons/tools"));
    Gui::BitmapFactory().addPath(QStringLiteral(":/icons/overlay"));

    // instantiating the commands
    CreateSketcherCommands();
    CreateSketcherCommandsCreateGeo();
    CreateSketcherCommandsConstraints();
    CreateSketcherCommandsAlterGeo();
    CreateSketcherCommandsConstraintAccel();
    CreateSketcherCommandsBSpline();
    CreateSketcherCommandsOverlay();
    CreateSketcherCommandsVirtualSpace();

    SketcherGui::Workbench::init();

    // Add Types to module
    Base::Interpreter().addType(
        &SketcherGui::ViewProviderSketchGeometryExtensionPy::Type,
        sketcherGuiModule,
        "ViewProviderSketchGeometryExtension"
    );

    // init objects
    SketcherGui::ViewProviderSketch::init();
    SketcherGui::ViewProviderPython::init();
    SketcherGui::ViewProviderCustom::init();
    SketcherGui::ViewProviderCustomPython::init();
    SketcherGui::SoZoomTranslation::initClass();
    SketcherGui::SoSketchFaces::initClass();
    SketcherGui::PropertyConstraintListItem::init();
    SketcherGui::ViewProviderSketchGeometryExtension::init();

    (void)new Gui::PrefPageProducer<SketcherGui::SketcherSettings>(
        QT_TRANSLATE_NOOP("QObject", "Sketcher")
    );
    (void)new Gui::PrefPageProducer<SketcherGui::SketcherSettingsGrid>(
        QT_TRANSLATE_NOOP("QObject", "Sketcher")
    );
    (void)new Gui::PrefPageProducer<SketcherGui::SketcherSettingsDisplay>(
        QT_TRANSLATE_NOOP("QObject", "Sketcher")
    );
    (void)new Gui::PrefPageProducer<SketcherGui::SketcherSettingsAppearance>(
        QT_TRANSLATE_NOOP("QObject", "Sketcher")
    );

    // add resources and reloads the translators
    loadSketcherResource();

    PyMOD_Return(sketcherGuiModule);
}
