/***************************************************************************
 *   Copyright (c) 2008 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/


#include <memory>

#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QPixmap>
#include <QThread>


#include <App/Application.h>
#include <Base/Reader.h>
#include <Base/Writer.h>
#ifdef _MSC_VER
# include <zipios++/zipios-config.h>
#endif
#include <zipios++/zipfile.h>

#include <Inventor/SbBox3f.h>
#include <Inventor/nodes/SoOrthographicCamera.h>

#include "Thumbnail.h"
#include "BitmapFactory.h"
#include "Camera.h"
#include "View3DInventorViewer.h"
#include "ViewProvider.h"


using namespace Gui;

Thumbnail::Thumbnail(int s)
    : size(s)
{}

Thumbnail::~Thumbnail() = default;

void Thumbnail::setViewer(View3DInventorViewer* v)
{
    this->viewer = v;
}

void Thumbnail::setSize(int s)
{
    this->size = s;
}

void Thumbnail::setFileName(const char* fn)
{
    this->uri = QUrl::fromLocalFile(QString::fromUtf8(fn));
}

unsigned int Thumbnail::getMemSize() const
{
    return 0;
}

void Thumbnail::Save(Base::Writer& writer) const
{
    // It's only possible to add extra information if force of XML is disabled
    if (!writer.isForceXML()) {
        writer.addFile("thumbnails/Thumbnail.png", this);
    }
}

void Thumbnail::Restore(Base::XMLReader& reader)
{
    Q_UNUSED(reader);
    // reader.addFile("Thumbnail.png",this);
}

namespace
{
QByteArray encodePng(const QPixmap& pixmap)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    pixmap.save(&buffer, "PNG");
    return bytes;
}
}  // namespace

void Thumbnail::capture()
{
    rendered.clear();
    fallback.clear();
    if (!this->viewer || this->viewer->thread() != QThread::currentThread()) {
        return;
    }

    // An empty document frames nothing, so it yields a blank thumbnail rather than a
    // stale one of geometry that has since been deleted.
    SbBox3f box;
    this->viewer->getSceneBoundBox(box);

    CoinPtr<SoOrthographicCamera> camera(new SoOrthographicCamera);
    camera->orientation.setValue(Camera::defaultOrientation());
    Camera::fitToBox(*camera, box, 1.0F);

    const View3DInventorViewer::RenderImageOptions options {
        .width = this->size,
        .height = this->size,
        .samples = 4,
        .background = QColor(0, 0, 0, 0),
        .alphaMode = View3DInventorViewer::AlphaMode::PerPixel,
        .intent = View3DInventorViewer::RenderIntent::RasterCapture,
        .includeViewerLighting = true,
        .camera = camera.get(),
    };
    QImage img = this->viewer->renderToImage(options);

    QPixmap appIcon = Gui::BitmapFactory().pixmap(App::Application::Config()["AppIcon"].c_str());
    if (img.isNull()) {
        fallback = encodePng(appIcon);
        return;
    }

    // according to specification add some meta-information to the image
    qint64 mt = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    img.setText(QLatin1String("Software"), qApp->applicationName());
    img.setText(QLatin1String("Thumb::Mimetype"), QLatin1String("application/x-extension-fcstd"));
    img.setText(QLatin1String("Thumb::MTime"), QStringLiteral("%1").arg(mt));
    img.setText(QLatin1String("Thumb::URI"), this->uri.toString());

    QPixmap px = QPixmap::fromImage(img);
    // Create a small "Fc" Application icon in the bottom right of the thumbnail
    if (App::GetApplication()
            .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Document")
            ->GetBool("AddThumbnailLogo", false)) {
        appIcon = appIcon.scaled(
            this->size / 4,
            this->size / 4,
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation
        );
        px = BitmapFactory().merge(px, appIcon, BitmapFactoryInst::BottomRight);
    }
    rendered = encodePng(px);
}

void Thumbnail::SaveDocFile(Base::Writer& writer) const
{
    // Runs wherever the document is serialized, possibly off the GUI thread, so
    // it only writes what capture() rendered on the GUI thread.
    if (!rendered.isEmpty()) {
        writer.Stream().write(rendered.constData(), rendered.size());
        return;
    }

    // Without a fresh rendering keep the thumbnail the file already has.
    QString filename = this->uri.toLocalFile();
    Base::FileInfo fi(filename.toUtf8().constData());
    if (fi.exists()) {
        try {
            zipios::ZipFile zf(fi.filePath());
            // getEntry uses default MatchPath=MATCH.
            zipios::ConstEntryPointer entry = zf.getEntry("thumbnails/Thumbnail.png");
            if (entry && entry->isValid()) {
                std::unique_ptr<std::istream> is(zf.getInputStream(entry));
                if (is && is->good()) {
                    writer.Stream() << is->rdbuf();
                    return;
                }
            }
        }
        catch (const std::exception&) {
            // If the file isn't a zip or is locked, we ignore it and proceed to fallback
        }
        catch (...) {
            // Ignore unknown exceptions
        }
    }

    if (!fallback.isEmpty()) {
        writer.Stream().write(fallback.constData(), fallback.size());
    }
}

void Thumbnail::RestoreDocFile(Base::Reader& reader)
{
    Q_UNUSED(reader);
}
