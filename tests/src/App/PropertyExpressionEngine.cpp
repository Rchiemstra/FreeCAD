// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>
#include <string>

#include "Base/Quantity.h"
#include "Base/Interpreter.h"

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentCollaborationService.h"
#include "App/DocumentObject.h"
#include "App/DocumentObjectGroup.h"
#include "App/Expression.h"
#include "App/FeatureTest.h"
#include "App/GenericIsolatedRecompute.h"
#include "App/RecomputeHandle.h"
#include "App/ObjectIdentifier.h"
#include "App/PropertyExpressionEngine.h"
#include "App/PropertyLinks.h"

#include "src/App/InitApplication.h"

// clang-format off

namespace
{
App::DocumentRecomputeId nextRecomputeId(App::Document& document)
{
    App::DocumentRecomputeId id = 1;
    while (document.recomputeCoordinator().status(id)) { ++id; }
    return id;
}

bool executedFailureSince(App::Document& document, App::DocumentRecomputeId id,
                          const std::string& name)
{
    while (const auto snapshot = document.recomputeCoordinator().status(id++)) {
        for (const auto& feature : snapshot->features) {
            if (feature.featureId == name && feature.executed
                && feature.state == App::DocumentRecomputeFeatureState::Failed) {
                return true;
            }
        }
    }
    return false;
}
}

class PropertyExpressionEngineTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _doc_name = App::GetApplication().getUniqueDocumentName("test");
        _this_doc = App::GetApplication().newDocument(_doc_name.c_str(), "testUser");
        _this_obj = _this_doc -> addObject("Sketcher::SketchObject");
        _source_name = std::string("this_origin");
        _source_prop = _this_obj -> addDynamicProperty("App::PropertyString", _source_name.c_str()); // property with length as string
        _target_name = std::string("this_length");
        _target_prop = _this_obj -> addDynamicProperty("App::PropertyLength", _target_name.c_str()); // property with reference to source
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_doc_name.c_str());
    }

    std::string doc_name() { return _doc_name; }
    App::Document* this_doc() { return _this_doc; }
    App::DocumentObject* this_obj() { return _this_obj; }
    std::string source_name() { return _source_name; }
    App::Property* source_prop() { return _source_prop; }
    std::string target_name() { return _target_name; }
    App::Property* target_prop() { return _target_prop; }

private:
    std::string _doc_name;
    App::Document* _this_doc {};
    App::DocumentObject* _this_obj {};
    std::string _source_name;
    App::Property* _source_prop {};
    std::string _target_name;
    App::Property* _target_prop {};
};

// https://github.com/FreeCAD/FreeCAD/issues/11965
TEST_F(PropertyExpressionEngineTest, executeCrossPropertyReference)
{
    auto source_text = std::string("1.5 m"); // provided in source
    auto target_text = std::string("1500 mm"); // expected in target

    auto source_path = App::ObjectIdentifier::parse(this_obj(), source_name());
    source_prop()->setPathValue(source_path, source_text);

    auto target_expr = "parsequant(" + source_name() + ")"; // Solution B: parsequant() function

    auto target_path = App::ObjectIdentifier::parse(this_obj(), target_name());
    std::shared_ptr<App::Expression> target_rule(App::Expression::parse(this_obj(), target_expr));
    this_obj()->setExpression(target_path, target_rule);

    this_obj() -> ExpressionEngine.execute();

    auto source_entry = source_prop() -> getPathValue(source_path);
    ASSERT_TRUE(source_entry.type() == typeid(std::string));
    auto source_value = App::any_cast<std::string>(source_entry);

    auto target_entry = target_prop() -> getPathValue(target_path);
    ASSERT_TRUE(target_entry.type() == typeid(Base::Quantity));
    auto target_quant = App::any_cast<Base::Quantity>(target_entry);
    auto target_value = target_quant.getValue();
    auto target_unit = target_quant.getUnit().getString();

    auto verify_quant = Base::Quantity::parse(target_text);

    EXPECT_EQ(target_quant, verify_quant) << ""
        "expecting equal: source_text='" + source_text + "' target_text='" + target_text + "'"
        "instead produced: target_value='" + std::to_string(target_value) + "' target_unit='" + target_unit + "'"
    ;
}

class CrossDocumentExpressionRecomputeTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _directory = std::filesystem::temp_directory_path()
            / ("fc-xdoc-expr-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(_directory);
        _sourceName = App::GetApplication().getUniqueDocumentName("QA_Hole");
        _drivenName = App::GetApplication().getUniqueDocumentName("QA_Driven");
        _source = App::GetApplication().newDocument(_sourceName.c_str(), "sourceUser");
        _driven = App::GetApplication().newDocument(_drivenName.c_str(), "drivenUser");
        _sourceObject = _source->addObject<App::FeatureTest>("Dims");
        _drivenObject = _driven->addObject<App::FeatureTest>("Pad");
        ASSERT_NE(_sourceObject, nullptr);
        ASSERT_NE(_drivenObject, nullptr);
        _sourceObject->Float.setValue(6.0);
        _drivenObject->Float.setValue(10.0);
        ASSERT_TRUE(_source->saveAs((_directory / "qa_hole.FCStd").string().c_str()));
        ASSERT_TRUE(_driven->saveAs((_directory / "qa_driven.FCStd").string().c_str()));
        const std::string expression =
            _sourceName + "#" + _sourceObject->getNameInDocument() + ".Float";
        _drivenObject->setExpression(
            App::ObjectIdentifier(_drivenObject->Float),
            std::shared_ptr<App::Expression>(
                App::Expression::parse(_drivenObject, expression)));
    }

    void TearDown() override
    {
        if (App::GetApplication().getDocument(_drivenName.c_str())) {
            App::GetApplication().closeDocument(_drivenName.c_str());
        }
        if (App::GetApplication().getDocument(_sourceName.c_str())) {
            App::GetApplication().closeDocument(_sourceName.c_str());
        }
        std::error_code error;
        std::filesystem::remove_all(_directory, error);
    }

    std::filesystem::path _directory;
    std::string _sourceName;
    std::string _drivenName;
    App::Document* _source {};
    App::Document* _driven {};
    App::FeatureTest* _sourceObject {};
    App::FeatureTest* _drivenObject {};
};

TEST_F(CrossDocumentExpressionRecomputeTest, openSourceDocumentDrivesTheDependentValue)
{
    _drivenObject->touch();
    EXPECT_NO_THROW(static_cast<void>(_driven->recompute()));
    EXPECT_FALSE(_drivenObject->isError());
    EXPECT_DOUBLE_EQ(_drivenObject->Float.getValue(), 6.0);

    _sourceObject->Float.setValue(9.0);
    _drivenObject->touch();
    EXPECT_NO_THROW(static_cast<void>(_driven->recompute()));
    EXPECT_FALSE(_drivenObject->isError());
    EXPECT_DOUBLE_EQ(_drivenObject->Float.getValue(), 9.0);
}

