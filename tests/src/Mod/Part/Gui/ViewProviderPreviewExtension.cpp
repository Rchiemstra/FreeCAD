// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Inventor/SoDB.h>
#include <Inventor/errors/SoDebugError.h>
#include <Inventor/nodes/SoSeparator.h>

#include <Mod/Part/Gui/ViewProviderPreviewExtensionInternal.h>

using namespace PartGui::PreviewExtensionInternal;

namespace
{

int coinErrors = 0;

void countCoinError(const SoError* /*error*/, void* /*data*/)
{
    ++coinErrors;
}

}  // namespace

class PreviewRootTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        SoDB::init();
    }

    void SetUp() override
    {
        annotation = new SoSeparator;
        annotation->ref();
        previewRoot = new SoSeparator;
        previewRoot->ref();
        coinErrors = 0;
        previousHandler = SoDebugError::getHandlerCallback();
        previousData = SoDebugError::getHandlerData();
        SoDebugError::setHandlerCallback(countCoinError, nullptr);
    }

    void TearDown() override
    {
        SoDebugError::setHandlerCallback(previousHandler, previousData);
        previewRoot->unref();
        annotation->unref();
    }

    SoSeparator* annotation {};
    SoSeparator* previewRoot {};
    SoErrorCB* previousHandler {};
    void* previousData {};
};

TEST_F(PreviewRootTest, attachAndDetachRoundTrip)
{
    attachPreviewRoot(annotation, previewRoot);
    attachPreviewRoot(annotation, previewRoot);
    EXPECT_EQ(annotation->getNumChildren(), 1);

    detachPreviewRoot(annotation, previewRoot);
    EXPECT_EQ(annotation->findChild(previewRoot), -1);
    EXPECT_EQ(coinErrors, 0);
}

// Deleting a feature whose preview was never shown (undo of its creation,
// closing the document) detaches a root that was never attached.
TEST_F(PreviewRootTest, detachingAnUnattachedRootPostsNoCoinError)
{
    detachPreviewRoot(annotation, previewRoot);
    detachPreviewRoot(annotation, previewRoot);

    EXPECT_EQ(annotation->getNumChildren(), 0);
    EXPECT_EQ(coinErrors, 0);
}

TEST_F(PreviewRootTest, missingAnnotationIsIgnored)
{
    attachPreviewRoot(nullptr, previewRoot);
    detachPreviewRoot(nullptr, previewRoot);

    EXPECT_EQ(coinErrors, 0);
}
