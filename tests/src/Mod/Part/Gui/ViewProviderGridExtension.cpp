// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <limits>

#include <Mod/Part/Gui/ViewProviderGridExtensionInternal.h>

TEST(GridExtensionPlan, rejectsLargeFiniteXOffsetCast)
{
    const auto plan = PartGui::GridExtensionInternal::planSketchGrid(
        100.0F,
        1.0,
        10000000000.0,
        0.0
    );
    EXPECT_FALSE(plan.valid);

    int offsetX = 0;
    int offsetY = 0;
    EXPECT_FALSE(PartGui::GridExtensionInternal::tryGridOffsets(
        static_cast<float>(10000000000.0) - 75.0F,
        0.0F,
        1.0,
        150,
        300,
        offsetX,
        offsetY
    ));
}

TEST(GridExtensionPlan, rejectsXOffsetAddOverflowAfterRepresentableCast)
{
    int offsetX = 0;
    int offsetY = 0;
    EXPECT_FALSE(PartGui::GridExtensionInternal::tryGridOffsets(
        2147483520.0F,
        0.0F,
        1.0,
        150,
        300,
        offsetX,
        offsetY
    ));
}

TEST(GridExtensionPlan, rejectsYOffsetSubOverflowAfterRepresentableCast)
{
    int offsetX = 0;
    int offsetY = 0;
    EXPECT_FALSE(PartGui::GridExtensionInternal::tryGridOffsets(
        0.0F,
        static_cast<float>(std::numeric_limits<int>::min() + 20),
        1.0,
        150,
        300,
        offsetX,
        offsetY
    ));
}

TEST(GridExtensionPlan, recoversFiniteExtentsToValidOffsets)
{
    const auto plan = PartGui::GridExtensionInternal::planSketchGrid(
        PartGui::GridExtensionInternal::recoverCameraExtent(
            std::numeric_limits<float>::infinity(),
            200.0F
        ),
        10.0,
        0.0,
        0.0
    );
    EXPECT_TRUE(plan.valid);
    EXPECT_GE(plan.vlines, 1);
    EXPECT_EQ(plan.nlines, 2 * plan.vlines);
}

TEST(GridExtensionOwnership, rejectedPlanDoesNotAllocate)
{
    int allocations = 0;
    const bool allocated = PartGui::GridExtensionInternal::allocateGridNodesAfterPlan(
        false,
        [&allocations] {
            ++allocations;
        }
    );
    EXPECT_FALSE(allocated);
    EXPECT_EQ(allocations, 0);
}
