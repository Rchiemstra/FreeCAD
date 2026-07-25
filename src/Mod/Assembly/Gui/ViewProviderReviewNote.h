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

#include <App/PropertyGeo.h>
#include <Base/Placement.h>
#include <Base/Vector3D.h>
#include <Gui/ViewProviderAnnotation.h>

#include <QRect>
#include <string>
#include <vector>


class SoAnnotation;
class SoCamera;
class SoMaterial;
class SoNodeSensor;
class SoSensor;
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

    /// Leader endpoint relative to BasePosition (updated with the visible port).
    App::PropertyVector LeaderEnd;

    QIcon getIcon() const override;
    bool doubleClicked() override;
    void attach(App::DocumentObject* obj) override;
    void updateData(const App::Property* prop) override;

    /// Leader end relative to BasePosition (on the annotation box boundary).
    Base::Vector3d leaderEndpoint(const Base::Vector3d& textPosition) const override;

    /// Half-extents of the label box in annotation-local billboard units.
    void labelHalfExtents(double& halfW, double& halfH) const;

    /// Map a perimeter parameter in [0,1] to a UV offset from the text/image center.
    static Base::Vector3d perimeterOffset(double port, double halfW, double halfH);

    /// Map a UV offset from the text center to a perimeter parameter in [0,1].
    static double perimeterParam(const Base::Vector3d& offset, double halfW, double halfH);

    /// Ray from textPos toward the base origin, clipped to the label rectangle (UV).
    static Base::Vector3d autoBoundaryEndpoint(
        const Base::Vector3d& textPos,
        double halfW,
        double halfH
    );

protected:
    void onChanged(const App::Property* prop) override;
    Base::Vector3d worldToAnnotationPoint(const Base::Vector3d& world) const override;
    void drawImage(const std::vector<std::string>& lines) override;
    bool acceptLabelDragStart(SoDragger* drag, DragState& state) override;
    void setLeaderCoords(const Base::Vector3d& textPosition) override;

private:
    struct RefHit
    {
        QRect pixelRect;
        std::string objName;
        std::string subName;
    };

    struct BillboardFrame
    {
        Base::Vector3d right {1.0, 0.0, 0.0};
        Base::Vector3d up {0.0, 1.0, 0.0};
        double halfW = 0.5;
        double halfH = 0.5;
        bool valid = false;
    };

    void refreshLeaderAndPort();
    void setupPortHandle();
    void updatePortHandle(const Base::Vector3d& textPos);
    bool hitTestReference(SoDragger* drag, RefHit& out) const;
    void selectReference(const RefHit& hit) const;

    BillboardFrame currentBillboardFrame(const Base::Vector3d& textPosition) const;
    Base::Vector3d billboardOffsetToLocal(const Base::Vector3d& uvOffset, const BillboardFrame& frame)
        const;
    Base::Vector3d worldToAnnotationDirection(const Base::Vector3d& worldDir) const;
    Base::Vector3d textPositionWorld(const Base::Vector3d& textPosition) const;
    bool screenWorldPerPixel(const Base::Vector3d& textWorld, double& worldPerPixel) const;
    void ensureCameraSensor();
    void detachCameraSensor();

    static void portDragStartCallback(void* data, SoDragger* d);
    static void portDragMotionCallback(void* data, SoDragger* d);
    static void portDragFinishCallback(void* data, SoDragger* d);
    static void cameraSensorCallback(void* data, SoSensor* sensor);
    static void cameraSensorDeleteCallback(void* data, SoSensor* sensor);

    std::vector<RefHit> refHits;
    SoAnnotation* portAnnotation = nullptr;
    SoTranslation* portTranslation = nullptr;
    SoSphere* portSphere = nullptr;
    SoMaterial* portMaterial = nullptr;
    SoNodeSensor* cameraSensor = nullptr;
    SoCamera* attachedCamera = nullptr;
    bool portDragging = false;
    double pendingPort = -1.0;
    Base::Vector3d portDragPlanePoint;
    Base::Vector3d portDragPlaneNormal;
};

}  // namespace AssemblyGui
