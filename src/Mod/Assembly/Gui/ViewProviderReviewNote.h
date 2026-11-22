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
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Base/Placement.h>
#include <Base/Vector3D.h>
#include <Gui/ViewProviderAnnotation.h>
#include <fastsignals/signal.h>

#include <QObject>
#include <QPointer>
#include <QPoint>
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
    /// Show or hide the leader from the target to the note box.
    App::PropertyBool ShowLeader;
    /// Leader color, independent from the note background.
    App::PropertyColor LeaderColor;
    /// Leader thickness in screen pixels.
    App::PropertyFloatConstraint LeaderWidth;
    /// Leader stroke pattern (solid, dashed, dotted, or dash-dot).
    App::PropertyEnumeration LeaderLineStyle;
    /// Text-box border color.
    App::PropertyColor FrameColor;
    /// Text-box border thickness in raster pixels.
    App::PropertyFloatConstraint FrameWidth;
    /// Text-box border stroke pattern.
    App::PropertyEnumeration FrameLineStyle;
    /// Test-visible: pixel width of the last rasterized box image (capped at MaxBoxRasterPx).
    App::PropertyInteger RasterWidth;
    /// Test-visible: pixel height of the last rasterized box image (capped at MaxBoxRasterPx).
    App::PropertyInteger RasterHeight;
    /// Test-visible: number of times drawImage has rasterized the box (incremented per raster;
    /// used to verify resize-triggered re-raster).
    App::PropertyInteger DrawImageCount;
    /// Test-visible first @link hit rectangle as x,y,width,height raster pixels.
    App::PropertyString FirstLinkHitRect;

    QIcon getIcon() const override;
    bool doubleClicked() override;
    void attach(App::DocumentObject* obj) override;
    void finishRestoring() override;
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
    static Base::Vector3d autoBoundaryEndpoint(const Base::Vector3d& textPos, double halfW, double halfH);

protected:
    void onChanged(const App::Property* prop) override;
    Base::Vector3d worldToAnnotationPoint(const Base::Vector3d& world) const override;
    void drawImage(const std::vector<std::string>& lines) override;
    bool acceptLabelDragStart(SoDragger* drag, DragState& state) override;
    void onLabelClicked(const DragState& state) override;
    void setLeaderCoords(const Base::Vector3d& textPosition) override;

private:
    /// P2: Qt event filter that watches the active 3D-view GL widget for QEvent::Resize
    /// and schedules a coalesced visual frame so fixed-mm boxes re-rasterize and
    /// auto-sized notes refresh leader half-extents/endpoints at the new viewport scale.
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
    void selectReference(const RefHit& hit, const QPoint& pressPosition) const;
    void updateLeaderAppearance();
    void onLabelDragFinished(const DragState& state) override;

    BillboardFrame currentBillboardFrame(const Base::Vector3d& textPosition) const;
    Base::Vector3d billboardOffsetToLocal(
        const Base::Vector3d& uvOffset,
        const BillboardFrame& frame
    ) const;
    Base::Vector3d worldToAnnotationDirection(const Base::Vector3d& worldDir) const;
    Base::Vector3d textPositionWorld(const Base::Vector3d& textPosition) const;
    bool screenWorldPerPixel(const Base::Vector3d& textWorld, double& worldPerPixel) const;
    void ensureCameraSensor();
    void detachCameraSensor();
    /// P2: viewport resize changes worldPerPixel without modifying the Coin camera node
    /// (Coin has no viewport-region sensor), so fixed-mm boxes must re-rasterize and
    /// auto-sized notes must refresh leader half-extents/endpoints. Idempotently installs
    /// or moves the owned ResizeObserver event filter onto the active viewer's GL widget
    /// (and removes it from the previously watched live widget when the widget changes;
    /// with no active GL widget it keeps the current filter).
    void ensureViewportResizeObserver();
    void detachViewportResizeObserver();
    void detachFrameSensor();

    static void cameraSensorCallback(void* data, SoSensor* sensor);
    static void cameraSensorDeleteCallback(void* data, SoSensor* sensor);
    static void frameSensorCallback(void* data, SoSensor* sensor);

    std::vector<RefHit> refHits;
    SoNodeSensor* cameraSensor = nullptr;
    SoCamera* attachedCamera = nullptr;
    /// P2: owned Qt event filter (parentless QObject deleted in detachViewportResizeObserver)
    /// installed on attachedGlWidget so viewport resizes refresh fixed-mm rasters and
    /// auto-sized leader half-extents/endpoints.
    ResizeObserver* resizeObserver = nullptr;
    /// P2: guarded weak pointer to the watched GL widget. Auto-clears if Qt destroys or
    /// replaces the GL widget before the view provider, preventing dangling dereferences
    /// and allowing ensureViewportResizeObserver() to reinstall resizeObserver on the new widget.
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

    static const char* LeaderLineStyleEnums[];
    static const char* FrameLineStyleEnums[];
};

/// P2: Qt event filter installed on the active 3D-view GL widget and owned by
/// ViewProviderReviewNote (constructed with parent=nullptr so replacing or destroying
/// the GL widget does not destroy the observer; deleted in detachViewportResizeObserver).
/// Viewport resize changes worldPerPixel without touching the Coin camera node, so
/// without this both fixed-mm box rasters and auto-sized leader half-extents/endpoints
/// stay stale until the next camera or property change. On QEvent::Resize it schedules
/// a coalesced visual frame (flushScheduledVisualFrame re-rasterizes when hasFixedSize()
/// and refreshes leader geometry in both fixed-mm and auto-size modes).
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
