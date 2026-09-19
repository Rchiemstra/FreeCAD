// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionRenderer.h"

#include <cmath>

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>

namespace InspectionGui
{

PdfRenderResult renderPhotoInspectionPdf(const Inspection::Photo::VectorScene& scene)
{
    PdfRenderResult result;
    if (!std::isfinite(scene.widthMm) || !std::isfinite(scene.heightMm) || scene.widthMm <= 0.0
        || scene.heightMm <= 0.0 || scene.primitives.size() > 250000) {
        result.error = QStringLiteral("Invalid or oversized vector scene");
        return result;
    }

    QBuffer buffer(&result.bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("Unable to create PDF buffer");
        return result;
    }
    QPdfWriter writer(&buffer);
    writer.setCreator(QStringLiteral("FreeCAD Inspection"));
    writer.setTitle(QStringLiteral("Photo Inspection Sheet"));
    writer.setResolution(1200);
    const QPageSize pageSize(
        QSizeF(scene.widthMm, scene.heightMm),
        QPageSize::Millimeter,
        QStringLiteral("PhotoInspectionExact"),
        QPageSize::ExactMatch
    );
    writer.setPageLayout(
        QPageLayout(pageSize, QPageLayout::Portrait, QMarginsF(), QPageLayout::Millimeter)
    );

    QPainter painter;
    if (!painter.begin(&writer)) {
        result.error = QStringLiteral("Unable to start vector PDF renderer");
        result.bytes.clear();
        return result;
    }
    const double unitsPerMm = static_cast<double>(writer.resolution()) / 25.4;
    painter.scale(unitsPerMm, unitsPerMm);
    painter.setRenderHint(QPainter::Antialiasing, false);

    for (const Inspection::Photo::ScenePrimitive& primitive : scene.primitives) {
        if (primitive.kind == Inspection::Photo::ScenePrimitiveKind::Text) {
            if (!primitive.points.empty()) {
                QFont font(QStringLiteral("DejaVu Sans"));
                font.setPointSizeF(3.0 * 72.0 / 25.4);
                painter.setFont(font);
                painter.setPen(Qt::black);
                painter.drawText(
                    QPointF(primitive.points.front().x, primitive.points.front().y),
                    QString::fromUtf8(
                        primitive.text.data(),
                        static_cast<qsizetype>(primitive.text.size())
                    )
                );
            }
            continue;
        }
        if (primitive.points.empty()) {
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
        painter.setBrush(primitive.filled ? QBrush(Qt::black) : Qt::NoBrush);
        if (primitive.strokeWidthMm > 0.0) {
            QPen pen(Qt::black);
            pen.setWidthF(primitive.strokeWidthMm);
            pen.setCosmetic(false);
            painter.setPen(pen);
        }
        else {
            painter.setPen(Qt::NoPen);
        }
        painter.drawPath(path);
    }
    painter.end();
    buffer.close();
    if (result.bytes.size() < 8 || !result.bytes.startsWith("%PDF-")) {
        result.error = QStringLiteral("PDF renderer returned an invalid stream");
        result.bytes.clear();
        return result;
    }
    result.valid = true;
    return result;
}

bool writePhotoInspectionFileAtomically(const QString& target, const QByteArray& content, QString& error)
{
    if (target.isEmpty() || content.isEmpty()) {
        error = QStringLiteral("Output target or content is empty");
        return false;
    }
    const QFileInfo targetInfo(target);
    if (targetInfo.fileName().isEmpty() || !targetInfo.dir().exists()) {
        error = QStringLiteral("Output directory does not exist");
        return false;
    }
    QSaveFile file(target);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    if (file.write(content) != content.size()) {
        error = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    error.clear();
    return true;
}

}  // namespace InspectionGui
