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

#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QObject>

#include <Inventor/SbLine.h>
#include <Inventor/SbPlane.h>
#include <Inventor/SbVec2s.h>
#include <Inventor/SbViewVolume.h>
#include <Inventor/SbViewportRegion.h>
#include <Inventor/draggers/SoDragger.h>
#include <Inventor/draggers/SoTranslate2Dragger.h>
#include <Inventor/events/SoEvent.h>
#include <Inventor/nodes/SoAnnotation.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoPickStyle.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoSphere.h>
#include <Inventor/nodes/SoTranslation.h>

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/GeoFeature.h>
#include <Base/Placement.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Tools.h>
#include <Gui/Utilities.h>

#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/ReviewNote.h>

#include "ViewProviderReviewNote.h"

using namespace AssemblyGui;

namespace
{

constexpr int TextPadding = 5;
constexpr qreal FrameWidth = 2.0;
constexpr qreal CornerRadius = 5.0;

const std::regex& refRegex()
{
    static const std::regex re(R"(@([A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)*))");
    return re;
}

bool projectPointerToPlane(
    SoDragger& drag,
    const Base::Vector3d& planePoint,
    const Base::Vector3d& planeNormal,
    Base::Vector3d& intersection
)
{
    const SoEvent* event = drag.getEvent();
    if (!event) {
        return false;
    }

    SbViewVolume viewVolume = drag.getViewVolume();
    SbLine pointerLine;
    viewVolume.projectPointToLine(event->getNormalizedPosition(drag.getViewportRegion()), pointerLine);

    SbVec3f point;
    const SbPlane dragPlane(
        Base::convertTo<SbVec3f>(planeNormal),
        Base::convertTo<SbVec3f>(planePoint)
    );
    if (!dragPlane.intersect(pointerLine, point)) {
        return false;
    }

    intersection = Base::convertTo<Base::Vector3d>(point);
    return true;
}

}  // namespace

PROPERTY_SOURCE(AssemblyGui::ViewProviderReviewNote, Gui::ViewProviderAnnotationLabel)

ViewProviderReviewNote::ViewProviderReviewNote() = default;

ViewProviderReviewNote::~ViewProviderReviewNote()
{
    if (portAnnotation) {
        portAnnotation->unref();
        portAnnotation = nullptr;
    }
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
    setupPortHandle();
    refreshLeaderAndPort();
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

    if (prop == &note->TextPosition || prop == &note->BasePosition || prop == &note->LeaderPort
        || prop == &note->LabelText) {
        if (!portDragging) {
            refreshLeaderAndPort();
        }
    }
}