TEST_F(CrossDocumentExpressionRecomputeTest, sourceDestructionClearsBothPrimedCaches)
{
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    ASSERT_FALSE(_drivenObject->getOutList().empty());
    ASSERT_FALSE(_drivenObject->getOutListProp().empty());
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::Structural;
    const auto result = _source->collaborationService().commitCompatibilityMutationWithPolicy(
        mutation, [&] { _source->removeObject("Dims"); },
        App::CollaborationCompatibilityRecomputePolicy::Deferred);
    ASSERT_TRUE(result.committed()) << result.message;
    ASSERT_EQ(_source->getObject("Dims"), nullptr);
    _source->clearUndos();
    _sourceObject = nullptr;
    // Do not dereference stale pointers on the unfixed build. The fixed build
    // must reach recompute after the source has actually been destroyed.
    ASSERT_TRUE(_drivenObject->getOutList().empty());
    ASSERT_TRUE(_drivenObject->getOutListProp().empty());
    EXPECT_NO_THROW(static_cast<void>(_driven->recompute()));
    EXPECT_TRUE(_drivenObject->isError());
    EXPECT_EQ(_drivenObject->ExpressionEngine.numExpressions(), 1U);
}

TEST_F(CrossDocumentExpressionRecomputeTest, ordinaryDeletionKeepsForeignTransactionCapture)
{
    int notifications = 0;
    auto connection = _driven->signalChangedObject.connect(
        [&](const App::DocumentObject& owner, const App::Property& property) {
            if (&owner == _drivenObject && &property == &_drivenObject->ExpressionEngine) {
                ++notifications;
            }
        });
    _driven->openTransaction("Record foreign expression retirement");
    _source->removeObject("Dims");
    _sourceObject = nullptr;
    _driven->commitTransaction();
    EXPECT_GT(notifications, 0);
    EXPECT_EQ(_driven->getAvailableUndoNames(),
              std::vector<std::string>({"Record foreign expression retirement"}));
    EXPECT_TRUE(_drivenObject->getOutList().empty());
    EXPECT_TRUE(_drivenObject->getOutListProp().empty());
    EXPECT_EQ(_drivenObject->ExpressionEngine.numExpressions(), 1U);
}

TEST_F(CrossDocumentExpressionRecomputeTest, deletionUndoRedoRestoresForeignDependency)
{
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    _source->openTransaction("Delete external source");
    _source->removeObject("Dims");
    _source->commitTransaction();
    ASSERT_TRUE(_source->undo());
    ASSERT_EQ(_source->getObject("Dims"), _sourceObject);
    ASSERT_NE(std::find(_drivenObject->getOutList().begin(),
                        _drivenObject->getOutList().end(), _sourceObject),
              _drivenObject->getOutList().end());
    ASSERT_FALSE(_drivenObject->getOutListProp().empty());
    ASSERT_FALSE(_sourceObject->getInListProp().empty());
    static_cast<void>(_driven->recompute());
    _sourceObject->Float.setValue(19.0);
    static_cast<void>(_source->recompute());
    EXPECT_TRUE(_drivenObject->isTouched());
    static_cast<void>(_driven->recompute());
    EXPECT_DOUBLE_EQ(_drivenObject->Float.getValue(), 19.0);
    ASSERT_TRUE(_source->redo());
    EXPECT_EQ(_source->getObject("Dims"), nullptr);
    EXPECT_TRUE(_drivenObject->getOutList().empty());
    EXPECT_TRUE(_drivenObject->getOutListProp().empty());
    EXPECT_TRUE(_sourceObject->getInListProp().empty());
    ASSERT_TRUE(_source->undo());
    _sourceObject->Float.setValue(23.0);
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    EXPECT_DOUBLE_EQ(_drivenObject->Float.getValue(), 23.0);
}

// Spreadsheet Cells is a second PropertyExpressionContainer, independent of
// the object's built-in ExpressionEngine. Keep both in the lifecycle repro.
class CrossDocumentSpreadsheetLifecycleTest:
    public CrossDocumentExpressionRecomputeTest,
    public ::testing::WithParamInterface<bool>
{
protected:
    void addSheet()
    {
        Base::PyGILStateLocker gil;
        PyObject* module = PyImport_ImportModule("Spreadsheet");
        if (!module) { PyErr_Print(); }
        ASSERT_NE(module, nullptr);
        Py_DECREF(module);
        sheet = _driven->addObject("Spreadsheet::Sheet", "Bridge");
        ASSERT_NE(sheet, nullptr);
        setCell("=" + _sourceName + "#Dims.Float");
    }

    void setCell(const std::string& value)
    {
        Base::PyGILStateLocker gil;
        Py::Object object(sheet->getPyObject(), true);
        PyObject* result = PyObject_CallMethod(object.ptr(), "set", "ss", "A1", value.c_str());
        if (!result) { PyErr_Print(); }
        ASSERT_NE(result, nullptr);
        Py_DECREF(result);
    }

    App::DocumentObject* sheet {};
};

TEST_P(CrossDocumentSpreadsheetLifecycleTest, atomicDeletionRetiresEveryExpressionContainer)
{
    if (GetParam()) { addSheet(); }
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    ASSERT_FALSE(_driven->mustExecute());
    if (sheet) {
        ASSERT_FALSE(sheet->getOutList().empty());
        static_cast<void>(sheet->getOutListProp());
    }
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::Structural;
    auto result = _source->collaborationService().commitCompatibilityMutationWithPolicy(
        mutation, [&] { _source->removeObject("Dims"); },
        App::CollaborationCompatibilityRecomputePolicy::Deferred);
    ASSERT_TRUE(result.committed()) << result.message;
    ASSERT_EQ(_source->getObject("Dims"), nullptr);
    EXPECT_TRUE(_drivenObject->getOutList().empty());
    if (sheet) {
        EXPECT_TRUE(sheet->getOutList().empty());
        EXPECT_TRUE(sheet->getOutListProp().empty());
    }
    ASSERT_TRUE(_source->undo());
    ASSERT_EQ(_source->getObject("Dims"), _sourceObject);
    if (sheet) { EXPECT_FALSE(sheet->getOutList().empty()); }
    _sourceObject->Float.setValue(18);
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    EXPECT_DOUBLE_EQ(_drivenObject->Float.getValue(), 18);
    if (sheet) {
        Base::PyGILStateLocker gil;
        Py::Object object(sheet->getPyObject(), true);
        Py::Object value(PyObject_GetAttrString(object.ptr(), "A1"), true);
        ASSERT_FALSE(value.isNull());
        EXPECT_DOUBLE_EQ(PyFloat_AsDouble(value.ptr()), 18);
        ASSERT_FALSE(PyErr_Occurred());
    }
    ASSERT_TRUE(_source->redo());
    ASSERT_EQ(_source->getObject("Dims"), nullptr);
    if (sheet) { EXPECT_TRUE(sheet->getOutList().empty()); }
}

