// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 FreeCAD contributors                               *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 ***************************************************************************/

#pragma once

#include <QRect>
#include <QSize>
#include <QString>

#include <string>

#include <FCGlobal.h>

namespace Gui
{
class Document;
}

namespace Gui::WindowLayout
{

/// Return the settings-group key for a canonicalized document path.
GuiExport QString documentKey(const std::string& fileName);

/// Save all floating views belonging to a document.
GuiExport void save(const Gui::Document& document, const std::string& fileName);

/// Restore all floating views belonging to a document.
GuiExport void restore(Gui::Document& document);

/// Evict layouts for missing files and retain only the most recently used entries.
GuiExport void prune();

/// Clamp a top-level window frame to the available desktop geometry.
GuiExport QRect clampToAvailableScreens(const QRect& frameGeometry, const QSize& minimumSize);

}  // namespace Gui::WindowLayout
