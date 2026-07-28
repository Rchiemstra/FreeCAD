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

#include <algorithm>
#include <cmath>
#include <regex>

#include <exception>

#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QEvent>
#include <QObject>
#include <QWidget>

#include <Inventor/SbRotation.h>
#include <Inventor/SbVec2s.h>
#include <Inventor/SbVec3f.h>
#include <Inventor/SbViewVolume.h>
#include <Inventor/SbViewportRegion.h>
#include <Inventor/SoRenderManager.h>
#include <Inventor/draggers/SoDragger.h>
#include <Inventor/events/SoEvent.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoImage.h>
#include <Inventor/sensors/SoOneShotSensor.h>
#include <Inventor/sensors/SoNodeSensor.h>
#include <Inventor/sensors/SoSensor.h>

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/GeoFeature.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Placement.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Tools.h>
#include <Gui/TranslateManip.h>
#include <Gui/Utilities.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>

#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/ReviewNote.h>

#include "ViewProviderReviewNote.h"
#include "ViewProviderReviewNoteTestHarness.h"

using namespace AssemblyGui;

namespace
{

constexpr int TextPadding = 5;
constexpr qreal FrameWidth = 2.0;
constexpr qreal CornerRadius = 5.0;
/// P1: cap (px) for the fixed-mm box raster so extreme zoom cannot exhaust memory
/// or overflow the 16-bit SbVec2s in BitmapFactory::convert. When the requested
/// mm size would exceed this, the raster is clamped and the leader follows the
/// clamped box (see currentBillboardFrame).
constexpr int MaxBoxRasterPx = 4096;

/// Coin delay-queue sensors sort ascending and processDelayQueue pops index 0, so a
/// *lower numeric* priority runs earlier. Redraw OneShot is typically 10000; the
/// visual-frame sensor must be redrawPri-1 (e.g. 9999) so LeaderEnd publishes before
/// that redraw. Priority 0 is reserved for immediate sensors (camera NodeSensor).
uint32_t visualFrameSensorPriority(const ViewProviderReviewNote* self)
{
    uint32_t redrawPri = SoRenderManager::getDefaultRedrawPriority();
    if (self) {
        if (const Gui::View3DInventorViewer* viewer = self->getActiveViewer()) {
            if (SoRenderManager* rm = viewer->getSoRenderManager()) {
                redrawPri = rm->getRedrawPriority();
            }
        }
    }
    if (redrawPri <= 1) {
        return 1;
    }
    return redrawPri - 1;
}

const std::regex& refRegex()
{
    static const std::regex re(R"(@([A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)*))");
    return re;
}

}  // namespace

PROPERTY_SOURCE(AssemblyGui::ViewProviderReviewNote, Gui::ViewProviderAnnotationLabel)

ViewProviderReviewNote::ViewProviderReviewNote()
{
    ADD_PROPERTY_TYPE(
        LeaderEnd,
        (Base::Vector3d()),
        "ReviewNote",
        App::Prop_Hidden,
        "Leader endpoint relative to BasePosition (nearest text-box border point)"
    );
    LeaderEnd.setStatus(App::Property::Output, true);
    LeaderEnd.setStatus(App::Property::ReadOnly, true);

    ADD_PROPERTY_TYPE(
        LeaderHalfExtent,
        (Base::Vector3d(0.5, 0.5, 0.0)),
        "ReviewNote",
        App::Prop_Hidden,
        "Billboard half-extents (x=halfW, y=halfH) of the text box used for leader attachment"
    );
    LeaderHalfExtent.setStatus(App::Property::Output, true);
    LeaderHalfExtent.setStatus(App::Property::ReadOnly, true);

    ADD_PROPERTY_TYPE(
        BoxWidth,
        (0.0),
        "ReviewNote",
        App::Prop_None,
        "Fixed box width in mm (0 = auto-size to text)"
    );
    ADD_PROPERTY_TYPE(
        BoxHeight,
        (0.0),
        "ReviewNote",
        App::Prop_None,
        "Fixed box height in mm (0 = auto-size to text)"
    );
    ADD_PROPERTY_TYPE(
        RasterWidth,
        (0),
        "ReviewNote",
        App::Prop_Hidden,
        "Test-visible: pixel width of the last rasterized box image (capped)"
    );
    RasterWidth.setStatus(App::Property::Output, true);
    RasterWidth.setStatus(App::Property::ReadOnly, true);
    ADD_PROPERTY_TYPE(
        RasterHeight,
        (0),
        "ReviewNote",
        App::Prop_Hidden,
        "Test-visible: pixel height of the last rasterized box image (capped)"
    );
    RasterHeight.setStatus(App::Property::Output, true);
    RasterHeight.setStatus(App::Property::ReadOnly, true);
    ADD_PROPERTY_TYPE(
        DrawImageCount,
        (0),
        "ReviewNote",
        App::Prop_Hidden,
        "Test-visible: count of drawImage rasterizations (verifies resize re-raster)"
    );
    DrawImageCount.setStatus(App::Property::Output, true);
    DrawImageCount.setStatus(App::Property::ReadOnly, true);
}

ViewProviderReviewNote::~ViewProviderReviewNote()
{
    syncLeaderConnection.disconnect();
    detachFrameSensor();
    detachCameraSensor();
    detachViewportResizeObserver();
}