TEST_P(CrossDocumentSpreadsheetLifecycleTest, terminalSheetErrorAllowsExpressionRepair)
{
    addSheet();
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    ASSERT_FALSE(_driven->mustExecute());
    ASSERT_TRUE(_driven->save());
    if (GetParam()) {
        App::GetApplication().closeDocument(_sourceName.c_str());
        _source = nullptr;
        _sourceObject = nullptr;
        bool error = false;
        _driven->recompute({}, true, &error);
        ASSERT_TRUE(error);
        ASSERT_TRUE(sheet->isError());
        {
            Base::PyGILStateLocker gil;
            Py::Object object(sheet->getPyObject(), true);
            Py::Object dirty(PyObject_CallMethod(object.ptr(), "getDirtyCells", nullptr), true);
            ASSERT_FALSE(dirty.isNull());
            EXPECT_EQ(PyList_Size(dirty.ptr()), 0)
                << "settled cell errors must retain diagnostics without pending cell work";
        }
        ASSERT_FALSE(_driven->recomputeCoordinator().hasPendingWork());
    }
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
    mutation.objectName = _drivenObject->getNameInDocument();
    mutation.stableObjectIdentity = _driven->collaborationObjectIdentity(*_drivenObject);
    const auto result = _driven->collaborationService().commitCompatibilityMutation(
        mutation, [&] {
            // A read-only copy retains the owner's pointer; destroying that
            // detached property must not request work on the canonical sheet.
            const std::unique_ptr<App::Property> cellsSnapshot(
                sheet->getPropertyByName("cells")->Copy());
            _drivenObject->setExpression(App::ObjectIdentifier(_drivenObject->Float), nullptr);
            _drivenObject->Float.setValue(21);
        });
    ASSERT_TRUE(result.committed()) << result.message;
    EXPECT_FALSE(_driven->getMutationReadiness().quarantined);
    EXPECT_DOUBLE_EQ(_drivenObject->Float.getValue(), 21);
    EXPECT_FALSE(sheet->isTouched()) << "unchanged terminal errors must be settled before postflight";
    mutation.objectName = sheet->getNameInDocument();
    mutation.stableObjectIdentity = _driven->collaborationObjectIdentity(*sheet);
    const auto repaired = _driven->collaborationService().commitCompatibilityMutation(
        mutation, [&] { setCell("24"); });
    EXPECT_TRUE(repaired.committed()) << repaired.message;
    EXPECT_FALSE(sheet->isError());
    EXPECT_FALSE(_driven->mustExecute());
}

TEST_P(CrossDocumentSpreadsheetLifecycleTest, sheetOnlyUndoAdmissionRetainsHistory)
{
    _drivenObject->setExpression(App::ObjectIdentifier(_drivenObject->Float), nullptr);
    addSheet();
    if (GetParam()) { setCell("=hiddenref(" + _sourceName + "#Dims.Float)"); }
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::Structural;
    auto result = _source->collaborationService().commitCompatibilityMutationWithPolicy(
        mutation, [&] { _source->removeObject("Dims"); },
        App::CollaborationCompatibilityRecomputePolicy::Deferred);
    ASSERT_TRUE(result.committed()) << result.message;
    const auto undos = _source->getAvailableUndoNames();
    const auto redos = _source->getAvailableRedoNames();
    _driven->setStatus(App::Document::Recomputing, true);
    bool refused = false;
    try { refused = !_source->undo(); }
    catch (const Base::Exception& error) {
        refused = true;
        EXPECT_NE(std::string(error.what()).find("dependent document"), std::string::npos);
    }
    _driven->setStatus(App::Document::Recomputing, false);
    EXPECT_TRUE(refused);
    EXPECT_EQ(_source->getAvailableUndoNames(), undos);
    EXPECT_EQ(_source->getAvailableRedoNames(), redos);
    ASSERT_EQ(_source->getObject("Dims"), nullptr);
    ASSERT_TRUE(_source->undo());
    std::vector<App::DocumentObject*> links;
    auto* cells = dynamic_cast<App::PropertyLinkBase*>(sheet->getPropertyByName("cells"));
    ASSERT_NE(cells, nullptr);
    cells->getLinks(links, true);
    EXPECT_EQ(links, std::vector<App::DocumentObject*>({_sourceObject}));
}

TEST_P(CrossDocumentSpreadsheetLifecycleTest, terminalErrorDoesNotDiscardFreshDirtyCell)
{
    addSheet();
    App::DocumentObject* fresh = GetParam() ? sheet
        : _driven->addObject("Spreadsheet::Sheet", "Fresh");
    {
        Base::PyGILStateLocker gil;
        Py::Object object(fresh->getPyObject(), true);
        PyObject* result = PyObject_CallMethod(object.ptr(), "set", "ss", "B1", "42");
        ASSERT_NE(result, nullptr);
        Py_DECREF(result);
    }
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    App::GetApplication().closeDocument(_sourceName.c_str());
    _source = nullptr;
    _sourceObject = nullptr;
    bool error = false;
    _driven->recompute({}, true, &error);
    ASSERT_TRUE(error);
    ASSERT_TRUE(sheet->isError());
    ASSERT_TRUE(sheet->isTouched());
    // Make the evaluated output stale without changing its cell expression.
    // touchCells only changes the dirty-cell cache, with no undo record.
    auto* property = fresh->getPropertyByName("B1");
    ASSERT_NE(property, nullptr);
    if (auto* output = dynamic_cast<App::PropertyInteger*>(property)) {
        output->setValue(99);
    }
    else {
        auto* floatOutput = dynamic_cast<App::PropertyFloat*>(property);
        ASSERT_NE(floatOutput, nullptr) << property->getTypeId().getName();
        floatOutput->setValue(99);
    }
    if (fresh != sheet) { fresh->purgeTouched(); }
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
    mutation.objectName = fresh->getNameInDocument();
    mutation.stableObjectIdentity = _driven->collaborationObjectIdentity(*fresh);
    App::DocumentRecomputeId firstRecompute = 0;
    const auto result = _driven->collaborationService().commitCompatibilityMutation(
        mutation, [&] {
            firstRecompute = nextRecomputeId(*_driven);
            Base::PyGILStateLocker gil;
            Py::Object object(fresh->getPyObject(), true);
            PyObject* result = PyObject_CallMethod(object.ptr(), "touchCells", "s", "B1");
            ASSERT_NE(result, nullptr);
            Py_DECREF(result);
            ASSERT_TRUE(fresh->mustExecute());
        });
    if (fresh == sheet) {
        EXPECT_TRUE(executedFailureSince(*_driven, firstRecompute, sheet->getNameInDocument()));
        EXPECT_EQ(result.status, App::DocumentCommitStatus::RecomputeFailed) << result.message;
        EXPECT_NE(result.message.find("document recompute reported an object error"), std::string::npos)
            << result.message;
    }
    else {
        ASSERT_TRUE(result.committed()) << result.message;
        Base::PyGILStateLocker gil;
        Py::Object object(fresh->getPyObject(), true);
        Py::Object value(PyObject_GetAttrString(object.ptr(), "B1"), true);
        ASSERT_FALSE(value.isNull());
        EXPECT_DOUBLE_EQ(PyFloat_AsDouble(value.ptr()), 42);
        EXPECT_FALSE(fresh->mustExecute());
    }
    EXPECT_FALSE(_driven->getMutationReadiness().quarantined);
}