void ViewProviderReviewNote::labelHalfExtents(double& halfW, double& halfH) const
{
    const double mmPerPx = std::max(0.05, FontSize.getValue() * 0.03);
    halfW = std::max(0.5, 0.5 * static_cast<double>(labelImageWidth) * mmPerPx);
    halfH = std::max(0.5, 0.5 * static_cast<double>(labelImageHeight) * mmPerPx);
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

    // Never leave the leader on a corner vertex — snap to the nearer side midpoint.
    const double eps = 1e-4 * std::min(w, h);
    if (std::fabs(std::fabs(offset.x) - w) < eps && std::fabs(std::fabs(offset.y) - h) < eps) {
        if (std::fabs(offset.x) >= std::fabs(offset.y)) {
            offset.y = 0.0;
            offset.x = (offset.x >= 0.0) ? w : -w;
        }
        else {
            offset.x = 0.0;
            offset.y = (offset.y >= 0.0) ? h : -h;
        }
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
    // Project to the rectangle boundary along the offset ray.
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
    const double w = std::max(1e-6, halfW);
    const double h = std::max(1e-6, halfH);
    // Attach to the midpoint of the box side facing the anchor — never a corner.
    if (textPos.Length() < 1e-9) {
        return Base::Vector3d(-w, 0.0, 0.0);
    }
    const Base::Vector3d towardBase = textPos * (-1.0);
    const double ax = std::fabs(towardBase.x) / w;
    const double ay = std::fabs(towardBase.y) / h;
    if (ax >= ay) {
        return textPos + Base::Vector3d(towardBase.x >= 0.0 ? w : -w, 0.0, 0.0);
    }
    return textPos + Base::Vector3d(0.0, towardBase.y >= 0.0 ? h : -h, 0.0);
}

Base::Vector3d ViewProviderReviewNote::leaderEndpoint(const Base::Vector3d& textPosition) const
{
    double halfW = 0.0;
    double halfH = 0.0;
    labelHalfExtents(halfW, halfH);

    auto* note = getObject<Assembly::ReviewNote>();
    if (!note || note->LeaderPort.getValue() < 0.0) {
        return autoBoundaryEndpoint(textPosition, halfW, halfH);
    }
    return textPosition + perimeterOffset(note->LeaderPort.getValue(), halfW, halfH);
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

void ViewProviderReviewNote::drawImage(const std::vector<std::string>& lines)
{
    refHits.clear();
    if (lines.empty()) {
        pImage->image = SoSFImage();
        pImageHitProxy->image = SoSFImage();
        labelImageWidth = 0;
        labelImageHeight = 0;
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

    QImage image(
        contentW + 2 * TextPadding,
        contentH + 2 * TextPadding,
        QImage::Format_ARGB32_Premultiplied
    );
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
        const int baselineY = TextPadding + fm.ascent() + row * fm.height();
        int x = TextPadding;
        if (Justification.getValue() == 1) {
            x = TextPadding + contentW - Gui::QtTools::horizontalAdvance(fm, line);
        }
        else if (Justification.getValue() == 2) {
            x = TextPadding + (contentW - Gui::QtTools::horizontalAdvance(fm, line)) / 2;
        }

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
            hit.pixelRect = QRect(x, TextPadding + row * fm.height(), linkW, fm.height());
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

    SoSFImage sfimage;
    Gui::BitmapFactory().convert(image, sfimage);
    pImage->image = sfimage;

    QImage hitProxy(image.size(), QImage::Format_ARGB32_Premultiplied);
    hitProxy.fill(Qt::transparent);
    SoSFImage sfHitProxy;
    Gui::BitmapFactory().convert(hitProxy, sfHitProxy);
    pImageHitProxy->image = sfHitProxy;

    refreshLeaderAndPort();
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

    const Base::Vector3d textWorld = [&]() {
        Base::Vector3d local = note->BasePosition.getValue() + note->TextPosition.getValue();
        if (auto* owner = note->getOwnerPart()) {
            Base::Vector3d world;
            App::GeoFeature::getGlobalPlacement(owner).multVec(local, world);
            return world;
        }
        return local;
    }();

    SbVec3f screen;
    volume.projectToScreen(Base::convertTo<SbVec3f>(textWorld), screen);
    // projectToScreen returns normalized [0,1]; convert to window pixels.
    const float winW = static_cast<float>(vp.getViewportSizePixels()[0]);
    const float winH = static_cast<float>(vp.getViewportSizePixels()[1]);
    const float cx = screen[0] * winW;
    // Inventor Y grows up; event position Y grows up from bottom in Coin.
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

void ViewProviderReviewNote::setupPortHandle()
{
    if (portAnnotation) {
        return;
    }

    portAnnotation = new SoAnnotation();
    portAnnotation->ref();

    auto* pick = new SoPickStyle();
    pick->style = SoPickStyle::SHAPE_ON_TOP;
    portAnnotation->addChild(pick);

    // Port lives under the same base translation as the leader.
    portAnnotation->addChild(pBaseTranslation);

    portTranslation = new SoTranslation();
    // Invisible pick volume on the box side — no yellow orb; leader ends on the face.
    portSphere = new SoSphere();
    portSphere->radius = 1.2f;
    auto* drawStyle = new SoDrawStyle();
    drawStyle->style = SoDrawStyle::INVISIBLE;

    auto* handle = new SoSeparator();
    handle->addChild(drawStyle);
    handle->addChild(portSphere);

    auto* dragger = new SoTranslate2Dragger();
    dragger->setPart("translator", handle);
    dragger->setPart("xAxisFeedback", new SoSeparator());
    dragger->setPart("yAxisFeedback", new SoSeparator());
    dragger->addStartCallback(portDragStartCallback, this);
    dragger->addMotionCallback(portDragMotionCallback, this);
    dragger->addFinishCallback(portDragFinishCallback, this);

    auto* manipSep = new SoSeparator();
    manipSep->addChild(portTranslation);
    manipSep->addChild(dragger);
    portAnnotation->addChild(manipSep);

    pcRoot->addChild(portAnnotation);
}

void ViewProviderReviewNote::updatePortHandle(const Base::Vector3d& textPos)
{
    if (!portTranslation) {
        return;
    }
    const Base::Vector3d end = leaderEndpoint(textPos);
    portTranslation->translation.setValue(end.x, end.y, end.z);

    double halfW = 0.0;
    double halfH = 0.0;
    labelHalfExtents(halfW, halfH);
    const float radius = static_cast<float>(std::max(0.6, 0.12 * std::min(halfW, halfH)));
    if (portSphere) {
        portSphere->radius = radius;
    }
}

void ViewProviderReviewNote::refreshLeaderAndPort()
{
    auto* note = getObject<Assembly::ReviewNote>();
    if (!note) {
        return;
    }
    const Base::Vector3d textPos = note->TextPosition.getValue();
    setLeaderCoords(textPos);
    updatePortHandle(textPos);
}

void ViewProviderReviewNote::portDragStartCallback(void* data, SoDragger* drag)
{
    auto* that = static_cast<ViewProviderReviewNote*>(data);
    that->portDragging = true;
    that->pendingPort = -1.0;
    that->portDragPlanePoint = Base::convertTo<Base::Vector3d>(drag->getWorldStartingPoint());
    that->portDragPlaneNormal = Base::convertTo<Base::Vector3d>(
        drag->getViewVolume().getProjectionDirection()
    );
    Gui::Application::Instance->activeDocument()->openCommand(
        QT_TRANSLATE_NOOP("Command", "Move Review Note Leader Port")
    );
}

void ViewProviderReviewNote::portDragMotionCallback(void* data, SoDragger* drag)
{
    auto* that = static_cast<ViewProviderReviewNote*>(data);
    if (!that->portDragging) {
        return;
    }
    auto* note = that->getObject<Assembly::ReviewNote>();
    if (!note) {
        return;
    }

    Base::Vector3d pointerWorld;
    if (!projectPointerToPlane(
            *drag,
            that->portDragPlanePoint,
            that->portDragPlaneNormal,
            pointerWorld
        )) {
        return;
    }

    const Base::Vector3d pointerLocal = that->worldToAnnotationPoint(pointerWorld);
    const Base::Vector3d textPos = note->TextPosition.getValue();
    const Base::Vector3d basePos = note->BasePosition.getValue();
    const Base::Vector3d offset = pointerLocal - (basePos + textPos);

    double halfW = 0.0;
    double halfH = 0.0;
    that->labelHalfExtents(halfW, halfH);
    that->pendingPort = perimeterParam(offset, halfW, halfH);
    const Base::Vector3d end = textPos + perimeterOffset(that->pendingPort, halfW, halfH);
    that->pCoords->point.set1Value(1, SbVec3f(end.x, end.y, end.z));
    if (that->portTranslation) {
        that->portTranslation->translation.setValue(end.x, end.y, end.z);
    }
}

void ViewProviderReviewNote::portDragFinishCallback(void* data, SoDragger*)
{
    auto* that = static_cast<ViewProviderReviewNote*>(data);
    auto* note = that->getObject<Assembly::ReviewNote>();
    if (note && that->pendingPort >= 0.0) {
        note->LeaderPort.setValue(that->pendingPort);
    }
    that->portDragging = false;
    that->pendingPort = -1.0;
    that->refreshLeaderAndPort();
    Gui::Application::Instance->activeDocument()->commitCommand();
}
