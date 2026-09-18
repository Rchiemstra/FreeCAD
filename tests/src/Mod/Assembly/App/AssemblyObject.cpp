// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <FCConfig.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/Expression.h>
#include <App/FeatureTest.h>
#include <App/Link.h>
#include <App/ObjectIdentifier.h>
#include <Base/Interpreter.h>
#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Assembly/App/Groups.h>
#include <src/App/InitApplication.h>

class AssemblyObjectTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        Base::Interpreter().runString("import Part");
        Base::Interpreter().runString("import Material");
        Base::Interpreter().runString("import Spreadsheet");
        Base::Interpreter().runString("import AssemblyApp");
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("test");
        auto* _doc = App::GetApplication().newDocument(_docName.c_str(), "testUser");
        _document = _doc;
        try {
            _assemblyObj = _doc->addObject<Assembly::AssemblyObject>();
            _jointGroupObj = _assemblyObj->addObject<Assembly::JointGroup>("jointGroupTest");
        }
        catch (const Base::Exception& e) {
            FAIL() << "AssemblyObject setup failed: " << e.what();
        }
        catch (const std::exception& e) {
            FAIL() << "AssemblyObject setup failed(std): " << e.what();
        }
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_docName.c_str());
    }

    Assembly::AssemblyObject* getObject()
    {
        return _assemblyObj;
    }

    App::Document* getDocument() const
    {
        return _document;
    }

private:
    // TODO: use shared_ptr or something else here?
    Assembly::AssemblyObject* _assemblyObj;
    Assembly::JointGroup* _jointGroupObj;
    App::Document* _document = nullptr;
    std::string _docName;
};

TEST_F(AssemblyObjectTest, createAssemblyObject)  // NOLINT
{
    // Arrange

    // Act

    // Assert
}

TEST_F(AssemblyObjectTest, recomputeAppliesSolverWritesToMemberPlacements)  // NOLINT
{
    // AssemblyObject::execute() runs solve(), and the whole product of a solve
    // lands on objects *other* than the assembly: ensureIdentityPlacements()
    // normalises member link groups and setNewPlacements() moves every jointed
    // part. The isolated recompute worker publishes only the recomputed
    // target's own declared outputs, so a solve that runs in the detached
    // process converges and is then discarded, leaving the parts exactly where
    // they were -- silently, because the assembly still settles.
    //
    // ensureIdentityPlacements() forcing a displaced member link group back to
    // identity is the observable: it reaches the caller only if the solve ran
    // where its writes could be kept.

    // Arrange
    auto* assembly = getObject();
    ASSERT_NE(assembly, nullptr);
    auto* document = getDocument();
    ASSERT_NE(document, nullptr);

    // A link group needs something to link to: with ElementCount set but no
    // LinkedObject the link itself fails to recompute and never reaches the
    // solver.
    auto* linked = document->addObject<App::FeatureTest>("Linked");
    ASSERT_NE(linked, nullptr);
    document->recompute();

    auto* group = document->addObject<App::Link>("LinkGroup");
    ASSERT_NE(group, nullptr);
    group->LinkedObject.setValue(linked);
    group->ElementCount.setValue(1);
    ASSERT_TRUE(group->isLinkGroup());
    assembly->addObject(group);
    document->recompute();
    ASSERT_FALSE(assembly->isError()) << "assembly setup did not recompute cleanly";
    ASSERT_FALSE(group->isError()) << "link group setup did not recompute cleanly";

    // App::Link inherits getPlacementProperty() from both LinkBaseExtension
    // and DocumentObject. ensureIdentityPlacements() reaches it through a
    // DocumentObject*, so resolve the same overload here.
    App::DocumentObject* groupObject = group;
    auto* placement = groupObject->getPlacementProperty();
    ASSERT_NE(placement, nullptr);
    placement->setValue(Base::Placement(Base::Vector3d(10, 20, 30), Base::Rotation()));
    ASSERT_FALSE(placement->getValue().isIdentity());
    // Settle the member so the recompute is driven by the assembly itself.
    group->purgeTouched();
    assembly->touch();
    ASSERT_TRUE(assembly->isTouched());

    // Act
    document->recompute();

    // Assert
    EXPECT_TRUE(placement->getValue().isIdentity())
        << "the assembly settled without its solve reaching the caller: the solve ran "
           "in the worker and its writes to member placements were discarded";
    EXPECT_FALSE(assembly->isTouched());
}