INSTANTIATE_TEST_SUITE_P(Round25, CrossDocumentSpreadsheetLifecycleTest, ::testing::Bool(),
    [](const auto& info) { return info.param ? "Defect" : "Happy"; });

class CrossDocumentReplayAdmissionTest:
    public CrossDocumentExpressionRecomputeTest,
    public ::testing::WithParamInterface<bool>
{};

TEST_P(CrossDocumentReplayAdmissionTest, busyDependentDoesNotConsumeHistory)
{
    auto* surviving = _source->addObject<App::FeatureTest>("Surviving");
    surviving->Float.setValue(2.0);
    _drivenObject->setExpression(App::ObjectIdentifier(_drivenObject->Float),
        std::shared_ptr<App::Expression>(App::Expression::parse(_drivenObject,
            _sourceName + "#Dims.Float + " + _sourceName + "#Surviving.Float")));
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    App::DocumentObject* replaySource = _sourceObject;
    _source->openTransaction(GetParam() ? "Delete source" : "Add source");
    if (GetParam()) {
        _source->removeObject("Dims");
    }
    else {
        replaySource = _source->addObject<App::FeatureTest>("Added");
        _drivenObject->setExpression(App::ObjectIdentifier(_drivenObject->Float),
            std::shared_ptr<App::Expression>(App::Expression::parse(_drivenObject,
                _sourceName + "#Added.Float + " + _sourceName + "#Surviving.Float")));
    }
    _source->commitTransaction();
    if (GetParam()) {
        ASSERT_TRUE(_source->undo());
    }
    const auto undos = _source->getAvailableUndoNames();
    const auto redos = _source->getAvailableRedoNames();
    const auto value = _drivenObject->Float.getValue();
    _driven->setStatus(App::Document::Recomputing, true);
    bool refused = false;
    try {
        refused = !(GetParam() ? _source->redo() : _source->undo());
    }
    catch (const Base::Exception& error) {
        refused = true;
        EXPECT_NE(std::string(error.what()).find("dependent document"), std::string::npos);
    }
    _driven->setStatus(App::Document::Recomputing, false);
    EXPECT_TRUE(refused);
    EXPECT_EQ(_source->getAvailableUndoNames(), undos);
    EXPECT_EQ(_source->getAvailableRedoNames(), redos);
    EXPECT_EQ(_source->getObject(GetParam() ? "Dims" : "Added"),
              replaySource);
    EXPECT_EQ(_source->getObject("Surviving"), surviving);
    EXPECT_DOUBLE_EQ(_drivenObject->Float.getValue(), value);
    // Only retry when history was retained, so the unfixed test cannot assert
    // inside redo() on an empty history stack.
    ASSERT_EQ(_source->getAvailableRedoNames(), redos);
    ASSERT_EQ(_source->getAvailableUndoNames(), undos);
    EXPECT_TRUE(GetParam() ? _source->redo() : _source->undo());
    EXPECT_EQ(_source->getObject(GetParam() ? "Dims" : "Added"), nullptr);
}

