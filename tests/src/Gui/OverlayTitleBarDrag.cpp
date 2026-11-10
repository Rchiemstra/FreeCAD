// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>
#include <QMouseEvent>
#include <QtTest/QTest>

#include <Gui/OverlayWidgets.h>

namespace Gui
{

class OverlayTitleBarDragTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        static int argc = 1;
        static char executable[] = "Gui_tests_run";
        static char* argv[] = {executable, nullptr};
        if (!QApplication::instance()) {
            new QApplication(argc, argv);
        }
    }

    void SetUp() override
    {
        originalDragging = OverlayTabWidget::_Dragging;
        originalFrame = OverlayTabWidget::_DragFrame;
        originalFloating = OverlayTabWidget::_DragFloating;
    }

    void TearDown() override
    {
        OverlayTabWidget::_Dragging = originalDragging;
        OverlayTabWidget::_DragFrame = originalFrame;
        OverlayTabWidget::_DragFloating = originalFloating;
    }

    static void setDrag(QWidget* dragging, OverlayDragFrame* frame, QDockWidget* floating)
    {
        OverlayTabWidget::_Dragging = dragging;
        OverlayTabWidget::_DragFrame = frame;
        OverlayTabWidget::_DragFloating = floating;
    }

    static QWidget* dragging()
    {
        return OverlayTabWidget::_Dragging;
    }

private:
    QWidget* originalDragging = nullptr;
    OverlayDragFrame* originalFrame = nullptr;
    QDockWidget* originalFloating = nullptr;
};

TEST_F(OverlayTitleBarDragTest, escapeHidesBothDragPreviews)
{
    OverlayTitleBar title(nullptr);
    OverlayDragFrame frame(nullptr);
    QDockWidget floating;
    frame.show();
    floating.show();
    setDrag(&title, &frame, &floating);

    QTest::keyClick(&title, Qt::Key_Escape);

    EXPECT_EQ(dragging(), nullptr);
    EXPECT_EQ(title.cursor().shape(), Qt::OpenHandCursor);
    EXPECT_TRUE(frame.isHidden());
    EXPECT_TRUE(floating.isHidden());
}

TEST_F(OverlayTitleBarDragTest, cancelOutsideBeforeAnyDockPreviewExists)
{
    OverlayTitleBar title(nullptr);
    QDockWidget floating;
    floating.show();
    // Dragging directly outside can create the floating preview before the
    // dock-target frame has ever been allocated.
    setDrag(&title, nullptr, &floating);

    title.endDrag();

    EXPECT_EQ(dragging(), nullptr);
    EXPECT_TRUE(floating.isHidden());
    title.endDrag();
    EXPECT_TRUE(floating.isHidden());
}

TEST_F(OverlayTitleBarDragTest, losingTheMouseButtonHidesTheFloatingPreview)
{
    OverlayTitleBar title(nullptr);
    QDockWidget floating;
    floating.show();
    setDrag(&title, nullptr, &floating);
    QMouseEvent move(QEvent::MouseMove, QPointF(10, 10), QPointF(10, 10),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);

    QApplication::sendEvent(&title, &move);

    EXPECT_EQ(dragging(), nullptr);
    EXPECT_TRUE(floating.isHidden());
}

TEST_F(OverlayTitleBarDragTest, anotherTitleBarCannotCancelTheActiveDrag)
{
    OverlayTitleBar title(nullptr);
    OverlayTitleBar otherTitle(nullptr);
    OverlayDragFrame frame(nullptr);
    QDockWidget floating;
    frame.show();
    floating.show();
    setDrag(&otherTitle, &frame, &floating);

    title.endDrag();

    EXPECT_EQ(dragging(), &otherTitle);
    EXPECT_FALSE(frame.isHidden());
    EXPECT_FALSE(floating.isHidden());
}

}  // namespace Gui
