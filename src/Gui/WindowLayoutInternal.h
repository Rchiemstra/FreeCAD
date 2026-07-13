// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QRect>

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>

namespace Gui::WindowLayout::Internal
{

struct ViewRecord
{
    std::string typeName;
    std::string objectName;
    std::size_t ordinal {0};
    int mode {0};
    QRect geometry;
    bool maximized {false};

    bool operator==(const ViewRecord&) const = default;
};

inline std::string encodeRecord(const ViewRecord& record)
{
    std::ostringstream stream;
    stream << record.typeName << ' ' << record.objectName << ' ' << record.ordinal << ' '
           << record.mode << ' ' << record.geometry.x() << ' ' << record.geometry.y() << ' '
           << record.geometry.width() << ' ' << record.geometry.height() << ' '
           << (record.maximized ? 1 : 0);
    return stream.str();
}

inline bool decodeRecord(std::string_view encoded, ViewRecord& record)
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int maximized = 0;
    std::istringstream stream {std::string(encoded)};
    if (!(stream >> record.typeName >> record.objectName >> record.ordinal >> record.mode >> x >> y
          >> width >> height >> maximized)
        || record.mode < 0 || record.mode > 2 || width <= 0 || height <= 0
        || (maximized != 0 && maximized != 1)) {
        return false;
    }

    stream >> std::ws;
    if (!stream.eof()) {
        return false;
    }

    record.geometry = QRect(x, y, width, height);
    record.maximized = maximized != 0;
    return true;
}

}  // namespace Gui::WindowLayout::Internal