INSTANTIATE_TEST_SUITE_P(ExternalSource, CrossDocumentReplayAdmissionTest, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Redo" : "Undo"; });

TEST_F(CrossDocumentExpressionRecomputeTest, hiddenSourceRemovalWaitsForInFlightCompletion)
{
    _drivenObject->setExpression(App::ObjectIdentifier(_drivenObject->Float),
        std::shared_ptr<App::Expression>(App::Expression::parse(_drivenObject,
            "hiddenref(" + _sourceName + "#Dims.Float)")));
    auto* worker = _driven->addObject<App::FeatureTestColumn>("Worker");
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    std::vector<App::DocumentObject*> hiddenLinks;
    _drivenObject->ExpressionEngine.getLinks(hiddenLinks, true);
    ASSERT_EQ(hiddenLinks, std::vector<App::DocumentObject*>({_sourceObject}));
    ASSERT_TRUE(_drivenObject->getOutList().empty());
    auto pending = _driven->recomputeAsync({worker}, true, 0, App::RecomputeVenue::Isolated);
    ASSERT_TRUE(_driven->recomputeCoordinator().hasPendingWork());
    bool refused = false;
    try { _source->removeObject("Dims"); }
    catch (const Base::Exception& error) {
        refused = true;
        EXPECT_NE(std::string(error.what()).find("dependent document"), std::string::npos);
    }
    EXPECT_TRUE(refused);
    EXPECT_EQ(_source->getObject("Dims"), _sourceObject);
    const auto completed = pending->wait(std::chrono::seconds(30));
    EXPECT_EQ(completed.state, App::DocumentRecomputeState::Completed);
    ASSERT_FALSE(_driven->recomputeCoordinator().hasPendingWork());
    if (!refused) { _sourceObject = nullptr; return; }
    EXPECT_FALSE(_sourceObject->testStatus(App::ObjectStatus::Remove));
    EXPECT_NO_THROW(_source->removeObject("Dims"));
    EXPECT_EQ(_source->getObject("Dims"), nullptr);
    _sourceObject = nullptr;
    hiddenLinks.clear();
    _drivenObject->ExpressionEngine.getLinks(hiddenLinks, true);
    EXPECT_TRUE(hiddenLinks.empty());
    EXPECT_EQ(_drivenObject->ExpressionEngine.numExpressions(), 1U);
}

// Source pins must end with evaluation; deletion must invalidate, not erase,
// a surviving external expression, so clearing it can repair the dependent.
class CrossDocumentSourceDeletionTest:
    public CrossDocumentExpressionRecomputeTest,
    public ::testing::WithParamInterface<bool>
{};

TEST_P(CrossDocumentSourceDeletionTest, settledSourceCanBeDeleted)
{
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    ASSERT_NO_THROW(_source->addObject("App::FeatureTest", "AfterEvaluation"));
    if (!GetParam()) {
        _drivenObject->setExpression(App::ObjectIdentifier(_drivenObject->Float), nullptr);
        static_cast<void>(_driven->recompute());
    }
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::Structural;
    const auto result = _source->collaborationService().commitCompatibilityMutationWithPolicy(
        mutation, [&] { _source->removeObject("Dims"); },
        App::CollaborationCompatibilityRecomputePolicy::Deferred);
    EXPECT_TRUE(result.committed()) << result.message;
    EXPECT_EQ(_source->getObject("Dims"), nullptr);
    EXPECT_NO_THROW(_source->addObject("App::FeatureTest", "AfterDeletion"));
    if (!result.committed()) {
        ASSERT_NE(_source->getObject("Dims"), nullptr);
        EXPECT_FALSE(_source->getObject("Dims")->testStatus(App::ObjectStatus::Remove));
        return;
    }
    _sourceObject = nullptr;
    if (GetParam()) {
        EXPECT_EQ(_drivenObject->ExpressionEngine.getExpressions().size(), 1U);
        EXPECT_TRUE(_drivenObject->isTouched());
        static_cast<void>(_driven->recompute());
        EXPECT_TRUE(_drivenObject->isError());
        _drivenObject->setExpression(App::ObjectIdentifier(_drivenObject->Float), nullptr);
        _drivenObject->Float.setValue(12);
        static_cast<void>(_driven->recompute());
        EXPECT_FALSE(_drivenObject->isError());
        EXPECT_DOUBLE_EQ(_drivenObject->Float.getValue(), 12);
    }
}

TEST_F(CrossDocumentExpressionRecomputeTest, failedRemovalDoesNotLeaveRecursiveDeletionFlag)
{
    _drivenObject->setExpression(App::ObjectIdentifier(_drivenObject->Float), nullptr);
    auto connection = _source->signalDeletedObject.connect([](const App::DocumentObject&) {
        throw std::runtime_error("injected removal failure");
    });
    EXPECT_THROW(_source->removeObject("Dims"), std::runtime_error);
    ASSERT_EQ(_source->getObject("Dims"), _sourceObject);
    EXPECT_FALSE(_sourceObject->testStatus(App::ObjectStatus::Remove));
    connection.disconnect();
    EXPECT_NO_THROW(_source->removeObject("Dims"));
    EXPECT_EQ(_source->getObject("Dims"), nullptr);
    _sourceObject = nullptr;
}

TEST_F(CrossDocumentExpressionRecomputeTest, sourceRemovalWhileDependentRecomputesRaisesAndCanRetry)
{
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    _driven->setStatus(App::Document::Recomputing, true);
    try {
        _source->removeObject("Dims");
        FAIL() << "removed an external source during dependent recompute";
    }
    catch (const Base::Exception& error) {
        EXPECT_NE(std::string(error.what()).find("dependent document"), std::string::npos)
            << error.what();
    }
    _driven->setStatus(App::Document::Recomputing, false);
    ASSERT_EQ(_source->getObject("Dims"), _sourceObject);
    EXPECT_FALSE(_sourceObject->testStatus(App::ObjectStatus::Remove));
    EXPECT_NO_THROW(_source->removeObject("Dims"));
    EXPECT_EQ(_source->getObject("Dims"), nullptr);
    _sourceObject = nullptr;
}

INSTANTIATE_TEST_SUITE_P(Round24, CrossDocumentSourceDeletionTest, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) {
        return info.param ? "DefectActiveBinding" : "HappyClearedBinding";
    });

TEST_F(CrossDocumentExpressionRecomputeTest,
       collaborativeCommitTellsTheCallerToRecomputeDirectly)
{
    // FeatureTest opts out of worker recompute, so it never reaches the
    // closure check. A worker-capable object with a cross-document input does.
    auto* sourceColumn = _source->addObject<App::FeatureTestColumn>("Column");
    auto* drivenColumn = _driven->addObject<App::FeatureTestColumn>("DrivenColumn");
    ASSERT_NE(sourceColumn, nullptr);
    ASSERT_NE(drivenColumn, nullptr);
    ASSERT_TRUE(drivenColumn->canRecomputeOnWorker());
    sourceColumn->Value.setValue(6);
    drivenColumn->Value.setValue(10);
    EXPECT_NO_THROW(static_cast<void>(_source->recompute()));
    _drivenObject->touch();
    EXPECT_NO_THROW(static_cast<void>(_driven->recompute()));
    ASSERT_FALSE(_driven->mustExecute());

    const std::string expression =
        _sourceName + "#" + sourceColumn->getNameInDocument() + ".Value";
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::Structural;
    const auto result = _driven->collaborationService().commitCompatibilityMutation(
        mutation,
        [&] {
            drivenColumn->setExpression(
                App::ObjectIdentifier(drivenColumn->Value),
                std::shared_ptr<App::Expression>(
                    App::Expression::parse(drivenColumn, expression)));
        });

    EXPECT_EQ(result.status, App::DocumentCommitStatus::RecomputeFailed) << result.message;
    EXPECT_NE(result.message.find("unresolved cross-document dependency"), std::string::npos)
        << result.message;
    EXPECT_NE(result.message.find("outside this collaborative commit"), std::string::npos)
        << result.message;
    EXPECT_EQ(drivenColumn->Value.getValue(), 10);
    EXPECT_TRUE(drivenColumn->ExpressionEngine.getExpressions().empty());

    drivenColumn->setExpression(
        App::ObjectIdentifier(drivenColumn->Value),
        std::shared_ptr<App::Expression>(
            App::Expression::parse(drivenColumn, expression)));
    drivenColumn->touch();
    EXPECT_NO_THROW(static_cast<void>(_driven->recompute()));
    EXPECT_FALSE(drivenColumn->isError());
    EXPECT_EQ(drivenColumn->Value.getValue(), 6);
}

