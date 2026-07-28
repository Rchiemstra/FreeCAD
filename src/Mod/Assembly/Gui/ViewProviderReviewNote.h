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
#include <App/PropertyUnits.h>
#include <Base/Placement.h>
#include <Base/Vector3D.h>
#include <Gui/ViewProviderAnnotation.h>
#include <fastsignals/signal.h>

#include <QObject>
#include <QPointer>
#include <QRect>
#include <string>
#include <vector>


class SoCamera;
class SoNodeSensor;
class SoOneShotSensor;
class SoSensor;
class SoDragger;

namespace AssemblyGui
{

class AssemblyGuiExport ViewProviderReviewNote: public Gui::ViewProviderAnnotationLabel
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderReviewNote);

public:
    ViewProviderReviewNote();
    ~ViewProviderReviewNote() override;

    /// Leader endpoint relative to BasePosition (nearest point on the text-box border).
    App::PropertyVector LeaderEnd;
    /// Billboard half-extents (x=halfW, y=halfH) used for leader attachment, relative to TextPosition.
    App::PropertyVector LeaderHalfExtent;
    /// Fixed box width in mm (0 = auto-size to text). Overrides the auto billboard half-width.
    App::PropertyLength BoxWidth;
    /// Fixed box height in mm (0 = auto-size to text). Overrides the auto billboard half-height.
    App::PropertyLength BoxHeight;
    /// Test-visible: pixel width of the last rasterized box image (capped at MaxBoxRasterPx).
    App::PropertyInteger RasterWidth;
    /// Test-visible: pixel height of the last rasterized box image (capped at MaxBoxRasterPx).
    App::PropertyInteger RasterHeight;
    /// Test-visible: number of times drawImage has rasterized the box (incremented per raster;
    /// used to verify resize-triggered re-raster).
    App::PropertyInteger DrawImageCount;

    QIcon getIcon() const override;
    bool doubleClicked() override;
    void attach(App::DocumentObject* obj) override;
    void updateData(const App::Property* prop) override;

    /// Leader end relative to BasePosition (nearest border point toward the anchor).
    Base::Vector3d leaderEndpoint(const Base::Vector3d& textPosition) const override;

    /// Half-extents of the label box in annotation-local billboard units.
    void labelHalfExtents(double& halfW, double& halfH) const;

    /// True when the user set a fixed box width/height (mm) from the View panel.
    bool hasFixedSize() const;

    /// Map a perimeter parameter in [0,1] to a UV offset from the text/image center.
    static Base::Vector3d perimeterOffset(double port, double halfW, double halfH);

    /// Map a UV offset from the text center to a perimeter parameter in [0,1].
    static double perimeterParam(const Base::Vector3d& offset, double halfW, double halfH);

    /// Nearest point on the label rectangle border along the line toward the base.
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
    /// P2: Qt event filter that watches the 3D-view GL widget for QEvent::Resize and
    /// re-schedules a visual frame so a fixed-mm box re-rasterizes at the new zoom.
    /// Nested so it can reach the private scheduleVisualFrame().
    class ResizeObserver;
    friend class ResizeObserver;

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

    void refreshLeader();
    /// Apply text translation + leader endpoint from one position/camera snapshot.
    void applyVisualFrame(const Base::Vector3d& textPosition, bool updateTextTranslation);
    void scheduleVisualFrame();
    void flushScheduledVisualFrame();
    bool hitTestReference(SoDragger* drag, RefHit& out) const;
    void selectReference(const RefHit& hit) const;
    void onLabelDragFinished(const DragState& state) override;

    BillboardFrame currentBillboardFrame(const Base::Vector3d& textPosition) const;
    Base::Vector3d billboardOffsetToLocal(const Base::Vector3d& uvOffset, const BillboardFrame& frame)
        const;
    Base::Vector3d worldToAnnotationDirection(const Base::Vector3d& worldDir) const;
    Base::Vector3d textPositionWorld(const Base::Vector3d& textPosition) const;
    bool screenWorldPerPixel(const Base::Vector3d& textWorld, double& worldPerPixel) const;
    void ensureCameraSensor();
    void detachCameraSensor();
    /// P2: viewport resize changes worldPerPixel (Coin has no viewport-region sensor),
    /// so a fixed-mm box must re-rasterize. Installs a Qt event filter on the viewer GL widget.
    void ensureViewportResizeObserver();
    void detachViewportResizeObserver();
    void detachFrameSensor();

    static void cameraSensorCallback(void* data, SoSensor* sensor);
    static void cameraSensorDeleteCallback(void* data, SoSensor* sensor);
    static void frameSensorCallback(void* data, SoSensor* sensor);

    std::vector<RefHit> refHits;
    SoNodeSensor* cameraSensor = nullptr;
    SoCamera* attachedCamera = nullptr;
    /// P2: Qt event filter on the viewer GL widget (viewport-resize -> re-raster).
    ResizeObserver* resizeObserver = nullptr;
    /// P2: held via QPointer so it auto-clears if Qt destroys the GL widget before
    /// the view provider (avoids a dangling pointer in detachViewportResizeObserver).
    QPointer<QWidget> attachedGlWidget;
    /// Coalesced visual refresh. SoOneShotSensor (not SoIdleSensor) so Quarter's
    /// processDelayQueue(false) still runs it. Priority is redrawPri-1 (lower
    /// numeric = earlier); redraw OneShot is typically 10000.
    SoOneShotSensor* frameSensor = nullptr;
    /// Last camera-derived half-extents. Reused when a transient viewer/camera
    /// lookup fails so we never fall back to FontSize*bitmap (looks detached).
    mutable double lastHalfW = 0.5;
    mutable double lastHalfH = 0.5;
    mutable bool hasLastHalfExtent = false;
    mutable int lastViewportWidthPx = 0;
    /// Re-entrancy / coalesce guards: drag, property, and camera callbacks share
    /// one applyVisualFrame path so a frame never shows new text + stale leader.
    bool applyingVisualFrame = false;
    bool visualFrameScheduled = false;
    bool visualFrameDirty = false;
    /// P3: re-entrancy guard while clamping negative BoxWidth/BoxHeight to 0.
    bool updatingBoxSize = false;
    fastsignals::scoped_connection syncLeaderConnection;
};

/// P2: Qt event filter on the 3D-view GL widget. Viewport resize changes
/// worldPerPixel but not the Coin camera node, so without this the fixed-mm box
/// stays stale until the next camera change. On QEvent::Resize it re-schedules a
/// visual frame (which re-rasterizes when hasFixedSize()).
class AssemblyGuiExport ViewProviderReviewNote::ResizeObserver: public QObject
{
    Q_OBJECT
public:
    explicit ResizeObserver(ViewProviderReviewNote* owner)
        : QObject(nullptr)
        , vp(owner)
    {}
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ViewProviderReviewNote* vp;
};

}  // namespace AssemblyGui
