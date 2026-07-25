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

#pragma once

#include <Mod/Assembly/AssemblyGlobal.h>

#include <Base/Placement.h>
#include <Base/Vector3D.h>
#include <Gui/ViewProviderAnnotation.h>

#include <QRect>
#include <string>
#include <vector>


class SoAnnotation;
class SoSeparator;
class SoSphere;
class SoTranslation;
class SoDragger;

namespace AssemblyGui
{

class AssemblyGuiExport ViewProviderReviewNote: public Gui::ViewProviderAnnotationLabel
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderReviewNote);

public:
    ViewProviderReviewNote();
    ~ViewProviderReviewNote() override;

    QIcon getIcon() const override;
    bool doubleClicked() override;
    void attach(App::DocumentObject* obj) override;
    void updateData(const App::Property* prop) override;

    /// Leader end relative to BasePosition (on the annotation box boundary).
    Base::Vector3d leaderEndpoint(const Base::Vector3d& textPosition) const override;

    /// Half-extents of the label box in annotation-local units.
    void labelHalfExtents(double& halfW, double& halfH) const;

    /// Map a perimeter parameter in [0,1] to an offset from the text/image center.
    static Base::Vector3d perimeterOffset(double port, double halfW, double halfH);

    /// Map an offset from the text center to a perimeter parameter in [0,1].
    static double perimeterParam(const Base::Vector3d& offset, double halfW, double halfH);

    /// Ray from textPos toward the base origin, clipped to the label rectangle.
    static Base::Vector3d autoBoundaryEndpoint(
        const Base::Vector3d& textPos,
        double halfW,
        double halfH
    );

protected:
    Base::Vector3d worldToAnnotationPoint(const Base::Vector3d& world) const override;
    void drawImage(const std::vector<std::string>& lines) override;
    bool acceptLabelDragStart(SoDragger* drag, DragState& state) override;

private:
    struct RefHit
    {
        QRect pixelRect;
        std::string objName;
        std::string subName;
    };

    void refreshLeaderAndPort();
    void setupPortHandle();
    void updatePortHandle(const Base::Vector3d& textPos);
    bool hitTestReference(SoDragger* drag, RefHit& out) const;
    void selectReference(const RefHit& hit) const;

    static void portDragStartCallback(void* data, SoDragger* d);
    static void portDragMotionCallback(void* data, SoDragger* d);
    static void portDragFinishCallback(void* data, SoDragger* d);

    std::vector<RefHit> refHits;
    SoAnnotation* portAnnotation = nullptr;
    SoTranslation* portTranslation = nullptr;
    SoSphere* portSphere = nullptr;
    bool portDragging = false;
    double pendingPort = -1.0;
    Base::Vector3d portDragPlanePoint;
    Base::Vector3d portDragPlaneNormal;
};

}  // namespace AssemblyGui
