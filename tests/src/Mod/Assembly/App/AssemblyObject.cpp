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
#include <OndselSolver/ASMTAssembly.h>
#include <src/App/InitApplication.h>

namespace Assembly
{
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

    bool hasSolverSystem() const
    {
        return _assemblyObj->mbdAssembly && _assemblyObj->mbdAssembly->mbdSystem;
    }

    void discardSolverSystem()
    {
        _assemblyObj->mbdAssembly->mbdSystem.reset();
    }

    void makeSolveFixture(bool conflicting = false)
    {
        Base::Interpreter().runStringArg(
            "import FreeCAD as App, Assembly\n"
            "from unittest.mock import patch\n"
            "_c1_a = App.getDocument('%s').getObject('%s')\n"
            "_c1_base = _c1_a.newObject('Part::Box', 'Base')\n"
            "_c1_lid = _c1_a.newObject('Part::Box', 'Lid')\n"
            "_c1_a.Document.recompute()\n"
            "Assembly.createGroundedJoint(_c1_a, _c1_base, recompute=False)\n"
            "_c1_refs = (Assembly.makeJointReference(_c1_base, 'Face1'), "
            "Assembly.makeJointReference(_c1_lid, 'Face1'))\n"
            "_c1_joint = Assembly.createJoint(_c1_a, 'Fixed', *_c1_refs, solve=False, recompute=False)\n",
            getDocument()->getName(),
            getObject()->getNameInDocument()
        );
        if (conflicting) {
            Base::Interpreter().runString(
                "Assembly.createJoint(_c1_a, 'Distance', *_c1_refs, Distance=100, "
                "solve=False, presolve=False, recompute=False)\n"
            );
        }
    }

    void makeDragFixture()
    {
        makeSolveFixture();
        Base::Interpreter().runString(
            "with patch('JointObject.solveIfAllowed'):\n"
            "    _c1_joint.JointType = 'Slider'\n"
            "    _c1_joint.Detach1 = _c1_joint.Detach2 = True\n"
            "    _c1_joint.Placement1 = _c1_joint.Placement2 = App.Placement()\n"
            "_c2_follower = _c1_a.newObject('Part::Box', 'Follower')\n"
            "_c2_follower.recompute()\n"
            "Assembly.createJoint(_c1_a, 'Fixed', "
            "Assembly.makeJointReference(_c1_lid, 'Face1'), "
            "Assembly.makeJointReference(_c2_follower, 'Face1'), "
            "solve=False, presolve=False, recompute=False)\n"
        );
    }

    int pythonSolve(const char* patchExpression = nullptr)
    {
        Base::PyGILStateLocker lock;
        if (patchExpression) {
            Base::Interpreter().runStringArg(
                "with %s:\n    _c1_status = _c1_a.solve(False)\n", patchExpression
            );
        }
        else {
            Base::Interpreter().runString("_c1_status = _c1_a.solve(False)");
        }
        return static_cast<int>(Py::Long(Base::Interpreter().runStringObject("_c1_status")));
    }

    void expectValidationFailure(const char* patchExpression)
    {
        makeSolveFixture();
        EXPECT_EQ(pythonSolve(patchExpression), -7);
        EXPECT_EQ(getObject()->getLastSolverStatus(), -7);
        getObject()->updateSolveStatus();
        EXPECT_EQ(getObject()->getLastSolverStatus(), -7);
        // A subsequent successful validation must clear the failure.
        EXPECT_EQ(pythonSolve(), 0);
        EXPECT_EQ(getObject()->getLastSolverStatus(), 0);
    }

private:
    // TODO: use shared_ptr or something else here?
    Assembly::AssemblyObject* _assemblyObj;
    Assembly::JointGroup* _jointGroupObj;
    App::Document* _document = nullptr;
    std::string _docName;
};
}  // namespace Assembly

using Assembly::AssemblyObjectTest;

TEST_F(AssemblyObjectTest, createAssemblyObject)  // NOLINT
{
    // Arrange

    // Act

    // Assert
}

