/***************************************************************************
 *   Copyright (c) 2022 Abdullah Tahiri <abdullah.tahiri.yo@gmail.com>     *
 *   Copyright (c) 2026 FreeCAD Developers                                *
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

#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <Base/Console.h>

namespace Gui
{

/** Returns a readable, translated label for a notification log style. */
inline QString notificationTypeToString(Base::LogStyle type)
{
    switch (type) {
        case Base::LogStyle::Error:
            return QCoreApplication::translate("NotificationsWidget", "Error");
        case Base::LogStyle::Warning:
            return QCoreApplication::translate("NotificationsWidget", "Warning");
        case Base::LogStyle::Critical:
            return QCoreApplication::translate("NotificationsWidget", "Critical");
        case Base::LogStyle::Notification:
            return QCoreApplication::translate("NotificationsWidget", "Notification");
        case Base::LogStyle::Message:
            return QCoreApplication::translate("NotificationsWidget", "Message");
        case Base::LogStyle::Log:
            return QCoreApplication::translate("NotificationsWidget", "Log");
        default:
            return QCoreApplication::translate("NotificationsWidget", "Notification");
    }
}

/** Formats one notification as Type, Notifier, Message columns separated by tabs. */
inline QString formatNotificationClipboardLine(
    const QString& type,
    const QString& notifier,
    const QString& message
)
{
    return type + QLatin1Char('\t') + notifier + QLatin1Char('\t') + message;
}

/** Joins clipboard lines with newlines. Empty input yields an empty string. */
inline QString joinNotificationClipboardLines(const QStringList& lines)
{
    return lines.join(QLatin1Char('\n'));
}

}  // namespace Gui