TEST_F(CrossDocumentExpressionRecomputeTest, closedSourceDocumentReportsUnresolvedLink)
{
    EXPECT_EQ(_driven->recompute(), 1);
    ASSERT_DOUBLE_EQ(_drivenObject->Float.getValue(), 6.0);

    App::GetApplication().closeDocument(_sourceName.c_str());
    _source = nullptr;
    _sourceObject = nullptr;

    // The expression still names the closed document. Recompute must report
    // that unresolved link instead of walking the destroyed object.
    EXPECT_NO_THROW(static_cast<void>(
        _driven->recompute(std::vector<App::DocumentObject*>(), true)));
    EXPECT_TRUE(_drivenObject->isError());
    const char* diagnostic = _driven->getErrorDescription(_drivenObject);
    ASSERT_NE(diagnostic, nullptr);
    const std::string diagnosticText(diagnostic);
    EXPECT_NE(diagnosticText.find("not found"), std::string::npos) << diagnosticText;
    EXPECT_NE(diagnosticText.find(_sourceName), std::string::npos) << diagnosticText;

    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
    mutation.objectName = _drivenObject->getNameInDocument();
    const auto committed = _driven->collaborationService().commitCompatibilityMutation(
        mutation,
        [this] {
            _drivenObject->touch();
        });
    EXPECT_NE(committed.status, App::DocumentCommitStatus::RollbackFailed) << committed.message;
    EXPECT_TRUE(_drivenObject->isError());
    diagnostic = _driven->getErrorDescription(_drivenObject);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_NE(std::string(diagnostic).find("not found"), std::string::npos) << diagnostic;

    // A later open of a copy under whatever name the file restores must not
    // resurrect the destroyed object pointer.
    const auto copyPath = _directory / "qa_hole_good.FCStd";
    std::filesystem::copy_file(_directory / "qa_hole.FCStd", copyPath);
    auto* reopened = App::GetApplication().openDocument(copyPath.string().c_str());
    ASSERT_NE(reopened, nullptr);
    EXPECT_NO_THROW(static_cast<void>(
        _driven->recompute(std::vector<App::DocumentObject*>(), true)));
    if (std::string(reopened->getName()) != _sourceName) {
        EXPECT_TRUE(_drivenObject->isError());
    }

    if (App::GetApplication().getDocument(reopened->getName())) {
        App::GetApplication().closeDocument(reopened->getName());
    }
}

class ClosedSourceRepairTest:
    public CrossDocumentExpressionRecomputeTest,
    public ::testing::WithParamInterface<bool>
{
protected:
    void checkFailedEdit(bool purge);
};

void ClosedSourceRepairTest::checkFailedEdit(bool purge)
{
    _driven->removeObject(_drivenObject->getNameInDocument());
    _source->removeObject(_sourceObject->getNameInDocument());
    auto* source = _source->addObject<App::FeatureTestColumn>("SourceColumn");
    auto* driven = _driven->addObject<App::FeatureTestColumn>("DrivenColumn");
    source->Column.setValue("C");
    driven->setExpression(App::ObjectIdentifier(driven->Column),
        std::shared_ptr<App::Expression>(App::Expression::parse(
            driven, _sourceName + "#SourceColumn.Column")));
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    ASSERT_FALSE(_driven->mustExecute());
    if (GetParam()) {
        App::GetApplication().closeDocument(_sourceName.c_str());
        _source = nullptr;
        _sourceObject = nullptr;
        bool error = false;
        _driven->recompute({}, true, &error);
        ASSERT_TRUE(error);
        ASSERT_TRUE(driven->isError());
        // Match the settled GUI boundary: the error persists without pending work.
        if (purge) { _driven->purgeTouched(); }
        else { ASSERT_TRUE(driven->isTouched()); }
    }
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
    mutation.objectName = driven->getNameInDocument();
    mutation.stableObjectIdentity = _driven->collaborationObjectIdentity(*driven);
    App::DocumentRecomputeId firstRecompute = 0;
    auto result = _driven->collaborationService().commitCompatibilityMutation(
        mutation, [&] {
            firstRecompute = nextRecomputeId(*_driven);
            driven->touch();
        });
    EXPECT_EQ(result.status, GetParam() ? App::DocumentCommitStatus::RecomputeFailed
                                       : App::DocumentCommitStatus::Committed)
        << result.message;
    if (GetParam()) {
        EXPECT_TRUE(executedFailureSince(*_driven, firstRecompute, driven->getNameInDocument()));
        // This diagnostic is produced only after the authoritative recompute
        // reports an object error, rather than a pending-work guard refusing it.
        EXPECT_NE(result.message.find("document recompute reported an object error"), std::string::npos)
            << result.message;
        EXPECT_NE(result.message.find(_sourceName), std::string::npos) << result.message;
    }
    EXPECT_FALSE(_driven->getMutationReadiness().quarantined);
    result = _driven->collaborationService().commitCompatibilityMutation(
        mutation, [&] {
            driven->setExpression(App::ObjectIdentifier(driven->Column), nullptr);
            driven->Column.setValue("D");
        });
    EXPECT_EQ(result.status, App::DocumentCommitStatus::Committed) << result.message;
    EXPECT_FALSE(driven->isError());
    EXPECT_EQ(driven->Value.getValue(), App::decodeColumn("D"));
}

TEST_P(ClosedSourceRepairTest, failedEditDoesNotFenceExpressionRepair)
{
    checkFailedEdit(true);
}

TEST_P(ClosedSourceRepairTest, repeatedTouchExecutesAndFailsWithoutPurging)
{
    checkFailedEdit(false);
}

INSTANTIATE_TEST_SUITE_P(SourceLifecycle, ClosedSourceRepairTest,
                        ::testing::Bool(),
                        [](const auto& info) { return info.param ? "ClosedSource" : "OpenSource"; });

class CrossDocumentUnrelatedMutationTest:
    public CrossDocumentExpressionRecomputeTest,
    public ::testing::WithParamInterface<bool>
{};