QIcon ViewProviderReviewNote::getIcon() const
{
    auto* note = getObject<Assembly::ReviewNote>();
    if (!note) {
        return Gui::BitmapFactory().pixmap("Assembly_ReviewNote.svg");
    }

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

void ViewProviderReviewNote::attach(App::DocumentObject* obj)
{
    ViewProviderAnnotationLabel::attach(obj);
    // Coin SoImage defaults to LEFT/BOTTOM. Center the label on TextPosition so the
    // leader attaches to the visible box border (and @ref hit-tests stay consistent).
    if (pImage) {
        pImage->horAlignment = SoImage::CENTER;
        pImage->vertAlignment = SoImage::HALF;
    }
    if (pImageHitProxy) {
        pImageHitProxy->horAlignment = SoImage::CENTER;
        pImageHitProxy->vertAlignment = SoImage::HALF;
    }

    syncLeaderConnection.disconnect();
    if (obj && obj->isDerivedFrom(Assembly::ReviewNote::getClassTypeId())) {
        auto* note = static_cast<Assembly::ReviewNote*>(obj);
        // Runs from ReviewNote::onChanged(TextPosition) before document observers,
        // so LeaderEnd is already published when TextPosition is sampled.
        // FastSignals does not catch slot exceptions; contain them so App::onChanged
        // still runs, then schedule a clean retry.
        syncLeaderConnection = note->signalSyncLeaderVisual.connect(
            [this](const Base::Vector3d& textPos) {
                try {
                    applyVisualFrame(textPos, /*updateTextTranslation*/ true);
                }
                catch (const Base::Exception& e) {
#ifdef ASSEMBLY_ENABLE_TEST_HOOKS
                    ++ReviewNoteTestHarness::applyExceptionsCaught;
#endif
                    Base::Console().error(
                        "AssemblyGui::ViewProviderReviewNote: leader sync failed: %s\n",
                        e.what()
                    );
                    visualFrameDirty = true;
                    scheduleVisualFrame();
                }
                catch (const std::exception& e) {
#ifdef ASSEMBLY_ENABLE_TEST_HOOKS
                    ++ReviewNoteTestHarness::applyExceptionsCaught;
#endif
                    Base::Console().error(
                        "AssemblyGui::ViewProviderReviewNote: leader sync failed: %s\n",
                        e.what()
                    );
                    visualFrameDirty = true;
                    scheduleVisualFrame();
                }
                catch (...) {
#ifdef ASSEMBLY_ENABLE_TEST_HOOKS
                    ++ReviewNoteTestHarness::applyExceptionsCaught;
#endif
                    Base::Console().error(
                        "AssemblyGui::ViewProviderReviewNote: leader sync failed "
                        "(unknown exception)\n"
                    );
                    visualFrameDirty = true;
                    scheduleVisualFrame();
                }
            }
        );
    }

    ensureCameraSensor();
    refreshLeader();
}

void ViewProviderReviewNote::updateData(const App::Property* prop)
{
    auto* note = getObject<Assembly::ReviewNote>();
    if (!note || !prop) {
        ViewProviderAnnotationLabel::updateData(prop);
        return;
    }

    if (prop == &note->Resolved || prop == &note->AttachmentBroken || prop == &note->Target
        || prop == &note->JointSide || prop == &note->LabelText || prop == &note->BasePosition
        || prop == &note->TextPosition || prop == &note->LocalAnchor) {
        signalChangeIcon();
    }

    // TextPosition: skip base AnnotationLabel path (leader then translation) and
    // publish one atomic frame instead. signalSyncLeaderVisual usually already
    // published; refreshLeader is a cheap second apply with the same snapshot.
    if (prop == &note->TextPosition) {
        if (!applyingVisualFrame) {
            ensureCameraSensor();
            refreshLeader();
        }
        ViewProviderDocumentObject::updateData(prop);
        return;
    }

    ViewProviderAnnotationLabel::updateData(prop);

    if (applyingVisualFrame) {
        return;
    }

    if (prop == &note->BasePosition || prop == &note->LabelText) {
        ensureCameraSensor();
        refreshLeader();
    }
}

void ViewProviderReviewNote::onChanged(const App::Property* prop)
{
    ViewProviderAnnotationLabel::onChanged(prop);

    // Test-build only: observe LeaderHalfExtent during an in-flight apply and
    // mutate the camera so scheduleVisualFrame marks the nested dirty branch.
#ifdef ASSEMBLY_ENABLE_TEST_HOOKS
    if (prop == &LeaderHalfExtent && ReviewNoteTestHarness::injectNestedCamera > 0
        && applyingVisualFrame && attachedCamera) {
        --ReviewNoteTestHarness::injectNestedCamera;
        const SbRotation cur = attachedCamera->orientation.getValue();
        attachedCamera->orientation.setValue(
            cur * SbRotation(SbVec3f(0.2f, 0.9f, 0.1f), 0.45f)
        );
    }
#endif

    if (applyingVisualFrame) {
        return;
    }
    if (prop == &BoxWidth || prop == &BoxHeight) {
        // P3: millimetre lengths must be non-negative; clamp negative input to 0 (auto).
        if (!updatingBoxSize) {
            if (BoxWidth.getValue() < 0.0) {
                updatingBoxSize = true;
                BoxWidth.setValue(0.0);
                updatingBoxSize = false;
            }
            if (BoxHeight.getValue() < 0.0) {
                updatingBoxSize = true;
                BoxHeight.setValue(0.0);
                updatingBoxSize = false;
            }
        }
        // Re-render the image at the new fixed/auto size; drawImage ends with refreshLeader.
        if (auto* note = getObject<Assembly::ReviewNote>()) {
            drawImage(note->LabelText.getValues());
        }
        else {
            ensureCameraSensor();
            scheduleVisualFrame();
        }
        return;
    }
    if (prop == &FontSize || prop == &FontName || prop == &Frame || prop == &Justification
        || prop == &TextColor || prop == &BackgroundColor) {
        ensureCameraSensor();
        scheduleVisualFrame();
    }
}

void ViewProviderReviewNote::setLeaderCoords(const Base::Vector3d& textPosition)
{
    // Drag preview and property paths share one atomic apply: Coin line + LeaderEnd
    // + optional text translation from the same position/camera snapshot.
    applyVisualFrame(textPosition, /*updateTextTranslation*/ false);
}

void ViewProviderReviewNote::applyVisualFrame(
    const Base::Vector3d& textPosition,
    bool updateTextTranslation
)
{
    if (applyingVisualFrame) {
        // Nested request while publishing a frame — coalesce to a follow-up idle.
        visualFrameDirty = true;
        return;
    }
    applyingVisualFrame = true;
    visualFrameDirty = false;

    // Compute endpoint once from current camera/billboard, then write Coin and
    // properties together so no observer/render can see new text with a stale end.
    // Order: text transform first, then leader geometry/properties (matches drag
    // preview). Inverse order would briefly show a new leader on the old box.
    struct Guard
    {
        bool& flag;
        ViewProviderReviewNote* self;
        ~Guard()
        {
            flag = false;
            if (self->visualFrameDirty) {
                self->scheduleVisualFrame();
            }
        }
    } guard {applyingVisualFrame, this};

    const Base::Vector3d end = leaderEndpoint(textPosition);
    const BillboardFrame frame = currentBillboardFrame(textPosition);

    if (updateTextTranslation && pTextTranslation) {
        pTextTranslation->translation.setValue(textPosition.x, textPosition.y, textPosition.z);
    }
    if (pCoords) {
        pCoords->point.set1Value(0, SbVec3f(0.0f, 0.0f, 0.0f));
        pCoords->point.set1Value(1, SbVec3f(end.x, end.y, end.z));
    }

    // Test-build only: throw after Coin writes, before LeaderEnd publishes.
#ifdef ASSEMBLY_ENABLE_TEST_HOOKS
    if (ReviewNoteTestHarness::injectThrowAfterCoords > 0) {
        --ReviewNoteTestHarness::injectThrowAfterCoords;
        throw Base::RuntimeError(
            "ReviewNote test inject: throw after translation/pCoords before LeaderEnd"
        );
    }
#endif

    LeaderHalfExtent.setValue(Base::Vector3d(frame.halfW, frame.halfH, 0.0));
    LeaderEnd.setValue(end);
}

void ViewProviderReviewNote::scheduleVisualFrame()
{
    visualFrameDirty = true;
    if (applyingVisualFrame) {
        // applyVisualFrame's Guard will reschedule when the current publish ends.
#ifdef ASSEMBLY_ENABLE_TEST_HOOKS
        ++ReviewNoteTestHarness::nestedDirtyMarked;
#endif
        return;
    }
    if (!frameSensor) {
        frameSensor = new SoOneShotSensor(frameSensorCallback, this);
    }
    // Re-apply each schedule: active render-manager redraw priority can change.
    frameSensor->setPriority(visualFrameSensorPriority(this));
    if (!frameSensor->isScheduled()) {
        visualFrameScheduled = true;
        frameSensor->schedule();
    }
}

void ViewProviderReviewNote::flushScheduledVisualFrame()
{
    visualFrameScheduled = false;
    visualFrameDirty = false;
    // A fixed-mm box must be re-rasterized when the zoom changes (mm -> px), otherwise
    // the visible box drifts away from the leader border. drawImage ends with
    // refreshLeader, so the leader stays glued to the new box in one atomic pass.
    if (hasFixedSize()) {
        if (auto* note = getObject<Assembly::ReviewNote>()) {
            drawImage(note->LabelText.getValues());
            return;
        }
    }
    refreshLeader();
}

void ViewProviderReviewNote::frameSensorCallback(void* data, SoSensor*)
{
    auto* that = static_cast<ViewProviderReviewNote*>(data);
    if (!that) {
        return;
    }
    that->flushScheduledVisualFrame();
}

void ViewProviderReviewNote::detachFrameSensor()
{
    if (frameSensor) {
        if (frameSensor->isScheduled()) {
            frameSensor->unschedule();
        }
        delete frameSensor;
        frameSensor = nullptr;
    }
    visualFrameScheduled = false;
    visualFrameDirty = false;
}

void ViewProviderReviewNote::labelHalfExtents(double& halfW, double& halfH) const
{
    const BillboardFrame frame = currentBillboardFrame(
        getObject<Assembly::ReviewNote>() ? getObject<Assembly::ReviewNote>()->TextPosition.getValue()
                                          : Base::Vector3d()
    );
    halfW = frame.halfW;
    halfH = frame.halfH;
}

bool ViewProviderReviewNote::hasFixedSize() const
{
    return BoxWidth.getValue() > 0.0 || BoxHeight.getValue() > 0.0;
}

Base::Vector3d ViewProviderReviewNote::perimeterOffset(double port, double halfW, double halfH)
{
    const double w = std::max(1e-6, halfW);
    const double h = std::max(1e-6, halfH);
    const double peri = 2.0 * (2.0 * w + 2.0 * h);
    double d = std::fmod(port, 1.0);
    if (d < 0.0) {
        d += 1.0;
    }
    d *= peri;

    const double right = 2.0 * h;
    const double bottom = right + 2.0 * w;
    const double left = bottom + 2.0 * h;

    Base::Vector3d offset;
    if (d <= right) {
        offset = Base::Vector3d(w, h - d, 0.0);
    }
    else if (d <= bottom) {
        offset = Base::Vector3d(w - (d - right), -h, 0.0);
    }
    else if (d <= left) {
        offset = Base::Vector3d(-w, -h + (d - bottom), 0.0);
    }
    else {
        offset = Base::Vector3d(-w + (d - left), h, 0.0);
    }
    return offset;
}

double ViewProviderReviewNote::perimeterParam(
    const Base::Vector3d& offset,
    double halfW,
    double halfH
)
{
    const double w = std::max(1e-6, halfW);
    const double h = std::max(1e-6, halfH);
    Base::Vector3d dir(offset.x, offset.y, 0.0);
    if (dir.Length() < 1e-9) {
        dir = Base::Vector3d(w, 0.0, 0.0);
    }
    const double sx = (std::fabs(dir.x) > 1e-12) ? (w / std::fabs(dir.x)) : 1e12;
    const double sy = (std::fabs(dir.y) > 1e-12) ? (h / std::fabs(dir.y)) : 1e12;
    const double t = std::min(sx, sy);
    const Base::Vector3d p = dir * t;

    const double peri = 2.0 * (2.0 * w + 2.0 * h);
    double dist = 0.0;
    if (std::fabs(p.x - w) < 1e-6) {
        dist = (h - p.y);
    }
    else if (std::fabs(p.y + h) < 1e-6) {
        dist = 2.0 * h + (w - p.x);
    }
    else if (std::fabs(p.x + w) < 1e-6) {
        dist = 2.0 * h + 2.0 * w + (p.y + h);
    }
    else {
        dist = 2.0 * h + 2.0 * w + 2.0 * h + (p.x + w);
    }
    return std::fmod(std::max(0.0, dist), peri) / peri;
}

Base::Vector3d ViewProviderReviewNote::autoBoundaryEndpoint(
    const Base::Vector3d& textPos,
    double halfW,
    double halfH
)
{
    // Nearest border point on the axis-aligned box along the line toward the base.
    // That is the exit point of the leader from the text box — no gap, no through-text.
    const double w = std::max(1e-6, halfW);
    const double h = std::max(1e-6, halfH);
    if (textPos.Length() < 1e-9) {
        return Base::Vector3d(-w, 0.0, 0.0);
    }
    const Base::Vector3d towardBase = textPos * (-1.0);
    Base::Vector3d dir(towardBase.x, towardBase.y, 0.0);
    if (dir.Length() < 1e-9) {
        return textPos + Base::Vector3d(-w, 0.0, 0.0);
    }
    const double sx = (std::fabs(dir.x) > 1e-12) ? (w / std::fabs(dir.x)) : 1e12;
    const double sy = (std::fabs(dir.y) > 1e-12) ? (h / std::fabs(dir.y)) : 1e12;
    const double t = std::min(sx, sy);
    return textPos + dir * t;
}

Base::Vector3d ViewProviderReviewNote::billboardOffsetToLocal(
    const Base::Vector3d& uvOffset,
    const BillboardFrame& frame
) const
{
    return frame.right * uvOffset.x + frame.up * uvOffset.y;
}

Base::Vector3d ViewProviderReviewNote::leaderEndpoint(const Base::Vector3d& textPosition) const
{
    const BillboardFrame frame = currentBillboardFrame(textPosition);
    const double w = frame.halfW;
    const double h = frame.halfH;

    // Project the anchor direction into the billboard plane, then clip to the box border.
    const Base::Vector3d towardBase = textPosition * (-1.0);
    double tu = towardBase * frame.right;
    double tv = towardBase * frame.up;
    if (std::fabs(tu) < 1e-12 && std::fabs(tv) < 1e-12) {
        tu = -1.0;
        tv = 0.0;
    }
    const double sx = (std::fabs(tu) > 1e-12) ? (w / std::fabs(tu)) : 1e12;
    const double sy = (std::fabs(tv) > 1e-12) ? (h / std::fabs(tv)) : 1e12;
    const double t = std::min(sx, sy);
    const Base::Vector3d uv(tu * t, tv * t, 0.0);
    return textPosition + billboardOffsetToLocal(uv, frame);
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

Base::Vector3d ViewProviderReviewNote::worldToAnnotationDirection(const Base::Vector3d& worldDir) const
{
    auto* note = getObject<Assembly::ReviewNote>();
    if (!note) {
        return worldDir;
    }
    if (auto* owner = note->getOwnerPart()) {
        return App::GeoFeature::getGlobalPlacement(owner).getRotation().inverse().multVec(worldDir);
    }
    return worldDir;
}

Base::Vector3d ViewProviderReviewNote::textPositionWorld(const Base::Vector3d& textPosition) const
{
    auto* note = getObject<Assembly::ReviewNote>();
    if (!note) {
        return textPosition;
    }
    Base::Vector3d local = note->BasePosition.getValue() + textPosition;
    if (auto* owner = note->getOwnerPart()) {
        Base::Vector3d world;
        App::GeoFeature::getGlobalPlacement(owner).multVec(local, world);
        return world;
    }
    return local;
}

bool ViewProviderReviewNote::screenWorldPerPixel(
    const Base::Vector3d& textWorld,
    double& worldPerPixel
) const
{
    const Gui::View3DInventorViewer* viewer = getActiveViewer();
    SoCamera* camera = attachedCamera;
    int vpWidth = lastViewportWidthPx;
    float aspect = 1.0f;

    if (viewer && viewer->getSoRenderManager()) {
        SoRenderManager* rm = viewer->getSoRenderManager();
        if (!camera) {
            camera = rm->getCamera();
        }
        const SbViewportRegion& vp = rm->getViewportRegion();
        const SbVec2s vpSize = vp.getViewportSizePixels();
        if (vpSize[0] > 0) {
            vpWidth = vpSize[0];
            aspect = vp.getViewportAspectRatio();
        }
    }

    if (!camera || vpWidth <= 0) {
        return false;
    }

    const SbViewVolume volume = camera->getViewVolume(aspect);
    // Coin: getWorldToScreenScale(..., 1) ≈ near-plane width in world units; / width → world/px.
    const float scale = volume.getWorldToScreenScale(Base::convertTo<SbVec3f>(textWorld), 1.0f);
    worldPerPixel = static_cast<double>(scale) / static_cast<double>(vpWidth);
    return worldPerPixel > 1e-12;
}

ViewProviderReviewNote::BillboardFrame ViewProviderReviewNote::currentBillboardFrame(
    const Base::Vector3d& textPosition
) const
{
    BillboardFrame frame;
    const Base::Vector3d textWorld = textPositionWorld(textPosition);

    double worldPerPixel = 0.0;
    if (screenWorldPerPixel(textWorld, worldPerPixel) && labelImageWidth > 0 && labelImageHeight > 0) {
        // Exact screen-pixel mapping — do not floor to 0.5 mm (that overshoots the
        // visible border when zoomed out and makes the leader look disconnected).
        frame.halfW = std::max(1e-6, 0.5 * static_cast<double>(labelImageWidth) * worldPerPixel);
        frame.halfH = std::max(1e-6, 0.5 * static_cast<double>(labelImageHeight) * worldPerPixel);
        frame.valid = true;
        // Cache for transient viewer/camera lookup failures during orbit sensors.
        lastHalfW = frame.halfW;
        lastHalfH = frame.halfH;
        hasLastHalfExtent = true;
        if (const Gui::View3DInventorViewer* viewer = getActiveViewer()) {
            if (viewer->getSoRenderManager()) {
                const SbVec2s vpSize =
                    viewer->getSoRenderManager()->getViewportRegion().getViewportSizePixels();
                if (vpSize[0] > 0) {
                    lastViewportWidthPx = vpSize[0];
                }
            }
        }
    }
    else if (hasLastHalfExtent) {
        // Prefer last good camera scale over FontSize*bitmap — that fallback was ~5–10×
        // too large and made the leader look detached from the screen-space box.
        frame.halfW = lastHalfW;
        frame.halfH = lastHalfH;
    }
    else {
        // Cold start only: font-sized box, not full bitmap × bogus mm/px.
        const double h = std::max(1e-6, FontSize.getValue() * 0.25);
        frame.halfH = h;
        if (labelImageWidth > 0 && labelImageHeight > 0) {
            frame.halfW = h * static_cast<double>(labelImageWidth)
                / static_cast<double>(labelImageHeight);
        }
        else {
            frame.halfW = h * 1.6;
        }
    }

    // Prefer the sensor-attached camera during orbit so axes stay in sync with scale.
    SoCamera* camera = attachedCamera;
    if (!camera) {
        const Gui::View3DInventorViewer* viewer = getActiveViewer();
        if (viewer && viewer->getSoRenderManager()) {
            camera = viewer->getSoRenderManager()->getCamera();
        }
    }
    if (camera) {
        SbRotation orient = camera->orientation.getValue();
        SbVec3f right(1.0f, 0.0f, 0.0f);
        SbVec3f up(0.0f, 1.0f, 0.0f);
        orient.multVec(right, right);
        orient.multVec(up, up);
        frame.right = worldToAnnotationDirection(Base::convertTo<Base::Vector3d>(right));
        frame.up = worldToAnnotationDirection(Base::convertTo<Base::Vector3d>(up));
        if (frame.right.Length() > 1e-9) {
            frame.right.Normalize();
        }
        else {
            frame.right = Base::Vector3d(1.0, 0.0, 0.0);
        }
        // Keep up orthogonal to right in annotation space.
        frame.up = frame.up - frame.right * (frame.up * frame.right);
        if (frame.up.Length() > 1e-9) {
            frame.up.Normalize();
        }
        else {
            frame.up = Base::Vector3d(0.0, 1.0, 0.0);
        }
    }
    // Keep the raster-derived half-extents for fixed boxes too. The requested mm
    // dimensions are minimums: the text can require a larger raster, ceil() can add
    // a partial pixel, and the safety cap can make it smaller. BoxWidth/2 therefore
    // is not necessarily the visible border; the rendered raster is the source of truth.
    return frame;
}

void ViewProviderReviewNote::drawImage(const std::vector<std::string>& lines)
{
    refHits.clear();
    if (lines.empty()) {
        pImage->image = SoSFImage();
        pImageHitProxy->image = SoSFImage();
        labelImageWidth = 0;
        labelImageHeight = 0;
        RasterWidth.setValue(0);
        RasterHeight.setValue(0);
        this->hide();
        return;
    }

    QFont font(QString::fromLatin1(this->FontName.getValue()), (int)this->FontSize.getValue());
    QFontMetrics fm(font);
    int contentW = 0;
    int contentH = fm.height() * static_cast<int>(lines.size());
    const Base::Color& b = this->BackgroundColor.getValue();
    QColor brush;
    brush.setRgbF(b.r, b.g, b.b);
    const Base::Color& t = this->TextColor.getValue();
    QColor front;
    front.setRgbF(t.r, t.g, t.b);
    const QColor linkColor(80, 160, 255);

    QStringList qlines;
    for (const auto& it : lines) {
        QString line = QString::fromUtf8(it.c_str());
        contentW = std::max<int>(contentW, Gui::QtTools::horizontalAdvance(fm, line));
        qlines << line;
    }

    // Auto box size (pixels) fits the text. A user-set BoxWidth/BoxHeight (mm) grows
    // the box to at least that mm size at the current zoom (mm -> px via worldPerPixel),
    // so the visible box and the leader-attachment border stay glued.
    int autoW = contentW + 2 * TextPadding;
    int autoH = contentH + 2 * TextPadding;
    int targetW = autoW;
    int targetH = autoH;
    if (hasFixedSize()) {
        double worldPerPixel = 0.0;
        if (auto* note = getObject<Assembly::ReviewNote>()) {
            const Base::Vector3d textWorld = textPositionWorld(note->TextPosition.getValue());
            screenWorldPerPixel(textWorld, worldPerPixel);
        }
        if (worldPerPixel > 1e-12) {
            // P1: bound the raster so extreme zoom cannot exhaust memory or overflow
            // the 16-bit SbVec2s downstream (BitmapFactory::convert). Clamp the
            // requested size as a DOUBLE before converting to int: a value above
            // INT_MAX would overflow static_cast<int> before the cap could help.
            // Positive overflow maps to the cap rather than back to auto-size; NaN,
            // negative, and otherwise invalid requests are ignored. The leader
            // half-extents always follow the rendered raster.
            auto clampRasterPx = [](double mm, double wpp) -> int {
                if (!(mm > 0.0) || !(wpp > 0.0) || !std::isfinite(wpp)) {
                    return 0;
                }
                double requested = mm / wpp;
                if (std::isinf(requested) && requested > 0.0) {
                    return MaxBoxRasterPx;
                }
                if (!std::isfinite(requested) || requested <= 0.0) {
                    return 0;
                }
                double clamped = std::min(std::ceil(requested), static_cast<double>(MaxBoxRasterPx));
                return static_cast<int>(clamped);
            };
            if (BoxWidth.getValue() > 0.0) {
                targetW = std::max(targetW, clampRasterPx(BoxWidth.getValue(), worldPerPixel));
            }
            if (BoxHeight.getValue() > 0.0) {
                targetH = std::max(targetH, clampRasterPx(BoxHeight.getValue(), worldPerPixel));
            }
        }
    }
    // Center the text block inside the (possibly larger) box.
    const int xOffset = (targetW - autoW) / 2;
    const int yOffset = (targetH - autoH) / 2;

    QImage image(
        targetW,
        targetH,
        QImage::Format_ARGB32_Premultiplied
    );
    // P1: if allocation failed (e.g. huge requested dims before clamping took effect
    // on some path), fall back to the auto text-sized box instead of feeding a null
    // QImage into BitmapFactory::convert (which would overflow/divide-by-zero).
    if (image.isNull()) {
        image = QImage(autoW, autoH, QImage::Format_ARGB32_Premultiplied);
    }
    image.fill(0x00000000);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    if (this->Frame.getValue()) {
        painter.setPen(
            QPen(QColor(0, 0, 127), FrameWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
        );
        painter.setBrush(QBrush(brush, Qt::SolidPattern));
        const QRectF rectangle = QRectF(image.rect())
                                     .adjusted(
                                         FrameWidth / 2.0,
                                         FrameWidth / 2.0,
                                         -FrameWidth / 2.0,
                                         -FrameWidth / 2.0
                                     );
        painter.drawRoundedRect(rectangle, CornerRadius, CornerRadius);
    }

    painter.setFont(font);
    for (int row = 0; row < qlines.size(); ++row) {
        const QString& line = qlines.at(row);
        const int baselineY = TextPadding + yOffset + fm.ascent() + row * fm.height();
        int x = TextPadding;
        if (Justification.getValue() == 1) {
            x = TextPadding + contentW - Gui::QtTools::horizontalAdvance(fm, line);
        }
        else if (Justification.getValue() == 2) {
            x = TextPadding + (contentW - Gui::QtTools::horizontalAdvance(fm, line)) / 2;
        }
        x += xOffset;

        const std::string utf8 = line.toUtf8().constData();
        std::sregex_iterator it(utf8.begin(), utf8.end(), refRegex());
        std::sregex_iterator end;
        size_t cursor = 0;
        while (it != end) {
            const std::smatch match = *it;
            const size_t start = static_cast<size_t>(match.position());
            const size_t mlen = static_cast<size_t>(match.length());
            if (start > cursor) {
                const QString plain = QString::fromUtf8(
                    utf8.substr(cursor, start - cursor).c_str()
                );
                painter.setPen(front);
                painter.drawText(x, baselineY, plain);
                x += Gui::QtTools::horizontalAdvance(fm, plain);
            }
            const QString link = QString::fromUtf8(match.str().c_str());
            const int linkW = Gui::QtTools::horizontalAdvance(fm, link);
            painter.setPen(linkColor);
            painter.drawText(x, baselineY, link);
            painter.drawLine(x, baselineY + 1, x + linkW, baselineY + 1);

            RefHit hit;
            hit.pixelRect = QRect(x, TextPadding + yOffset + row * fm.height(), linkW, fm.height());
            const std::string full = match[1].str();
            const auto dot = full.find('.');
            if (dot == std::string::npos) {
                hit.objName = full;
            }
            else {
                hit.objName = full.substr(0, dot);
                hit.subName = full.substr(dot + 1);
            }
            refHits.push_back(hit);

            x += linkW;
            cursor = start + mlen;
            ++it;
        }
        if (cursor < utf8.size()) {
            const QString plain = QString::fromUtf8(utf8.substr(cursor).c_str());
            painter.setPen(front);
            painter.drawText(x, baselineY, plain);
        }
    }
    painter.end();

    labelImageWidth = image.width();
    labelImageHeight = image.height();
    // P1/P2 test-visible: publish the rendered raster dims (already capped above) and
    // count this rasterization so tests can verify the cap and a resize re-raster.
    RasterWidth.setValue(image.width());
    RasterHeight.setValue(image.height());
    DrawImageCount.setValue(DrawImageCount.getValue() + 1);

    SoSFImage sfimage;
    Gui::BitmapFactory().convert(image, sfimage);
    pImage->image = sfimage;

    QImage hitProxy(image.size(), QImage::Format_ARGB32_Premultiplied);
    hitProxy.fill(Qt::transparent);
    SoSFImage sfHitProxy;
    Gui::BitmapFactory().convert(hitProxy, sfHitProxy);
    pImageHitProxy->image = sfHitProxy;

    refreshLeader();
}

bool ViewProviderReviewNote::hitTestReference(SoDragger* drag, RefHit& out) const
{
    if (!drag || refHits.empty() || labelImageWidth <= 0 || labelImageHeight <= 0) {
        return false;
    }
    const SoEvent* event = drag->getEvent();
    if (!event) {
        return false;
    }

    auto* note = getObject<Assembly::ReviewNote>();
    if (!note) {
        return false;
    }

    const SbViewportRegion& vp = drag->getViewportRegion();
    const SbVec2s cursor = event->getPosition();
    const SbViewVolume volume = drag->getViewVolume();

    const Base::Vector3d textWorld = textPositionWorld(note->TextPosition.getValue());

    SbVec3f screen;
    volume.projectToScreen(Base::convertTo<SbVec3f>(textWorld), screen);
    const float winW = static_cast<float>(vp.getViewportSizePixels()[0]);
    const float winH = static_cast<float>(vp.getViewportSizePixels()[1]);
    const float cx = screen[0] * winW;
    const float cy = screen[1] * winH;

    const float imgX = static_cast<float>(cursor[0]) - (cx - 0.5f * labelImageWidth);
    const float imgYFromTop = (cy + 0.5f * labelImageHeight) - static_cast<float>(cursor[1]);

    const QPoint pixel(static_cast<int>(std::lround(imgX)), static_cast<int>(std::lround(imgYFromTop)));
    for (const auto& hit : refHits) {
        if (hit.pixelRect.contains(pixel)) {
            out = hit;
            return true;
        }
    }
    return false;
}

void ViewProviderReviewNote::selectReference(const RefHit& hit) const
{
    auto* note = getObject<Assembly::ReviewNote>();
    if (!note || !note->getDocument()) {
        return;
    }
    App::DocumentObject* obj = note->getDocument()->getObject(hit.objName.c_str());
    if (!obj) {
        return;
    }

    Gui::Selection().clearSelection();
    if (hit.subName.empty()) {
        Gui::Selection().addSelection(note->getDocument()->getName(), obj->getNameInDocument());
    }
    else {
        Gui::Selection().addSelection(
            note->getDocument()->getName(),
            obj->getNameInDocument(),
            hit.subName.c_str()
        );
    }
}

bool ViewProviderReviewNote::acceptLabelDragStart(SoDragger* drag, DragState& state)
{
    (void)state;
    RefHit hit;
    if (hitTestReference(drag, hit)) {
        selectReference(hit);
        return false;
    }
    return true;
}

void ViewProviderReviewNote::onLabelDragFinished(const DragState& state)
{
    // Runs before TextPosition is committed. Publish Coin translation + LeaderEnd
    // from the finished drag position so the TextPosition notification never
    // exposes a new box with the previous leader endpoint.
    applyVisualFrame(state.currentTextPosition, /*updateTextTranslation*/ true);
}

void ViewProviderReviewNote::refreshLeader()
{
    auto* note = getObject<Assembly::ReviewNote>();
    if (!note) {
        return;
    }
    ensureCameraSensor();

    // During an active label drag, TextPosition is not committed yet — use the
    // preview position and keep pTextTranslation owned by the drag preview path
    // (applyVisualFrame with updateTextTranslation=false).
    if (dragState) {
        applyVisualFrame(dragState->currentTextPosition, /*updateTextTranslation*/ false);
        return;
    }

    applyVisualFrame(note->TextPosition.getValue(), /*updateTextTranslation*/ true);
}

void ViewProviderReviewNote::detachCameraSensor()
{
    if (cameraSensor) {
        cameraSensor->detach();
        delete cameraSensor;
        cameraSensor = nullptr;
    }
    attachedCamera = nullptr;
}

void ViewProviderReviewNote::ensureViewportResizeObserver()
{
    QWidget* gl = nullptr;
    if (const Gui::View3DInventorViewer* viewer = getActiveViewer()) {
        gl = viewer->getGLWidget();
    }
    // Already installed on this exact widget (or there is no widget yet). QPointer
    // auto-clears if the previous widget was destroyed, so a stale pointer reads null.
    if (gl == attachedGlWidget.data() && resizeObserver) {
        return;
    }
    // Widget changed (or first time, or previous widget destroyed): move the filter.
    if (resizeObserver && !attachedGlWidget.isNull()) {
        attachedGlWidget->removeEventFilter(resizeObserver);
    }
    if (!resizeObserver) {
        resizeObserver = new ResizeObserver(this);
    }
    if (gl) {
        gl->installEventFilter(resizeObserver);
    }
    attachedGlWidget = gl;
}

void ViewProviderReviewNote::detachViewportResizeObserver()
{
    if (!resizeObserver) {
        return;
    }
    // QPointer clears automatically if Qt already destroyed the GL widget, so this
    // removeEventFilter only runs when the widget is still alive (no dangling deref).
    if (!attachedGlWidget.isNull()) {
        attachedGlWidget->removeEventFilter(resizeObserver);
    }
    delete resizeObserver;
    resizeObserver = nullptr;
    attachedGlWidget = nullptr;
}

bool ViewProviderReviewNote::ResizeObserver::eventFilter(QObject* /*watched*/, QEvent* event)
{
    // P2: viewport resize changes worldPerPixel; re-rasterize a fixed-mm box so the
    // visible box and leader stay glued. Auto-size notes are viewport-independent
    // and need no re-raster, so skip the schedule to avoid needless work.
    if (event->type() == QEvent::Resize && vp && vp->hasFixedSize()) {
        vp->scheduleVisualFrame();
    }
    return false;  // never consume the event
}

void ViewProviderReviewNote::ensureCameraSensor()
{
    const Gui::View3DInventorViewer* viewer = getActiveViewer();
    if (!viewer || !viewer->getSoRenderManager()) {
        return;
    }
    SoCamera* camera = viewer->getSoRenderManager()->getCamera();
    if (!camera) {
        return;
    }
    if (camera == attachedCamera && cameraSensor) {
        return;
    }

    if (!cameraSensor) {
        cameraSensor = new SoNodeSensor(cameraSensorCallback, this);
        cameraSensor->setDeleteCallback(cameraSensorDeleteCallback, this);
        // Immediate: nested camera mutations during applyVisualFrame mark dirty in
        // the same stack, so the Guard can reschedule a follow-up frame.
        cameraSensor->setPriority(0);
    }
    else {
        cameraSensor->detach();
    }
    cameraSensor->attach(camera);
    attachedCamera = camera;
    // P2: also watch the GL widget for viewport resize (changes worldPerPixel but
    // not the camera node), so a fixed-mm box re-rasterizes on view resize.
    ensureViewportResizeObserver();
}

void ViewProviderReviewNote::cameraSensorCallback(void* data, SoSensor*)
{
    auto* that = static_cast<ViewProviderReviewNote*>(data);
    if (!that) {
        return;
    }
    // Coalesce camera-driven refreshes into one OneShot at redrawPri-1 so it
    // runs on the non-idle delay path before the redraw sensor (redrawPri).
    // Mid-apply calls mark dirty only.
    that->scheduleVisualFrame();
}

void ViewProviderReviewNote::cameraSensorDeleteCallback(void* data, SoSensor* sensor)
{
    auto* that = static_cast<ViewProviderReviewNote*>(data);
    if (!that) {
        return;
    }
    that->attachedCamera = nullptr;
    // Camera was replaced (e.g. ortho ↔ perspective); reattach to the new one.
    auto* nodeSensor = static_cast<SoNodeSensor*>(sensor);
    const Gui::View3DInventorViewer* viewer = that->getActiveViewer();
    if (viewer && viewer->getSoRenderManager()) {
        if (SoCamera* camera = viewer->getSoRenderManager()->getCamera()) {
            nodeSensor->attach(camera);
            that->attachedCamera = camera;
            that->scheduleVisualFrame();
            return;
        }
    }
}
