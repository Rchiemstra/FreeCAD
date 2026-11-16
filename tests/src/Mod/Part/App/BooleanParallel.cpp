// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <sstream>

#include <BOPAlgo_Options.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Standard_Version.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <src/App/InitApplication.h>

#include "Mod/Part/App/BooleanParallel.h"
#include "Mod/Part/App/FCBRepAlgoAPI_Common.h"
#include "Mod/Part/App/FCBRepAlgoAPI_Cut.h"
#include "Mod/Part/App/FCBRepAlgoAPI_Fuse.h"
#include "Mod/Part/App/FCBRepAlgoAPI_Section.h"
#include "Mod/Part/App/TopoShape.h"

#if OCC_VERSION_HEX >= 0x080000
static constexpr bool ExpectBooleanParallel = false;
#else
static constexpr bool ExpectBooleanParallel = true;
#endif

namespace
{

class BooleanParallelTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        savedParallelMode = BOPAlgo_Options::GetParallelMode();
    }

    void TearDown() override
    {
        // setBooleanParallelMode() writes booleanRunParallel(requested), not
        // the observed flag. On OCCT >= 8 that result is always false, so the
        // helper cannot put back a true this process already had. Restore the
        // GetParallelMode() value from SetUp with SetParallelMode directly.
        BOPAlgo_Options::SetParallelMode(savedParallelMode);
    }

    bool savedParallelMode = false;
};

TopoDS_Shape boxAt(double x, double y, double z)
{
    return BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), 10.0, 20.0, 30.0).Shape();
}

}  // namespace

TEST_F(BooleanParallelTest, helperFollowsOcctVersion)
{
    EXPECT_EQ(Part::booleanRunParallel(), ExpectBooleanParallel);
    EXPECT_EQ(Part::booleanRunParallel(true), ExpectBooleanParallel);
    EXPECT_FALSE(Part::booleanRunParallel(false));
}

TEST_F(BooleanParallelTest, constructorsReportPolicy)
{
    const TopoDS_Shape base = boxAt(0.0, 0.0, 0.0);
    const TopoDS_Shape tool = boxAt(5.0, 5.0, 5.0);
    const gp_Pln plane;

    FCBRepAlgoAPI_Fuse fuseDefault;
    FCBRepAlgoAPI_Fuse fuseShapes(base, tool);
    FCBRepAlgoAPI_Cut cutDefault;
    FCBRepAlgoAPI_Cut cutShapes(base, tool);
    FCBRepAlgoAPI_Common commonDefault;
    FCBRepAlgoAPI_Common commonShapes(base, tool);
    FCBRepAlgoAPI_Section sectionDefault;
    FCBRepAlgoAPI_Section sectionShapes(base, tool, false);
    FCBRepAlgoAPI_Section sectionPlane(base, plane, false);

    EXPECT_EQ(fuseDefault.RunParallel(), ExpectBooleanParallel);
    EXPECT_EQ(fuseShapes.RunParallel(), ExpectBooleanParallel);
    EXPECT_EQ(cutDefault.RunParallel(), ExpectBooleanParallel);
    EXPECT_EQ(cutShapes.RunParallel(), ExpectBooleanParallel);
    EXPECT_EQ(commonDefault.RunParallel(), ExpectBooleanParallel);
    EXPECT_EQ(commonShapes.RunParallel(), ExpectBooleanParallel);
    EXPECT_EQ(sectionDefault.RunParallel(), ExpectBooleanParallel);
    EXPECT_EQ(sectionShapes.RunParallel(), ExpectBooleanParallel);
    EXPECT_EQ(sectionPlane.RunParallel(), ExpectBooleanParallel);
}

TEST_F(BooleanParallelTest, analyzeLeavesGlobalModeAtPolicy)
{
    // Baseline for this case only. TearDown restores the mode from SetUp.
    Part::setBooleanParallelMode(false);
    Part::TopoShape shape(boxAt(0.0, 0.0, 0.0));
    std::ostringstream report;
    const bool valid = shape.analyze(true, report);
    EXPECT_TRUE(valid) << report.str();
    EXPECT_EQ(BOPAlgo_Options::GetParallelMode(), ExpectBooleanParallel);
}
