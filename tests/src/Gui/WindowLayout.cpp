// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QDir>

#include <vector>

#include <Gui/WindowLayout.h>
#include <Gui/WindowLayoutInternal.h>

namespace
{

using Gui::WindowLayout::Internal::ViewRecord;

TEST(WindowLayoutRecord, roundTripsEveryField)
{
    const ViewRecord expected {
        "SpreadsheetGui::SheetView",
        "Spreadsheet",
        3,
        1,
        QRect(-1200, 75, 1024, 768),
        true,
    };

    ViewRecord decoded;
    ASSERT_TRUE(Gui::WindowLayout::Internal::decodeRecord(
        Gui::WindowLayout::Internal::encodeRecord(expected),
        decoded
    ));
    EXPECT_EQ(decoded, expected);
}

TEST(WindowLayoutRecord, rejectsMalformedAndUnsafeRecords)
{
    const std::vector<std::string> invalid {
        "",
        "Gui::View3DInventor - 0 1 10 20 0 300 0",
        "Gui::View3DInventor - 0 9 10 20 300 200 0",
        "Gui::View3DInventor - 0 1 10 20 -1 200 0",
        "Gui::View3DInventor - 0 1 10 20 300 200 2",
        "Gui::View3DInventor - 0 1 10 20 300 200 0 trailing",
    };

    for (const auto& encoded : invalid) {
        ViewRecord decoded;
        EXPECT_FALSE(Gui::WindowLayout::Internal::decodeRecord(encoded, decoded)) << encoded;
    }
}

TEST(WindowLayoutPath, normalizesNativeSeparators)
{
    const QString path = QDir::current().absoluteFilePath(QStringLiteral("layout-test.FCStd"));
    const QString nativePath = QDir::toNativeSeparators(path);

    const QString key = Gui::WindowLayout::documentKey(path.toUtf8().toStdString());
    EXPECT_EQ(key.size(), 32);
    EXPECT_EQ(key, Gui::WindowLayout::documentKey(nativePath.toUtf8().toStdString()));

#ifdef Q_OS_WIN
    EXPECT_EQ(key, Gui::WindowLayout::documentKey(path.toUpper().toUtf8().toStdString()));
#endif
}

}  // namespace