TEST_P(CrossDocumentUnrelatedMutationTest, settledBindingAllowsUnrelatedCollaborativeEdit)
{
    auto* source = (GetParam() ? _source : _driven)
        ->addObject<App::FeatureTestColumn>("SourceColumn");
    auto* driven = _driven->addObject<App::FeatureTestColumn>("DrivenColumn");
    auto* unrelated = _driven->addObject<App::FeatureTestColumn>("UnrelatedColumn");
    auto* group = _driven->addObject<App::DocumentObjectGroup>("Group");
    ASSERT_NE(source, nullptr);
    ASSERT_NE(driven, nullptr);
    ASSERT_NE(unrelated, nullptr);
    ASSERT_NE(group, nullptr);
    source->Column.setValue("C");
    const std::string expression =
        (GetParam() ? _sourceName + "#" : "") + "SourceColumn.Column";
    driven->setExpression(
        App::ObjectIdentifier(driven->Column),
        std::shared_ptr<App::Expression>(App::Expression::parse(driven, expression)));
    ASSERT_FALSE(group->addObject(driven).empty());
    ASSERT_FALSE(group->addObject(unrelated).empty());
    // The shared container connects the edited and driven objects, as a
    // PartDesign Body does for a pad and an unrelated sketch.
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    ASSERT_FALSE(_driven->mustExecute());
    ASSERT_EQ(driven->Value.getValue(), App::decodeColumn("C"));
    // Eager commits still settle edits when the human has disabled automatic
    // recompute. That override must not force clean, unrelated bindings.
    _driven->setStatus(App::Document::SkipRecompute, true);

    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
    mutation.objectName = unrelated->getNameInDocument();
    mutation.stableObjectIdentity = _driven->collaborationObjectIdentity(*unrelated);
    const auto result = _driven->collaborationService().commitCompatibilityMutation(
        mutation, [&] { unrelated->Column.setValue("D"); });

    EXPECT_TRUE(result.committed()) << result.message;
    EXPECT_EQ(unrelated->Column.getStrValue(), "D");
    EXPECT_EQ(unrelated->Value.getValue(), App::decodeColumn("D"));
    EXPECT_EQ(driven->Value.getValue(), App::decodeColumn("C"));
    EXPECT_EQ(driven->ExpressionEngine.getExpressions().size(), 1U);
    EXPECT_FALSE(_driven->mustExecute());
    EXPECT_TRUE(_driven->testStatus(App::Document::SkipRecompute));
}

INSTANTIATE_TEST_SUITE_P(
    Round21,
    CrossDocumentUnrelatedMutationTest,
    ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) {
        return info.param ? "DefectCrossDocument" : "HappySameDocument";
    });

// A profile edit makes the directly bound feature dirty. A separate object
// sharing only a group (Round21 above) leaves that feature clean.
TEST_P(CrossDocumentUnrelatedMutationTest, profileEditReexecutesDirectlyBoundFeature)
{
    auto* source = (GetParam() ? _source : _driven)
        ->addObject<App::FeatureTestColumn>("SourceColumn");
    auto* profile = _driven->addObject<App::FeatureTestColumn>("Profile");
    auto* driven = _driven->addObject<App::FeatureTestColumn>("DrivenColumn");
    auto* profileLink = dynamic_cast<App::PropertyLink*>(
        driven->addDynamicProperty("App::PropertyLink", "Profile"));
    ASSERT_NE(profileLink, nullptr);
    profileLink->setValue(profile);
    source->Column.setValue("C");
    const std::string expression =
        (GetParam() ? _sourceName + "#" : "") + "SourceColumn.Column";
    driven->setExpression(App::ObjectIdentifier(driven->Column),
        std::shared_ptr<App::Expression>(App::Expression::parse(driven, expression)));
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    ASSERT_FALSE(_driven->mustExecute());
    ASSERT_FALSE(driven->ExpressionEngine.isTouched());

    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
    mutation.objectName = profile->getNameInDocument();
    mutation.stableObjectIdentity = _driven->collaborationObjectIdentity(*profile);
    const auto result = _driven->collaborationService().commitCompatibilityMutation(
        mutation, [&] { profile->Column.setValue("D"); });
    EXPECT_TRUE(result.committed()) << result.message;
    EXPECT_EQ(profile->Value.getValue(), App::decodeColumn("D"));
    EXPECT_EQ(driven->Value.getValue(), App::decodeColumn("C"));
    EXPECT_EQ(driven->ExpressionEngine.getExpressions().size(), 1U);
    EXPECT_FALSE(_driven->mustExecute());

    source->Column.setValue("E");
    if (GetParam()) {
        // Dirty external inputs still need an explicit source recompute.
        const auto unsettled = _driven->collaborationService().commitCompatibilityMutation(
            mutation, [&] { profile->Column.setValue("F"); });
        EXPECT_EQ(unsettled.status, App::DocumentCommitStatus::Busy) << unsettled.message;
        EXPECT_NE(unsettled.message.find("document has pending recompute work outside the prepared operation"),
                  std::string::npos) << unsettled.message;
        EXPECT_EQ(profile->Column.getStrValue(), "D");
    }
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    EXPECT_EQ(driven->Value.getValue(), App::decodeColumn("E"));
    EXPECT_FALSE(_driven->mustExecute());
}


// Exercise both sides of the detached preparation boundary without timing
// sleeps: a clean source at capture can change before the operation applies.
class DerivedExternalExpressionTest: public CrossDocumentExpressionRecomputeTest
{
protected:
    void SetUp() override
    {
        CrossDocumentExpressionRecomputeTest::SetUp();
        source = _source->addObject<App::FeatureTestColumn>("SourceColumn");
        profile = _driven->addObject<App::FeatureTestColumn>("Profile");
        driven = _driven->addObject<App::FeatureTestColumn>("DrivenColumn");
        auto* link = dynamic_cast<App::PropertyLink*>(
            driven->addDynamicProperty("App::PropertyLink", "Profile"));
        ASSERT_NE(link, nullptr);
        link->setValue(profile);
        source->Column.setValue("C");
        driven->setExpression(App::ObjectIdentifier(driven->Column),
            std::shared_ptr<App::Expression>(App::Expression::parse(
                driven, _sourceName + "#SourceColumn.Column")));
        static_cast<void>(_source->recompute());
        static_cast<void>(_driven->recompute());
        ASSERT_FALSE(_source->mustExecute());
        ASSERT_FALSE(_driven->mustExecute());
        ASSERT_TRUE(_source->save());
    }

    App::CollaborativeOperationPreparation prepareOnCurrentThread()
    {
        App::Internal::ensureGenericIsolatedRecomputeRegistered();
        return App::CollaborativeOperationRegistry::instance().prepare(*_driven,
            {std::string(App::GenericIsolatedRecomputeOperationType),
             {{"feature", "DrivenColumn"}, {"force_execution", "1"},
              {"owner_thread_execution", "1"}, {"derived_coordinator_recompute", "1"}}});
    }

    App::CollaborativeOperationPreparation prepare()
    {
        std::optional<App::CollaborativeOperationPreparation> preparation;
        static_cast<void>(_driven->collaborationService().runOnOwnerThread([&] {
            preparation.emplace(prepareOnCurrentThread());
            return true;
        }));
        return std::move(*preparation);
    }