TEST_F(AssemblyObjectTest, unsolvedStatusRefreshIsObservational)  // NOLINT
{
    auto prefs = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly"
    );
    const bool previous = prefs->GetBool("SolveOnRecompute", true);
    prefs->SetBool("SolveOnRecompute", false);
    makeSolveFixture();
    prefs->SetBool("SolveOnRecompute", previous);
    // Group changes used to solve implicitly during fixture creation. Start the
    // refresh with no system even on that implementation.
    discardSolverSystem();
    Base::Interpreter().runString(
        "import os, tempfile\n"
        "_c1_lid.Placement.Base = App.Vector(30, 0, 0)\n"
        "for _c3_obj in _c1_a.Document.Objects:\n"
        "    _c3_obj.purgeTouched()\n"
        "def _c3_snapshot():\n"
        "    return [(o.Name, list(o.State), "
        "{p: tuple(getattr(o, p).toMatrix().A) for p in o.PropertiesList "
        "if o.getTypeIdOfProperty(p) == 'App::PropertyPlacement'}) "
        "for o in _c1_a.Document.Objects]\n"
        "_c3_path = os.path.join(tempfile.mkdtemp(), 'status.FCStd')\n"
        "_c1_a.Document.saveAs(_c3_path)\n"
    );
    EXPECT_FALSE(hasSolverSystem());
    EXPECT_FALSE(getObject()->hasSolveResult());
    EXPECT_FALSE(getDocument()->hasPendingFileChanges());
    EXPECT_FALSE(getDocument()->isTouched());
    int updates = 0;
    auto connection = getObject()->signalSolverUpdate.connect([&updates]() { ++updates; });
    for (const bool touched : {false, true}) {
        if (touched) {
            getDocument()->getObject("Lid")->touch();
        }
        const auto fileState = getDocument()->getFileChangeState();
        const auto fileChanges = getDocument()->getPendingFileChanges();
        const bool documentTouched = getDocument()->isTouched();
        std::vector<std::pair<App::Property*, unsigned long>> propertyStatuses;
        for (auto* object : getDocument()->getObjects()) {
            std::vector<App::Property*> properties;
            object->getPropertyList(properties);
            for (auto* property : properties) {
                propertyStatuses.emplace_back(property, property->getStatus());
            }
        }
        Base::Interpreter().runString("_c3_before = _c3_snapshot()");
        // Exercise the public Python refresh entry point as well as its C++ signal.
        Base::Interpreter().runString("_c1_a.updateSolveStatus()");
        Base::PyGILStateLocker lock;
        EXPECT_TRUE(Base::Interpreter().runStringObject("_c3_snapshot() == _c3_before").isTrue());
        EXPECT_EQ(getDocument()->getFileChangeState(), fileState);
        EXPECT_EQ(getDocument()->getPendingFileChanges().getFlags(), fileChanges.getFlags());
        EXPECT_EQ(getDocument()->isTouched(), documentTouched);
        for (const auto& [property, status] : propertyStatuses) {
            EXPECT_EQ(property->getStatus(), status) << property->getName();
        }
        EXPECT_FALSE(hasSolverSystem());
        EXPECT_EQ(getObject()->getLastDoF(), -1);  // Unknown until an explicit solve.
    }
    EXPECT_EQ(updates, 2);
    connection.disconnect();
    Base::Interpreter().runString("os.remove(_c3_path)\nos.rmdir(os.path.dirname(_c3_path))");
}

TEST_F(AssemblyObjectTest, statusRefreshPreservesLastExplicitSolve)  // NOLINT
{
    makeSolveFixture();
    ASSERT_EQ(pythonSolve(), 0);
    EXPECT_EQ(getObject()->getLastDoF(), 0);
    EXPECT_FALSE(getObject()->getLastHasConflicts());
    const auto redundant = getObject()->getLastRedundant();
    // Changing membership must not reinterpret the old solve using the new model.
    Base::Interpreter().runString("_c1_a.newObject('Part::Box', 'UnsolvedPart')");
    getObject()->updateSolveStatus();
    EXPECT_EQ(getObject()->getLastDoF(), 0);
    EXPECT_EQ(getObject()->getLastSolverStatus(), 0);
    EXPECT_FALSE(getObject()->getLastHasConflicts());
    EXPECT_EQ(getObject()->getLastRedundant(), redundant);
    ASSERT_EQ(pythonSolve(), 0);
    EXPECT_EQ(getObject()->getLastDoF(), 6);
}

