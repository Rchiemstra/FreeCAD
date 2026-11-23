// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Inventor/SoDB.h>
#include <Inventor/errors/SoDebugError.h>

#include <Gui/Inventor/Draggers/SoLinearDragger.h>
#include <Gui/Inventor/Draggers/SoRotationDragger.h>
#include <Gui/Inventor/Draggers/SoTransformDragger.h>
#include <Gui/SoFCDB.h>
#include <src/App/InitApplication.h>

#include <string>
#include <vector>

namespace
{
class CoinErrors
{
public:
    CoinErrors()
        : previous(SoDebugError::getHandlerCallback())
        , previousData(SoDebugError::getHandlerData())
    {
        SoDebugError::setHandlerCallback(record, this);
    }

    ~CoinErrors()
    {
        SoDebugError::setHandlerCallback(previous, previousData);
    }

    bool contains(const std::string& text) const
    {
        for (const auto& message : messages) {
            if (message.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    static void record(const SoError* error, void* data)
    {
        auto* self = static_cast<CoinErrors*>(data);
        self->messages.emplace_back(error->getDebugString().getString());
        if (self->previous) {
            self->previous(error, self->previousData);
        }
    }

    SoErrorCB* previous;
    void* previousData;
    std::vector<std::string> messages;
};

class SoTransformDraggerTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        SoDB::init();
        if (!Gui::SoFCDB::isInitialized()) {
            Gui::SoFCDB::init();
        }
    }
};

TEST_F(SoTransformDraggerTest, ContainersStartWithValidIdentityRotations)
{
    CoinErrors errors;
    auto* linear = new Gui::SoLinearDraggerContainer;
    auto* angular = new Gui::SoRotationDraggerContainer;
    linear->ref();
    angular->ref();

    EXPECT_TRUE(linear->rotation.getValue().equals(SbRotation::identity(), 1e-6f));
    EXPECT_TRUE(angular->rotation.getValue().equals(SbRotation::identity(), 1e-6f));
    angular->unref();
    linear->unref();
    EXPECT_FALSE(errors.contains("Quarternion has zero length"));
}

TEST_F(SoTransformDraggerTest, DestructionHandlesScheduledAndIdleSensors)
{
    CoinErrors errors;
    for (bool schedule : {false, true}) {
        auto* dragger = new Gui::SoTransformDragger;
        dragger->ref();
        if (schedule) {
            dragger->idleSensor.schedule();
        }
        dragger->unref();
    }
    EXPECT_FALSE(errors.contains("tried to unschedule a sensor which is not scheduled"));
}
}  // namespace
