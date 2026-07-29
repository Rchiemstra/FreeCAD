// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QByteArray>
#include <QString>

#include <Mod/Inspection/App/PhotoInspectionSheet.h>
#include <Mod/Inspection/InspectionGlobal.h>

namespace InspectionGui
{

struct InspectionGuiExport PdfRenderResult
{
    bool valid {false};
    QByteArray bytes;
    QString error;
};

InspectionGuiExport PdfRenderResult renderPhotoInspectionPdf(const Inspection::Photo::VectorScene& scene
);

InspectionGuiExport bool writePhotoInspectionFileAtomically(
    const QString& target,
    const QByteArray& content,
    QString& error
);

}  // namespace InspectionGui
