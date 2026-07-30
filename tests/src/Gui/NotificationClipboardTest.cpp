// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include <Base/Console.h>
#include "Gui/NotificationClipboard.h"

using namespace Gui;

TEST(NotificationClipboardTest, TypeStringsAreReadable)
{
    EXPECT_FALSE(notificationTypeToString(Base::LogStyle::Error).isEmpty());
    EXPECT_FALSE(notificationTypeToString(Base::LogStyle::Warning).isEmpty());
    EXPECT_FALSE(notificationTypeToString(Base::LogStyle::Critical).isEmpty());
    EXPECT_FALSE(notificationTypeToString(Base::LogStyle::Notification).isEmpty());

    // Must not be empty merely because the UI shows an icon for the type column
    EXPECT_NE(notificationTypeToString(Base::LogStyle::Error), QString());
}

TEST(NotificationClipboardTest, FormatSingleNotification)
{
    const QString line = formatNotificationClipboardLine(
        QStringLiteral("Error"),
        QStringLiteral("HamaAdapter"),
        QStringLiteral(
            "Transaction denied (DENY_NO_CAPABILITY): TransactionOpen on document "
            "'HamaAdapter', property 'Transform'"
        )
    );

    EXPECT_EQ(
        line,
        QStringLiteral(
            "Error\tHamaAdapter\tTransaction denied (DENY_NO_CAPABILITY): TransactionOpen "
            "on document 'HamaAdapter', property 'Transform'"
        )
    );
}

TEST(NotificationClipboardTest, FormatMultipleNotifications)
{
    QStringList lines;
    lines << formatNotificationClipboardLine(
        QStringLiteral("Error"),
        QStringLiteral("HamaAdapter"),
        QStringLiteral("Transaction denied (DENY_NO_CAPABILITY): TransactionOpen on document "
                       "'HamaAdapter', property 'Transform'")
    );
    lines << formatNotificationClipboardLine(
        QStringLiteral("Warning"),
        QStringLiteral("ExampleNotifier"),
        QStringLiteral("Example warning text")
    );

    const QString clipboard = joinNotificationClipboardLines(lines);

    EXPECT_EQ(
        clipboard,
        QStringLiteral(
            "Error\tHamaAdapter\tTransaction denied (DENY_NO_CAPABILITY): TransactionOpen on "
            "document 'HamaAdapter', property 'Transform'\n"
            "Warning\tExampleNotifier\tExample warning text"
        )
    );
}

TEST(NotificationClipboardTest, EmptySelectionProducesEmptyClipboardText)
{
    EXPECT_TRUE(joinNotificationClipboardLines({}).isEmpty());
}

TEST(NotificationClipboardTest, RepetitionCountPreservedInMessage)
{
    const QString line = formatNotificationClipboardLine(
        QStringLiteral("Warning"),
        QStringLiteral("Part"),
        QStringLiteral("Skipped recompute (3 times)")
    );

    EXPECT_EQ(line, QStringLiteral("Warning\tPart\tSkipped recompute (3 times)"));
    EXPECT_TRUE(line.contains(QStringLiteral("(3 times)")));
}

TEST(NotificationClipboardTest, FormattingDoesNotAlterInputs)
{
    const QString type = QStringLiteral("Error");
    const QString notifier = QStringLiteral("Notifier");
    const QString message = QStringLiteral("Original message");

    const QString line = formatNotificationClipboardLine(type, notifier, message);

    EXPECT_EQ(type, QStringLiteral("Error"));
    EXPECT_EQ(notifier, QStringLiteral("Notifier"));
    EXPECT_EQ(message, QStringLiteral("Original message"));
    EXPECT_TRUE(line.startsWith(type));
    EXPECT_TRUE(line.endsWith(message));
}