    App::DocumentCommitResult apply(const App::CollaborativeOperation& operation)
    {
        App::CollaborationCompatibilityMutation mutation;
        mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
        mutation.objectName = profile->getNameInDocument();
        mutation.stableObjectIdentity = _driven->collaborationObjectIdentity(*profile);
        return _driven->collaborationService().commitCompatibilityMutationWithPolicy(
            mutation, [&] { operation.apply(*_driven); },
            App::CollaborationCompatibilityRecomputePolicy::Deferred);
    }

    void expectRefusal(const App::DocumentCommitResult& result)
    {
        EXPECT_FALSE(result.committed()) << result.message;
        EXPECT_NE(result.message.find("unresolved cross-document dependency"),
                  std::string::npos) << result.message;
        EXPECT_EQ(driven->Value.getValue(), App::decodeColumn("C"));
    }

    void expectPreparationRefusal()
    {
        try {
            static_cast<void>(prepare());
            FAIL() << "accepted an unsafe external source";
        }
        catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find("unresolved cross-document dependency"),
                      std::string::npos) << error.what();
        }
    }

    App::FeatureTestColumn* source {};
    App::FeatureTestColumn* profile {};
    App::FeatureTestColumn* driven {};
};

TEST_F(DerivedExternalExpressionTest, successfulEvaluationReleasesSourceRestriction)
{
    auto preparation = prepare();
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    const auto result = apply(*operation);
    ASSERT_TRUE(result.committed()) << result.message;
    // A native structural mutation after evaluation checks that the capture
    // depth returned to zero; retaining the operation must not retain pins.
    ASSERT_NO_THROW(_source->addObject("App::FeatureTest", "AfterEvaluation"));
    EXPECT_NO_THROW(_source->removeObject("AfterEvaluation"));
    EXPECT_EQ(_source->getObject("AfterEvaluation"), nullptr);
}

TEST_F(DerivedExternalExpressionTest, cleanSourceWithDirtyAncestorIsRefused)
{
    auto* ancestor = _source->addObject<App::FeatureTestColumn>("Ancestor");
    ancestor->Column.setValue("C");
    source->setExpression(App::ObjectIdentifier(source->Column),
        std::shared_ptr<App::Expression>(App::Expression::parse(source, "Ancestor.Column")));
    static_cast<void>(_source->recompute());
    static_cast<void>(_driven->recompute());
    ancestor->Column.setValue("E");
    ASSERT_FALSE(source->isTouched());
    ASSERT_EQ(source->mustRecompute(), 0);
    ASSERT_TRUE(ancestor->isTouched());
    expectPreparationRefusal();
    // The compatibility preflight must also refuse this unsettled dependent.
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::ObjectModel;
    mutation.objectName = "Profile";
    mutation.stableObjectIdentity = _driven->collaborationObjectIdentity(*profile);
    const auto result = _driven->collaborationService().commitCompatibilityMutation(
        mutation, [&] { profile->Column.setValue("D"); });
    EXPECT_EQ(result.status, App::DocumentCommitStatus::Busy) << result.message;
    EXPECT_NE(result.message.find("document has pending recompute work outside the prepared operation"),
              std::string::npos) << result.message;
    EXPECT_EQ(profile->Column.getStrValue(), "A");
}

TEST_F(DerivedExternalExpressionTest, pendingForcedSourceRecomputeIsRefusedAtPreparation)
{
    auto pending = _source->recomputeAsync({source}, true, 0, App::RecomputeVenue::Isolated);
    ASSERT_TRUE(_source->recomputeCoordinator().hasPendingWork());
    ASSERT_FALSE(source->isTouched());
    ASSERT_EQ(source->mustRecompute(), 0);
    expectPreparationRefusal();
    static_cast<void>(pending->cancel());
}

TEST_F(DerivedExternalExpressionTest, pendingForcedSourceRecomputeIsRefusedAtEvaluation)
{
    auto preparation = prepare();
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    auto pending = _source->recomputeAsync({source}, true, 0, App::RecomputeVenue::Isolated);
    ASSERT_TRUE(_source->recomputeCoordinator().hasPendingWork());
    ASSERT_FALSE(source->isTouched());
    expectRefusal(apply(*operation));
    static_cast<void>(pending->cancel());
}

TEST_F(DerivedExternalExpressionTest, settledSourceMutationAfterPreparationIsRefused)
{
    auto preparation = prepare();
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    source->Column.setValue("E");
    static_cast<void>(_source->recompute());
    ASSERT_FALSE(source->isTouched());
    ASSERT_FALSE(_source->mustExecute());
    expectRefusal(apply(*operation));
}

TEST_F(DerivedExternalExpressionTest, ownerFallbackStillRefusesChangedExternalSource)
{
    auto* unopted = _driven->addObject("App::FeaturePython", "OwnerOnlyInput");
    ASSERT_FALSE(unopted->canRecomputeOnWorker());
    auto* link = dynamic_cast<App::PropertyLink*>(
        driven->addDynamicProperty("App::PropertyLink", "OwnerOnlyInput"));
    ASSERT_NE(link, nullptr);
    link->setValue(unopted);
    static_cast<void>(_driven->recompute());
    auto preparation = prepare();
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    source->Column.setValue("E");
    static_cast<void>(_source->recompute());
    expectRefusal(apply(*operation));
}

TEST_F(DerivedExternalExpressionTest, sourceCloseAfterPreparationIsRefused)
{
    auto preparation = prepare();
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    ASSERT_TRUE(App::GetApplication().closeDocument(_sourceName.c_str()));
    _source = nullptr;
    _sourceObject = nullptr;
    source = nullptr;
    expectRefusal(apply(*operation));
}

TEST_F(DerivedExternalExpressionTest, sourceReloadAfterPreparationIsRefused)
{
    auto preparation = prepare();
    ASSERT_TRUE(preparation.detachedTask);
    auto operation = preparation.detachedTask(std::stop_token {});
    _source->restore((_directory / "qa_hole.FCStd").string().c_str());
    static_cast<void>(_source->afterRestore());
    static_cast<void>(_source->recompute());
    ASSERT_FALSE(_source->mustExecute());
    expectRefusal(apply(*operation));
}

TEST_F(DerivedExternalExpressionTest, foreignThreadSourceReadIsRefused)
{
    // An unsupported caller must be refused before reading source objects,
    // even when both documents and all their properties are currently clean.
    std::thread foreign([&] {
        try {
            static_cast<void>(prepareOnCurrentThread());
            FAIL() << "accepted a foreign-thread source read";
        }
        catch (const std::invalid_argument& error) {
            EXPECT_NE(std::string(error.what()).find("unresolved cross-document dependency"),
                      std::string::npos) << error.what();
        }
    });
    foreign.join();
}

// clang-format on
