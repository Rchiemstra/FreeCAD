// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 FreeCAD contributors                               *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 ***************************************************************************/

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QPolygon>
#include <QScreen>

#include <algorithm>
#include <iterator>
#include <map>
#include <vector>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Parameter.h>
#include <Base/Type.h>

#include "Document.h"
#include "MDIView.h"
#include "ViewProvider.h"
#include "WindowLayout.h"
#include "WindowLayoutInternal.h"

namespace
{

constexpr std::size_t maximumLayoutCount = 20;

ParameterGrp::handle layoutRoot()
{
    return App::GetApplication()
        .GetUserParameter()
        .GetGroup("BaseApp")
        ->GetGroup("Preferences")
        ->GetGroup("DocumentWindows");
}

bool layoutEnabled()
{
    return App::GetApplication()
        .GetUserParameter()
        .GetGroup("BaseApp")
        ->GetGroup("Preferences")
        ->GetGroup("View")
        ->GetBool("SaveWindowLayoutPerDocument", true);
}

QString canonicalPath(const std::string& fileName)
{
    if (fileName.empty()) {
        return {};
    }

    QString path = QDir::fromNativeSeparators(
        QFileInfo(QString::fromUtf8(fileName.c_str())).absoluteFilePath()
    );
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    return path;
}

std::string groupName(const ParameterGrp::handle& group)
{
    const std::string path = group->GetPath();
    const auto separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

}  // namespace

QString Gui::WindowLayout::documentKey(const std::string& fileName)
{
    const QString path = canonicalPath(fileName);
    if (path.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex()
    );
}

void Gui::WindowLayout::save(const Gui::Document& document, const std::string& fileName)
{
    if (!layoutEnabled()) {
        return;
    }

    const QString path = canonicalPath(fileName);
    const QString key = documentKey(fileName);
    if (path.isEmpty() || key.isEmpty()) {
        return;
    }

    auto group = layoutRoot()->GetGroup(key.toLatin1().constData());
    group->Clear();
    group->SetASCII("FileName", path.toUtf8().constData());
    group->SetUnsigned("LastUsed", static_cast<unsigned long>(QDateTime::currentSecsSinceEpoch()));

    std::map<std::string, std::size_t> typeOrdinals;
    int recordIndex = 0;
    for (auto* view : document.getMDIViews()) {
        const std::string typeName = view->getTypeId().getName();
        const std::size_t ordinal = typeOrdinals[typeName]++;
        if (view->currentViewMode() == Gui::MDIView::Child) {
            continue;
        }

        const auto* owner = view->getOwnerObject();
        const char* ownerName = owner ? owner->getNameInDocument() : nullptr;
        const QRect geometry = view->isMaximized() ? view->normalGeometry()
                                                    : QRect(view->pos(), view->size());
        Internal::ViewRecord record {
            typeName,
            ownerName ? ownerName : "-",
            ordinal,
            static_cast<int>(view->currentViewMode()),
            geometry,
            view->isMaximized(),
        };
        group->SetASCII(
            ("View" + std::to_string(recordIndex++)).c_str(),
            Internal::encodeRecord(record)
        );
    }

    prune();
}

void Gui::WindowLayout::restore(Gui::Document& document)
{
    if (!layoutEnabled() || document.getMDIViews().empty()) {
        return;
    }

    const std::string fileName = document.getDocument()->FileName.getValue();
    const QString path = canonicalPath(fileName);
    const QString key = documentKey(fileName);
    if (path.isEmpty() || key.isEmpty()) {
        return;
    }

    auto root = layoutRoot();
    if (!root->HasGroup(key.toLatin1().constData())) {
        return;
    }
    auto group = root->GetGroup(key.toLatin1().constData());
    if (QString::fromUtf8(group->GetASCII("FileName").c_str()) != path) {
        return;
    }
    group->SetUnsigned("LastUsed", static_cast<unsigned long>(QDateTime::currentSecsSinceEpoch()));

    for (int index = 0;; ++index) {
        const std::string encoded = group->GetASCII(("View" + std::to_string(index)).c_str());
        if (encoded.empty()) {
            break;
        }

        Internal::ViewRecord record;
        if (!Internal::decodeRecord(encoded, record)) {
            continue;
        }

        const Base::Type type = Base::Type::fromName(record.typeName.c_str());
        if (type.isBad() || !type.isDerivedFrom(Gui::MDIView::getClassTypeId())) {
            continue;
        }

        Gui::MDIView* view = nullptr;
        if (record.objectName != "-") {
            if (auto* provider = document.getViewProviderByName(record.objectName.c_str())) {
                view = provider->getMDIView();
            }
        }
        else {
            auto views = document.getMDIViewsOfType(type);
            views.remove_if([](MDIView* v) { return v->isDeleting(); });
            if (record.ordinal < views.size()) {
                auto iterator = views.begin();
                std::advance(iterator, record.ordinal);
                view = *iterator;
            }
            else {
                view = document.createView(type);
            }
        }
        if (!view) {
            continue;
        }

        // Establish a normal top-level geometry before optionally entering fullscreen.
        view = Gui::MDIView::changeViewMode(view, Gui::MDIView::TopLevel);
        if (!view) {
            continue;
        }
        view->showNormal();
        const QRect geometry
            = clampToAvailableScreens(record.geometry, view->minimumSizeHint());
        view->resize(geometry.size());
        view->move(geometry.topLeft());

        if (record.mode == static_cast<int>(Gui::MDIView::FullScreen)) {
            Gui::MDIView::changeViewMode(view, Gui::MDIView::FullScreen);
        }
        else if (record.maximized) {
            view->showMaximized();
        }
    }
}

void Gui::WindowLayout::prune()
{
    auto root = layoutRoot();
    struct Entry
    {
        std::string name;
        unsigned long lastUsed;
    };
    std::vector<Entry> entries;

    for (const auto& group : root->GetGroups()) {
        const std::string name = groupName(group);
        const QString fileName = QString::fromUtf8(group->GetASCII("FileName").c_str());
        if (fileName.isEmpty() || !QFileInfo::exists(fileName)) {
            root->RemoveGrp(name.c_str());
            continue;
        }
        entries.push_back({name, group->GetUnsigned("LastUsed")});
    }

    if (entries.size() <= maximumLayoutCount) {
        return;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
        return left.lastUsed < right.lastUsed;
    });
    for (std::size_t index = 0; index < entries.size() - maximumLayoutCount; ++index) {
        root->RemoveGrp(entries[index].name.c_str());
    }
}