TEST_F(AssemblyObjectTest, conflictingSolveAgreesWithStoredStatus)  // NOLINT
{
    makeSolveFixture(true);
    EXPECT_EQ(pythonSolve(), -3);
    EXPECT_EQ(getObject()->getLastSolverStatus(), -3);
    const int dof = getObject()->getLastDoF();
    const auto redundant = getObject()->getLastRedundant();
    Base::Interpreter().runString("_c1_a.newObject('Part::Box', 'UnsolvedPart')");
    getObject()->updateSolveStatus();
    EXPECT_EQ(getObject()->getLastSolverStatus(), -3);
    EXPECT_TRUE(getObject()->getLastHasConflicts());
    EXPECT_EQ(getObject()->getLastDoF(), dof);
    EXPECT_EQ(getObject()->getLastRedundant(), redundant);
}

TEST_F(AssemblyObjectTest, rejectedSolveCachesFailureAfterRestoringPlacements)  // NOLINT
{
    makeSolveFixture();
    ASSERT_EQ(pythonSolve(), 0);
    ASSERT_EQ(getObject()->getLastDoF(), 0);
    Base::Interpreter().runString("_c1_lid.Placement.Base = App.Vector(30, 0, 0)");
    auto* lid = getDocument()->getObject("Lid");
    const auto original = lid->getPlacementProperty()->getValue();
    int updates = 0;
    auto connection = getObject()->signalSolverUpdate.connect([&]() {
        ++updates;
        EXPECT_TRUE(lid->getPlacementProperty()->getValue().isSame(original));
        EXPECT_TRUE(getObject()->hasSolveResult());
        EXPECT_EQ(getObject()->getLastSolverStatus(), -7);
        EXPECT_EQ(getObject()->getLastDoF(), -1);
        EXPECT_TRUE(getObject()->getLastRedundant().empty());
    });
    EXPECT_EQ(pythonSolve("patch('UtilsAssembly.violatedJointLabels', return_value=None)"), -7);
    EXPECT_TRUE(hasSolverSystem());  // The rejected candidate has a solved system.
    getObject()->updateSolveStatus();
    EXPECT_EQ(updates, 2);
    connection.disconnect();
    ASSERT_EQ(pythonSolve(), 0);
    EXPECT_EQ(getObject()->getLastDoF(), 0);
}

TEST_F(AssemblyObjectTest, conflictingExecuteReportsFailure)  // NOLINT
{
    makeSolveFixture(true);
    auto prefs = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly"
    );
    const bool previous = prefs->GetBool("SolveOnRecompute", true);
    prefs->SetBool("SolveOnRecompute", true);
    auto* result = getObject()->execute();
    EXPECT_NE(result, App::DocumentObject::StdReturn);
    if (result != App::DocumentObject::StdReturn) {
        EXPECT_FALSE(result->Why.empty());
        delete result;
    }
    EXPECT_EQ(getObject()->getLastSolverStatus(), -3);
    prefs->SetBool("SolveOnRecompute", false);
    EXPECT_EQ(getObject()->execute(), App::DocumentObject::StdReturn);
    EXPECT_EQ(getObject()->getLastSolverStatus(), -3);
    prefs->SetBool("SolveOnRecompute", previous);
}

TEST_F(AssemblyObjectTest, validatorImportFailureIsFailure)  // NOLINT
{
    expectValidationFailure("patch.dict('sys.modules', {'UtilsAssembly': None})");
}

TEST_F(AssemblyObjectTest, validatorCallFailureIsFailure)  // NOLINT
{
    expectValidationFailure(
        "patch('UtilsAssembly.violatedJointLabels', "
        "side_effect=RuntimeError('C1 validator error'))"
    );
}

TEST_F(AssemblyObjectTest, validatorMissingFunctionIsFailure)  // NOLINT
{
    expectValidationFailure(
        "patch.dict('sys.modules', "
        "{'UtilsAssembly': __import__('types').ModuleType('UtilsAssembly')})"
    );
}

TEST_F(AssemblyObjectTest, validatorNonCallableIsFailure)  // NOLINT
{
    expectValidationFailure("patch('UtilsAssembly.violatedJointLabels', new=None)");
}

