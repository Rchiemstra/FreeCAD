// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <memory>

#include <QApplication>
#include <QTimer>

#include <Gui/Navigation/NavigationAnimation.h>
#include <Gui/Navigation/NavigationAnimator.h>

namespace
{

class ProbeAnimation: public Gui::NavigationAnimation
{
public:
    explicit ProbeAnimation(int duration)
        : NavigationAnimation(nullptr)
    {
        setDuration(duration);
        setStartValue(0.0);
        setEndValue(1.0);
        if (duration > 0) {
            // Completion/interruption is controlled by the queued test callback.
            setLoopCount(-1);
        }
    }

    void finishNow()
    {
        setLoopCount(1);
        setCurrentTime(duration());
    }

    bool initialized = false;
    int stopCount = 0;
    bool completedNormally = false;
    double lastValue = 0.0;

private:
    void initialize() override
    {
        initialized = true;
    }

    void update(const QVariant& value) override
    {
        lastValue = value.toDouble();
    }

    void onStop(bool finished) override
    {
        ++stopCount;
        completedNormally = finished;
    }
};

class NavigationAnimatorTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!QApplication::instance()) {
            static int argc = 1;
            static char name[] = "NavigationAnimator";
            static char* argv[] = {name, nullptr};
            // Match the process-lifetime QApplication used by the other GUI suites.
            new QApplication(argc, argv);
        }
    }
};

}  // namespace

TEST_F(NavigationAnimatorTest, zeroDurationDoesNotEnterEventLoop)
{
    Gui::NavigationAnimator animator;
    auto animation = std::make_shared<ProbeAnimation>(0);
    bool enteredEventLoop = false;
    QTimer unexpectedWait;
    unexpectedWait.setSingleShot(true);
    QObject::connect(&unexpectedWait, &QTimer::timeout, &unexpectedWait, [&] {
        enteredEventLoop = true;
        // Completion already happened. Release the old implementation's wait
        // through its interruption connection so a regression cannot hang the suite.
        Q_EMIT animation->interrupted();
    });
    unexpectedWait.start(0);

    const bool finished = animator.startAndWait(animation);
    unexpectedWait.stop();

    EXPECT_TRUE(finished);
    EXPECT_FALSE(enteredEventLoop);
    EXPECT_TRUE(animation->initialized);
    EXPECT_EQ(animation->stopCount, 1);
    EXPECT_TRUE(animation->completedNormally);
    EXPECT_DOUBLE_EQ(animation->lastValue, 1.0);
    EXPECT_FALSE(animator.isAnimating());
}

TEST_F(NavigationAnimatorTest, runningAnimationWaitsForCompletion)
{
    Gui::NavigationAnimator animator;
    auto animation = std::make_shared<ProbeAnimation>(1000);
    QTimer completion;
    completion.setSingleShot(true);
    QObject::connect(&completion, &QTimer::timeout, &completion, [&] {
        animation->finishNow();
    });
    completion.start(0);

    const bool finished = animator.startAndWait(animation);
    completion.stop();

    EXPECT_TRUE(finished);
    EXPECT_TRUE(animation->initialized);
    EXPECT_EQ(animation->stopCount, 1);
    EXPECT_TRUE(animation->completedNormally);
    EXPECT_DOUBLE_EQ(animation->lastValue, 1.0);
    EXPECT_FALSE(animator.isAnimating());
}

TEST_F(NavigationAnimatorTest, runningAnimationCanBeInterrupted)
{
    Gui::NavigationAnimator animator;
    auto animation = std::make_shared<ProbeAnimation>(1000);
    QTimer interruption;
    interruption.setSingleShot(true);
    QObject::connect(&interruption, &QTimer::timeout, &interruption, [&] {
        animator.stop();
    });
    interruption.start(0);

    const bool finished = animator.startAndWait(animation);
    interruption.stop();

    EXPECT_FALSE(finished);
    EXPECT_TRUE(animation->initialized);
    EXPECT_EQ(animation->stopCount, 1);
    EXPECT_FALSE(animation->completedNormally);
    EXPECT_FALSE(animator.isAnimating());
}
