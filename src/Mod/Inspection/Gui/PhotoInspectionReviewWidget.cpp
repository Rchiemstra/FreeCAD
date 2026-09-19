// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionReviewWidget.h"

#include <algorithm>
#include <cmath>

#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

namespace InspectionGui
{
namespace
{

QString text(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

}  // namespace

PhotoInspectionReviewWidget::PhotoInspectionReviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setAutoFillBackground(true);
    layerColors.insert(QStringLiteral("cad-boundary"), QColor(0, 120, 255));
    layerColors.insert(QStringLiteral("measured"), QColor(255, 70, 30));
    layerColors.insert(QStringLiteral("deviation"), QColor(220, 0, 170));
    layerColors.insert(QStringLiteral("markers"), QColor(0, 160, 80));
    layerColors.insert(QStringLiteral("references"), QColor(120, 80, 0));
}

void PhotoInspectionReviewWidget::setScene(const Inspection::Photo::VectorScene& value)
{
    scene = value;
    update();
}

void PhotoInspectionReviewWidget::setRectifiedImage(const Inspection::Photo::GrayRaster& raster)
{
    if (!raster.valid()) {
        clearRectifiedImage();
        return;
    }
    const QImage borrowed(
        raster.pixels.data(),
        raster.width,
        raster.height,
        raster.width,
        QImage::Format_Grayscale8
    );
    rectifiedImage = borrowed.copy();
    update();
}

void PhotoInspectionReviewWidget::clearRectifiedImage()
{
    rectifiedImage = {};
    update();
}

void PhotoInspectionReviewWidget::setLayerVisible(const QString& layer, const bool visible)
{
    layerVisibility.insert(layer, visible);
    update();
}

bool PhotoInspectionReviewWidget::isLayerVisible(const QString& layer) const
{
    return layerVisibility.value(layer, true);
}

void PhotoInspectionReviewWidget::setLayerColor(const QString& layer, const QColor& color)
{
    if (color.isValid()) {
        layerColors.insert(layer, color);
        update();
    }
}

void PhotoInspectionReviewWidget::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    if (!std::isfinite(scene.widthMm) || !std::isfinite(scene.heightMm) || scene.widthMm <= 0.0
        || scene.heightMm <= 0.0) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double scale = std::min(
        static_cast<double>(width()) / scene.widthMm,
        static_cast<double>(height()) / scene.heightMm
    );
    const double left = (width() - scene.widthMm * scale) * 0.5;
    const double top = (height() - scene.heightMm * scale) * 0.5;
    painter.translate(left, top);
    painter.scale(scale, scale);
    painter.fillRect(QRectF(0.0, 0.0, scene.widthMm, scene.heightMm), Qt::white);
    if (!rectifiedImage.isNull() && isLayerVisible(QStringLiteral("image"))) {
        painter.setOpacity(0.65);
        painter.drawImage(QRectF(0.0, 0.0, scene.widthMm, scene.heightMm), rectifiedImage);
        painter.setOpacity(1.0);
    }

    for (const Inspection::Photo::ScenePrimitive& primitive : scene.primitives) {
        const QString layer = text(primitive.layer);
        if (!isLayerVisible(layer) || primitive.points.empty()) {
            continue;
        }
        const QColor color = layerColors.value(layer, Qt::black);
        if (primitive.kind == Inspection::Photo::ScenePrimitiveKind::Text) {
            painter.setPen(color);
            QFont font = painter.font();
            font.setPointSizeF(3.0 * 72.0 / 25.4);
            painter.setFont(font);
            painter.drawText(
                QPointF(primitive.points.front().x, primitive.points.front().y),
                text(primitive.text)
            );
            continue;
        }
        QPainterPath path;
        path.moveTo(primitive.points.front().x, primitive.points.front().y);
        for (std::size_t index = 1; index < primitive.points.size(); ++index) {
            path.lineTo(primitive.points[index].x, primitive.points[index].y);
        }
        if (primitive.closed) {
            path.closeSubpath();
        }
        painter.setBrush(primitive.filled ? QBrush(color) : Qt::NoBrush);
        QPen pen(color);
        pen.setWidthF(primitive.strokeWidthMm > 0.0 ? primitive.strokeWidthMm : 1.0 / scale);
        pen.setCosmetic(false);
        painter.setPen(pen);
        painter.drawPath(path);
    }
}

}  // namespace InspectionGui