TEST_F(AssemblyObjectTest, validatorNonListIsFailure)  // NOLINT
{
    for (const char* value : {"None", "()", "{}", "False", "'invalid'"}) {
        SCOPED_TRACE(value);
        const std::string patchExpression =
            std::string("patch('UtilsAssembly.violatedJointLabels', return_value=") + value + ")";
        expectValidationFailure(patchExpression.c_str());
    }
}

TEST_F(AssemblyObjectTest, rejectedDragPreservesPlacementsAndTouchedState)  // NOLINT
{
    makeSolveFixture();
    auto* base = getDocument()->getObject("Base");
    ASSERT_NE(base, nullptr);
    getObject()->preDrag({base});
    // The drag input is already applied by the caller. The grounded solver body
    // still holds its original position, so this step must be rejected.
    base->getPlacementProperty()->setValue(
        Base::Placement(Base::Vector3d(43, 17, 9), Base::Rotation())
    );
    ASSERT_FALSE(getObject()->validateNewPlacements());
    std::vector<std::pair<App::Property*, unsigned long>> propertyStatuses;
    for (auto* object : getDocument()->getObjects()) {
        std::vector<App::Property*> properties;
        object->getPropertyList(properties);
        for (auto* property : properties) {
            propertyStatuses.emplace_back(property, property->getStatus());
        }
    }
    Base::Interpreter().runString(
        "def _c2_snapshot():\n"
        "    return [(o.Name, list(o.State), "
        "{p: tuple(getattr(o, p).toMatrix().A) for p in o.PropertiesList "
        "if o.getTypeIdOfProperty(p) == 'App::PropertyPlacement'}) "
        "for o in _c1_a.Document.Objects]\n"
        "_c2_before = _c2_snapshot()\n"
    );
    getObject()->doDragStep();
    for (const auto& [property, status] : propertyStatuses) {
        EXPECT_EQ(property->getStatus(), status) << property->getName();
    }
    Base::PyGILStateLocker lock;
    EXPECT_TRUE(Base::Interpreter().runStringObject("_c2_snapshot() == _c2_before").isTrue());
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
    // Supply a grounded, satisfiable assembly so the regression also exercises
    // successful solver writes alongside the tracked group normalisation.
    makeSolveFixture();
    ASSERT_EQ(pythonSolve(), 0);
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

TEST_F(AssemblyObjectTest, failedDragPreparationDoesNotRunStep)  // NOLINT
{
    makeDragFixture();
    auto* lid = getDocument()->getObject("Lid");
    const auto original = lid->getPlacementProperty()->getValue();
    Base::Interpreter().runString(
        "_c2_bad = patch('UtilsAssembly.violatedJointLabels', return_value=None)\n"
        "_c2_bad.start()\n"
    );
    getObject()->preDrag({lid});
    Base::Interpreter().runString("_c2_bad.stop()");
    ASSERT_EQ(getObject()->getLastSolverStatus(), -7);
    lid->getPlacementProperty()->setValue(
        Base::Placement(Base::Vector3d(0, 0, 12), Base::Rotation())
    );
    Base::Interpreter().runString(
        "_c2_redraw_patch = patch('JointObject.Joint.redrawJointPlacements')\n"
        "_c2_redraw = _c2_redraw_patch.start()\n"
    );
    getObject()->doDragStep();
    Base::Interpreter().runString("_c2_redraw_patch.stop()");
    Base::PyGILStateLocker lock;
    EXPECT_EQ(static_cast<int>(Py::Long(
        Base::Interpreter().runStringObject("_c2_redraw.call_count")
    )), 0);
    getObject()->postDrag();
    EXPECT_TRUE(lid->getPlacementProperty()->getValue().isSame(original));
}

TEST_F(AssemblyObjectTest, dragCompletionRejectsViolatedRelations)  // NOLINT
{
    makeDragFixture();
    auto* lid = getDocument()->getObject("Lid");
    const auto original = lid->getPlacementProperty()->getValue();
    getObject()->preDrag({lid});
    ASSERT_EQ(getObject()->getLastSolverStatus(), 0);
    lid->getPlacementProperty()->setValue(
        Base::Placement(Base::Vector3d(0, 0, 12), Base::Rotation())
    );
    getObject()->doDragStep();
    ASSERT_FALSE(lid->getPlacementProperty()->getValue().isSame(original));
    Base::Interpreter().runString(
        "_c2_bad = patch('UtilsAssembly.violatedJointLabels', return_value=['joint'])\n"
        "_c2_validator = _c2_bad.start()\n"
    );
    getObject()->postDrag();
    Base::Interpreter().runString("_c2_bad.stop()");
    EXPECT_EQ(getObject()->getLastSolverStatus(), -3);
    EXPECT_TRUE(getObject()->hasSolveResult());
    EXPECT_TRUE(getObject()->getLastHasConflicts());
    EXPECT_EQ(getObject()->getLastDoF(), -1);
    getObject()->updateSolveStatus();
    EXPECT_TRUE(getObject()->getLastHasConflicts());
    EXPECT_TRUE(lid->getPlacementProperty()->getValue().isSame(original))
        << "position delta: "
        << (lid->getPlacementProperty()->getValue().getPosition() - original.getPosition()).Length()
        << "; rotation restored: "
        << lid->getPlacementProperty()->getValue().getRotation().isSame(original.getRotation());
    Base::PyGILStateLocker lock;
    EXPECT_EQ(static_cast<int>(Py::Long(
        Base::Interpreter().runStringObject("_c2_validator.call_count")
    )), 1);
}

TEST_F(AssemblyObjectTest, dragStepsOnlyGuardSolverObjects)  // NOLINT
{
    makeDragFixture();
    auto* lid = getDocument()->getObject("Lid");
    auto* follower = getDocument()->getObject("Follower");
    auto* unrelated = getDocument()->addObject<App::FeatureTest>("Unrelated");
    getObject()->preDrag({lid});
    int inspected = 0;
    auto connection = getDocument()->signalBeforeChangeObject.connect(
        [&](const App::DocumentObject& object, const App::Property& property) {
            if (&object == follower && &property == follower->getPlacementProperty()) {
                ++inspected;
                EXPECT_FALSE(unrelated->testStatus(App::ObjectStatus::NoTouch));
            }
        }
    );
    // Moving the driver writes the bundled follower. Inspect both the first
    // snapshot and its reuse on the next step.
    for (int step = 1; step <= 2; ++step) {
        lid->getPlacementProperty()->setValue(
            Base::Placement(Base::Vector3d(0, 0, step), Base::Rotation())
        );
        inspected = 0;
        getObject()->doDragStep();
        EXPECT_GT(inspected, 0);
    }
    connection.disconnect();
    getObject()->postDrag();
}

TEST_F(AssemblyObjectTest, dragPlacementCallbackDefersNestedSolve)  // NOLINT
{
    makeDragFixture();
    auto* lid = getDocument()->getObject("Lid");
    auto* follower = getDocument()->getObject("Follower");
    getObject()->preDrag({lid});
    const auto solverPart = getObject()->getMbDPart(lid);
    lid->getPlacementProperty()->setValue(
        Base::Placement(Base::Vector3d(0, 0, 12), Base::Rotation())
    );
    bool requested = false;
    auto connection = getDocument()->signalBeforeChangeObject.connect(
        [&](const App::DocumentObject& object, const App::Property& property) {
            if (&object == follower && &property == follower->getPlacementProperty() && !requested) {
                requested = true;
                getObject()->solve();
            }
        }
    );
    getObject()->doDragStep();
    connection.disconnect();
    EXPECT_TRUE(requested);
    EXPECT_EQ(getObject()->getMbDPart(lid), solverPart);
    EXPECT_TRUE(getObject()->isTouched());
    getObject()->postDrag();
    EXPECT_TRUE(getObject()->isTouched());
    getObject()->purgeTouched();
    EXPECT_EQ(getObject()->mustExecute(), 1);
    EXPECT_EQ(getObject()->solve(), Assembly::SolveResult::Success);
    getObject()->purgeTouched();
    EXPECT_EQ(getObject()->mustExecute(), 0);
}

TEST_F(AssemblyObjectTest, dragCompletionRejectsValidatorFailure)  // NOLINT
{
    makeDragFixture();
    auto* lid = getDocument()->getObject("Lid");
    const auto original = lid->getPlacementProperty()->getValue();
    getObject()->preDrag({lid});
    lid->getPlacementProperty()->setValue(
        Base::Placement(Base::Vector3d(0, 0, 12), Base::Rotation())
    );
    getObject()->doDragStep();
    Base::Interpreter().runString(
        "_c2_bad = patch('UtilsAssembly.violatedJointLabels', "
        "side_effect=RuntimeError('drag validation'))\n"
        "_c2_bad.start()\n"
    );
    getObject()->postDrag();
    Base::Interpreter().runString("_c2_bad.stop()");
    EXPECT_EQ(getObject()->getLastSolverStatus(), -7);
    EXPECT_TRUE(lid->getPlacementProperty()->getValue().isSame(original))
        << "position delta: "
        << (lid->getPlacementProperty()->getValue().getPosition() - original.getPosition()).Length()
        << "; rotation restored: "
        << lid->getPlacementProperty()->getValue().getRotation().isSame(original.getRotation());
}

TEST_F(AssemblyObjectTest, dragCompletionValidatesOnce)  // NOLINT
{
    makeDragFixture();
    auto* lid = getDocument()->getObject("Lid");
    getObject()->preDrag({lid});
    Base::Interpreter().runString(
        "_c2_check_patch = patch('UtilsAssembly.violatedJointLabels', return_value=[])\n"
        "_c2_check = _c2_check_patch.start()\n"
    );
    for (int step = 1; step <= 2; ++step) {
        lid->getPlacementProperty()->setValue(
            Base::Placement(Base::Vector3d(0, 0, step), Base::Rotation())
        );
        getObject()->doDragStep();
    }
    {
        Base::PyGILStateLocker lock;
        EXPECT_EQ(static_cast<int>(Py::Long(
            Base::Interpreter().runStringObject("_c2_check.call_count")
        )), 0);
    }
    getObject()->postDrag();
    Base::Interpreter().runString("_c2_check_patch.stop()");
    Base::PyGILStateLocker lock;
    EXPECT_EQ(static_cast<int>(Py::Long(
        Base::Interpreter().runStringObject("_c2_check.call_count")
    )), 1);
    EXPECT_EQ(getObject()->getLastSolverStatus(), 0);
    EXPECT_NEAR(lid->getPlacementProperty()->getValue().getPosition().z, 2, 1e-6);
}

TEST_F(AssemblyObjectTest, geometryExceptionRejectsSolvePreparation)  // NOLINT
{
    makeSolveFixture();
    Base::Interpreter().runString(
        "import Part\n"
        "with patch('JointObject.solveIfAllowed'):\n"
        "    _c1_joint.JointType = 'Distance'\n"
        "    _c1_joint.Detach1 = _c1_joint.Detach2 = True\n"
        "    _c1_lid.Shape = Part.Shape()\n"
    );
    EXPECT_EQ(getObject()->solve(), Assembly::SolveResult::SolverFailure);
    EXPECT_EQ(getObject()->getLastSolverStatus(), -1);
    EXPECT_EQ(getObject()->getLastDoF(), -1);
}

TEST_F(AssemblyObjectTest, geometryExceptionRejectsIslandDragPreparation)  // NOLINT
{
    makeSolveFixture();
    Base::Interpreter().runString(
        "import Part\n"
        "with patch('JointObject.solveIfAllowed'):\n"
        "    _c1_base.setEditorMode('Placement', 0)\n"
        "    _c1_joint.JointType = 'Distance'\n"
        "    _c1_joint.Detach1 = _c1_joint.Detach2 = True\n"
        "    _c1_lid.Shape = Part.Shape()\n"
    );
    auto* lid = getDocument()->getObject("Lid");
    EXPECT_EQ(getObject()->preDrag({lid}), Assembly::SolveResult::SolverFailure);
    EXPECT_EQ(getObject()->getLastSolverStatus(), -1);
    EXPECT_FALSE(hasSolverSystem());
    getObject()->doDragStep();
    EXPECT_EQ(getObject()->postDrag(), Assembly::SolveResult::SolverFailure);
}
