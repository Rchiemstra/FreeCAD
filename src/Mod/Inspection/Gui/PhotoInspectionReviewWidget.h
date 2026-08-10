// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QString>
#include <QWidget>

#include <Mod/Inspection/App/PhotoInspectionImage.h>
#include <Mod/Inspection/App/PhotoInspectionSheet.h>
#include <Mod/Inspection/InspectionGlobal.h>

namespace InspectionGui
{

class InspectionGuiExport PhotoInspectionReviewWidget: public QWidget
{
    Q_OBJECT

public:
    explicit PhotoInspectionReviewWidget(QWidget* parent = nullptr);

    void setScene(const Inspection::Photo::VectorScene& scene);
    void setRectifiedImage(const Inspection::Photo::GrayRaster& raster);
    void clearRectifiedImage();
    void setLayerVisible(const QString& layer, bool visible);
    bool isLayerVisible(const QString& layer) const;
    void setLayerColor(const QString& layer, const QColor& color);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Inspection::Photo::VectorScene scene;
    QImage rectifiedImage;
    QHash<QString, bool> layerVisibility;
    QHash<QString, QColor> layerColors;
};

}  // namespace InspectionGui
