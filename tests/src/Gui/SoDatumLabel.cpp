// SPDX-License-Identifier: LGPL-2.1-or-later
// Runtime coverage for the SoDatumLabel distance pick/bbox path that previously
// bound an unused pixel `srcw` local under -Werror,-Wunused-variable.

#include <gtest/gtest.h>

#include <QApplication>

#include <Inventor/SbBox3f.h>
#include <Inventor/SbVec3f.h>
#include <Inventor/SbViewportRegion.h>
#include <Inventor/actions/SoGetBoundingBoxAction.h>
#include <Inventor/nodes/SoOrthographicCamera.h>
#include <Inventor/nodes/SoSeparator.h>

#include <Gui/Application.h>
#include <Gui/SoDatumLabel.h>
#include <src/App/InitApplication.h>

namespace
{

void initializeDatumLabelGui()
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
    tests::initApplication();
    if (!Gui::Application::Instance) {
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        new Gui::Application(true);
    }
}

bool boxContainsXY(const SbBox3f& box, const SbVec3f& point)
{
    const SbVec3f min = box.getMin();
    const SbVec3f max = box.getMax();
    return point[0] >= min[0] && point[0] <= max[0] && point[1] >= min[1] && point[1] <= max[1];
}

class SoDatumLabelTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        initializeDatumLabelGui();
    }
};

}  // namespace

TEST_F(SoDatumLabelTest, measureDatumTextUsesSrcwForAspectRatio)
{
    auto* label = new Gui::SoDatumLabel();
    label->ref();
    label->string.setValue("12.00 mm");
    label->size.setValue(12);
    label->sampling.setValue(2.0F);

    const Gui::SoDatumLabel::DatumTextMetrics metrics = label->measureDatumText(1.0F);
    ASSERT_GT(metrics.srcw, 0);
    ASSERT_GT(metrics.srch, 0);
    ASSERT_GT(metrics.imgHeight, 0.0F);
    ASSERT_GT(metrics.imgWidth, 0.0F);
    EXPECT_NEAR(
        metrics.imgWidth / metrics.imgHeight,
        static_cast<float>(metrics.srcw) / static_cast<float>(metrics.srch),
        1.0e-5F
    );

    label->unref();
}

TEST_F(SoDatumLabelTest, distanceBBoxContainsEndpoints)
{
    auto* root = new SoSeparator();
    root->ref();
    root->addChild(new SoOrthographicCamera());

    auto* label = new Gui::SoDatumLabel();
    label->string.setValue("10");
    label->size.setValue(12);
    label->sampling.setValue(2.0F);
    label->datumtype.setValue(Gui::SoDatumLabel::DISTANCE);
    label->param1.setValue(2.0F);
    label->setPoints(SbVec3f(0.0F, 0.0F, 0.0F), SbVec3f(10.0F, 0.0F, 0.0F));
    root->addChild(label);

    SoGetBoundingBoxAction action(SbViewportRegion(800, 600));
    action.apply(root);
    const SbBox3f box = action.getBoundingBox();

    ASSERT_FALSE(box.isEmpty());
    EXPECT_TRUE(boxContainsXY(box, SbVec3f(0.0F, 0.0F, 0.0F)));
    EXPECT_TRUE(boxContainsXY(box, SbVec3f(10.0F, 0.0F, 0.0F)));
    EXPECT_GT(box.getMax()[0] - box.getMin()[0], 10.0F - 1.0e-3F);

    root->unref();
}

TEST_F(SoDatumLabelTest, distanceBBoxWithTooFewPointsStaysEmpty)
{
    auto* root = new SoSeparator();
    root->ref();
    root->addChild(new SoOrthographicCamera());

    auto* label = new Gui::SoDatumLabel();
    label->string.setValue("10");
    label->datumtype.setValue(Gui::SoDatumLabel::DISTANCE);
    root->addChild(label);

    SoGetBoundingBoxAction action(SbViewportRegion(800, 600));
    action.apply(root);
    EXPECT_TRUE(action.getBoundingBox().isEmpty());

    root->unref();
}