QRect Gui::WindowLayout::clampToAvailableScreens(
    const QRect& frameGeometry,
    const QSize& minimumSize
)
{
    QRect geometry = frameGeometry;
    const auto screens = QApplication::screens();
    if (screens.isEmpty()) {
        return geometry;
    }

    auto invisible = QPolygon(geometry);
    for (auto* screen : screens) {
        invisible = invisible.subtracted(screen->geometry().adjusted(-10, -10, 10, 10));
    }
    if (invisible.empty()) {
        return geometry;
    }

    QRect targetScreen;
    for (int screenArea = 0; auto* screen : screens) {
        const auto overlap = screen->availableGeometry().intersected(geometry);
        const int overlapArea = overlap.width() * overlap.height();
        if (overlapArea > screenArea) {
            targetScreen = screen->availableGeometry();
            screenArea = overlapArea;
        }
    }
    if (targetScreen.isEmpty()) {
        for (int screenDist = -1; auto* screen : screens) {
            const auto dist
                = (geometry.center() - screen->availableGeometry().center()).manhattanLength();
            if (screenDist == -1 || dist < screenDist) {
                targetScreen = screen->availableGeometry();
                screenDist = dist;
            }
        }
    }

    geometry.setSize(geometry.size().boundedTo(targetScreen.size()).expandedTo(minimumSize));
    geometry.moveLeft(
        qMax(qMin(geometry.x(), targetScreen.right() - geometry.width()), targetScreen.x())
    );
    geometry.moveTop(
        qMax(qMin(geometry.y(), targetScreen.bottom() - geometry.height()), targetScreen.y())
    );
    return geometry;
}
