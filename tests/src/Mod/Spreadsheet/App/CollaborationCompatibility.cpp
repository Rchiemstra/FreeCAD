// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentCommitCoordinator.h>
#include <App/DocumentObserverPython.h>
#include <App/DocumentRevisionIndex.h>
#include <App/Expression.h>
#include <App/FeatureTest.h>
#include <App/ObjectIdentifier.h>
#include <App/PropertyStandard.h>
#include <Base/Interpreter.h>
#include <Base/Tools.h>
#include <Mod/Spreadsheet/App/Sheet.h>
#include <Mod/Spreadsheet/App/Cell.h>
#include <src/App/InitApplication.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{

class PyObjectRef final
{
public:
    explicit PyObjectRef(PyObject* object = nullptr)
        : _object(object)
    {}

    ~PyObjectRef()
    {
        Py_XDECREF(_object);
    }

    PyObjectRef(const PyObjectRef&) = delete;
    PyObjectRef& operator=(const PyObjectRef&) = delete;

    [[nodiscard]] PyObject* get() const noexcept
    {
        return _object;
    }

private:
    PyObject* _object;
};

class PythonDocumentObserverGuard final
{
public:
    explicit PythonDocumentObserverGuard(PyObject* observer)
        : _observer(observer)
    {
        App::DocumentObserverPython::addObserver(_observer);
    }

    ~PythonDocumentObserverGuard()
    {
        App::DocumentObserverPython::removeObserver(_observer);
    }

    PythonDocumentObserverGuard(const PythonDocumentObserverGuard&) = delete;
    PythonDocumentObserverGuard& operator=(const PythonDocumentObserverGuard&) = delete;

private:
    Py::Object _observer;
};

class ScopedTemporaryDirectory final
{
public:
    explicit ScopedTemporaryDirectory(const std::string& prefix)
        : path(std::filesystem::temp_directory_path()
               / (prefix
                  + std::to_string(
                      std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(path);
    }

    ~ScopedTemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

class SpreadsheetCollaborationCompatibilityTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _documentName =
            App::GetApplication().getUniqueDocumentName("spreadsheetCollaboration");
        _document = App::GetApplication().newDocument(
            _documentName.c_str(), "Spreadsheet collaboration test");
        _sheet = freecad_cast<Spreadsheet::Sheet*>(
            _document->addObject("Spreadsheet::Sheet", "Sheet"));
        ASSERT_NE(_sheet, nullptr);
        _failure = freecad_cast<App::FeatureTest*>(
            _document->addObject("App::FeatureTest", "Failure"));
        ASSERT_NE(_failure, nullptr);
        _failure->Source1.setValue(_sheet);
        _document->recompute();

        const auto identity = _document->collaborationIdentity();
        _cursor.documentInstanceId = identity.instanceId;
        _cursor.lifecycleEpoch = identity.lifecycleEpoch;
        _cursor.afterSequence = _document->collaborationRevisions()
                                    .pollPublications(_cursor, 0)
                                    .latestSequence;
    }

    void TearDown() override
    {
        if (_document) {
            App::GetApplication().closeDocument(_documentName.c_str());
        }
    }

    App::DocumentCommitResult commitCell(const char* address, const char* value)
    {
        App::CollaborationCompatibilityMutation mutation;
        mutation.scope = App::CollaborationCompatibilityScope::UnknownModel;
        return _document->collaborationService().commitCompatibilityMutation(
            std::move(mutation), [this, address, value] { _sheet->setCell(address, value); });
    }

    void expectSingleSpreadsheetPublication()
    {
        const auto poll = _document->collaborationRevisions().pollPublications(_cursor);
        ASSERT_EQ(poll.events.size(), 1U);
        ASSERT_EQ(poll.events.front().changes.size(), 2U);

        const auto& changes = poll.events.front().changes;
        const auto find = [&](const App::DocumentRevisionKey& key) {
            return std::ranges::find_if(changes, [&](const auto& change) {
                return change.key == key;
            });
        };
        const auto wildcard = find(App::DocumentRevisionKey::unknownModelMutation());
        const auto structure =
            find(App::DocumentRevisionKey::objectStructure(_sheet->getNameInDocument()));
        ASSERT_NE(wildcard, changes.end());
        ASSERT_NE(structure, changes.end());
        EXPECT_FALSE(wildcard->stableObjectIdentity.has_value());
        EXPECT_EQ(structure->stableObjectIdentity,
                  _document->collaborationObjectIdentity(*_sheet));
        _cursor = poll.nextCursor;
    }

    std::string _documentName;
    App::Document* _document {nullptr};
    Spreadsheet::Sheet* _sheet {nullptr};
    App::FeatureTest* _failure {nullptr};
    App::DocumentRevisionCursor _cursor;
};

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       authoritativeRecomputePublishesTransientCellSchema)
{
    const auto numericResult = commitCell("A1", "=1+2");
    ASSERT_EQ(numericResult.status, App::DocumentCommitStatus::Committed)
        << numericResult.message;
    auto* numeric = _sheet->getPropertyByName("A1");
    ASSERT_NE(numeric, nullptr);
    ASSERT_TRUE(numeric->is<App::PropertyInteger>());
    EXPECT_EQ(static_cast<App::PropertyInteger*>(numeric)->getValue(), 3);
    expectSingleSpreadsheetPublication();

    int removed = 0;
    int appended = 0;
    fastsignals::scoped_connection removeConnection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
        [&](const App::Property& property) {
            if (property.getContainer() == _sheet
                && property.getName() && std::strcmp(property.getName(), "A1") == 0) {
                ++removed;
                EXPECT_FALSE(_document->hasPendingTransaction());
            }
        });
    fastsignals::scoped_connection appendConnection =
        App::GetApplication().signalAppendDynamicProperty.connect(
        [&](const App::Property& property) {
            if (property.getContainer() == _sheet
                && property.getName() && std::strcmp(property.getName(), "A1") == 0) {
                ++appended;
                EXPECT_FALSE(_document->hasPendingTransaction());
            }
        });

    Base::PyGILStateLocker gil;
    PyObject* mainModule = PyImport_AddModule("__main__");
    ASSERT_NE(mainModule, nullptr);
    PyObject* globals = PyModule_GetDict(mainModule);
    PyObjectRef pythonObserver(PyRun_String(
        "type('SpreadsheetObserver', (), {"
        "'removed': [], "
        "'slotRemoveDynamicProperty': "
        "lambda self, obj, name: self.removed.append(name)})()",
        Py_eval_input,
        globals,
        globals));
    ASSERT_NE(pythonObserver.get(), nullptr);
    PythonDocumentObserverGuard observerGuard(pythonObserver.get());

    const auto stringResult = commitCell("A1", "text");
    ASSERT_EQ(stringResult.status, App::DocumentCommitStatus::Committed)
        << stringResult.message;
    auto* text = _sheet->getPropertyByName("A1");
    ASSERT_NE(text, nullptr);
    ASSERT_TRUE(text->is<App::PropertyString>());
    EXPECT_STREQ(static_cast<App::PropertyString*>(text)->getValue(), "text");
    EXPECT_EQ(removed, 1);
    EXPECT_EQ(appended, 1);
    PyObjectRef pythonRemoved(
        PyObject_GetAttrString(pythonObserver.get(), "removed"));
    ASSERT_NE(pythonRemoved.get(), nullptr);
    ASSERT_EQ(PyList_Size(pythonRemoved.get()), 1);
    EXPECT_STREQ(PyUnicode_AsUTF8(PyList_GetItem(pythonRemoved.get(), 0)), "A1");
    expectSingleSpreadsheetPublication();
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       createThenSetCellsReachCleanStableBoundaries)
{
    Spreadsheet::Sheet* created = nullptr;
    App::FeatureTest* dependent = nullptr;
    App::CollaborationCompatibilityMutation createMutation;
    createMutation.scope = App::CollaborationCompatibilityScope::Structural;
    const auto createResult =
        _document->collaborationService().commitCompatibilityMutation(
            std::move(createMutation), [&] {
                created = freecad_cast<Spreadsheet::Sheet*>(
                    _document->addObject("Spreadsheet::Sheet", "CreatedSheet"));
                dependent = _document->addObject<App::FeatureTest>("SheetDependent");
            });
    ASSERT_EQ(createResult.status, App::DocumentCommitStatus::Committed)
        << createResult.message;
    ASSERT_NE(created, nullptr);
    ASSERT_NE(dependent, nullptr);
    EXPECT_FALSE(_document->mustExecute());

    App::CollaborationCompatibilityMutation cellsMutation;
    cellsMutation.scope = App::CollaborationCompatibilityScope::UnknownModel;
    const auto cellsResult =
        _document->collaborationService().commitCompatibilityMutation(
            std::move(cellsMutation), [created, dependent] {
                created->setCell("A1", "=1+2");
                created->setCell("B1", "120 mm");
                created->setAlias(App::CellAddress("B1"), "span");
                dependent->setExpression(
                    App::ObjectIdentifier(dependent->QuantityLength),
                    std::shared_ptr<App::Expression>(
                        App::Expression::parse(dependent, "CreatedSheet.span")));
            });
    ASSERT_EQ(cellsResult.status, App::DocumentCommitStatus::Committed)
        << cellsResult.message;
    auto* numeric = created->getPropertyByName("A1");
    ASSERT_NE(numeric, nullptr);
    ASSERT_TRUE(numeric->is<App::PropertyInteger>());
    EXPECT_EQ(static_cast<App::PropertyInteger*>(numeric)->getValue(), 3);
    EXPECT_NE(created->getPropertyByName("B1"), nullptr);
    EXPECT_NE(created->getPropertyByName("span"), nullptr);
    EXPECT_DOUBLE_EQ(dependent->QuantityLength.getValue(), 120.0);
    EXPECT_FALSE(_document->mustExecute());
    EXPECT_TRUE(created->isValid());
    EXPECT_TRUE(dependent->isValid());
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       recomputeFailureDiscardsTransientSchemaNotifications)
{
    const auto numericResult = commitCell("A1", "=1+2");
    ASSERT_EQ(numericResult.status, App::DocumentCommitStatus::Committed)
        << numericResult.message;
    expectSingleSpreadsheetPublication();

    int removed = 0;
    int appended = 0;
    fastsignals::scoped_connection removeConnection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
        [&](const App::Property& property) {
            if (property.getContainer() == _sheet) {
                ++removed;
            }
        });
    fastsignals::scoped_connection appendConnection =
        App::GetApplication().signalAppendDynamicProperty.connect(
        [&](const App::Property& property) {
            if (property.getContainer() == _sheet) {
                ++appended;
            }
        });

    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::UnknownModel;
    const auto result = _document->collaborationService().commitCompatibilityMutation(
        std::move(mutation), [this] {
            _sheet->setCell("A1", "text");
            _failure->ExceptionType.setValue(1);
        });

    EXPECT_EQ(result.status, App::DocumentCommitStatus::RecomputeFailed)
        << result.message;
    EXPECT_EQ(removed, 0);
    EXPECT_EQ(appended, 0);
    EXPECT_EQ(_failure->ExceptionType.getValue(), 0);
    auto* restored = _sheet->getPropertyByName("A1");
    ASSERT_NE(restored, nullptr);
    ASSERT_TRUE(restored->is<App::PropertyInteger>());
    EXPECT_EQ(static_cast<App::PropertyInteger*>(restored)->getValue(), 3);
    const auto poll = _document->collaborationRevisions().pollPublications(_cursor);
    EXPECT_TRUE(poll.events.empty());
    EXPECT_EQ(poll.latestSequence, _cursor.afterSequence);
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       callbackFailureRestoresCellWithoutPublication)
{
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::UnknownModel;
    const auto result = _document->collaborationService().commitCompatibilityMutation(
        std::move(mutation), [this] {
            _sheet->setCell("A2", "=40+2");
            throw std::runtime_error("spreadsheet callback failure");
        });

    EXPECT_EQ(result.status, App::DocumentCommitStatus::ApplyFailed)
        << result.message << "; sheet error: "
        << (_document->getErrorDescription(_sheet)
                ? _document->getErrorDescription(_sheet)
                : "<none>")
        << "; A2 cell error: "
        << (_sheet->getCell(App::CellAddress("A2"))
                ? _sheet->getCell(App::CellAddress("A2"))->getException()
                : "<no cell>");
    EXPECT_EQ(_sheet->getCell(App::CellAddress("A2")), nullptr);
    EXPECT_EQ(_sheet->getPropertyByName("A2"), nullptr);
    const auto poll = _document->collaborationRevisions().pollPublications(_cursor);
    EXPECT_TRUE(poll.events.empty());
    EXPECT_EQ(poll.latestSequence, _cursor.afterSequence);
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       crossDocumentDeferredTouchCanCloseItsDependentDuringCommitPublication)
{
    ScopedTemporaryDirectory temporary("fc_spreadsheet_collaboration_close_");
    const auto sourcePath = (temporary.path / "source.FCStd").string();
    _sheet->setCell("A1", "10 mm");
    _sheet->setAlias(App::CellAddress("A1"), "Width");
    ASSERT_TRUE(_document->recompute());
    ASSERT_TRUE(_document->saveAs(sourcePath.c_str()));

    const auto dependentName = App::GetApplication().getUniqueDocumentName(
        "spreadsheetCollaborationDependent");
    auto* dependentDocument = App::GetApplication().newDocument(
        dependentName.c_str(), "Spreadsheet collaboration dependent");
    ASSERT_NE(dependentDocument, nullptr);
    Base::ScopeGuard closeDependent([&] {
        if (App::GetApplication().getDocument(dependentName.c_str())) {
            App::GetApplication().closeDocument(dependentName.c_str());
        }
    });
    auto* dependent = dependentDocument->addObject<App::FeatureTest>("Dependent");
    ASSERT_NE(dependent, nullptr);
    const auto dependentPath = (temporary.path / "dependent.FCStd").string();
    ASSERT_TRUE(dependentDocument->saveAs(dependentPath.c_str()));
    dependent->setExpression(
        App::ObjectIdentifier(dependent->QuantityLength),
        std::shared_ptr<App::Expression>(App::Expression::parse(
            dependent, _documentName + "#Sheet.Width")));
    ASSERT_TRUE(dependentDocument->recompute());
    ASSERT_DOUBLE_EQ(dependent->QuantityLength.getValue(), 10.0);

    bool callbackRan = false;
    fastsignals::scoped_connection touchedConnection;
    touchedConnection = dependentDocument->signalTouchedObject.connect(
        [&](const App::DocumentObject& object) {
            if (&object != dependent) {
                return;
            }
            callbackRan = true;
            touchedConnection.disconnect();
            EXPECT_TRUE(App::GetApplication().closeDocument(dependentName.c_str()));
            dependentDocument = nullptr;
            dependent = nullptr;
        });

    const auto result = commitCell("A1", "20 mm");
    EXPECT_EQ(result.status, App::DocumentCommitStatus::Committed) << result.message;
    EXPECT_TRUE(callbackRan);
    EXPECT_EQ(App::GetApplication().getDocument(dependentName.c_str()), nullptr);
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       failedCommitDiscardsDeferredCrossDocumentTouches)
{
    ScopedTemporaryDirectory temporary("fc_spreadsheet_collaboration_rollback_");
    const auto sourcePath = (temporary.path / "source.FCStd").string();
    _sheet->setCell("A1", "10 mm");
    _sheet->setAlias(App::CellAddress("A1"), "Width");
    ASSERT_TRUE(_document->recompute());
    ASSERT_TRUE(_document->saveAs(sourcePath.c_str()));

    const auto dependentName = App::GetApplication().getUniqueDocumentName(
        "spreadsheetCollaborationDependent");
    auto* dependentDocument = App::GetApplication().newDocument(
        dependentName.c_str(), "Spreadsheet collaboration dependent");
    ASSERT_NE(dependentDocument, nullptr);
    Base::ScopeGuard closeDependent([&] {
        if (App::GetApplication().getDocument(dependentName.c_str())) {
            App::GetApplication().closeDocument(dependentName.c_str());
        }
    });
    auto* dependent = dependentDocument->addObject<App::FeatureTest>("Dependent");
    ASSERT_NE(dependent, nullptr);
    const auto dependentPath = (temporary.path / "dependent.FCStd").string();
    ASSERT_TRUE(dependentDocument->saveAs(dependentPath.c_str()));
    dependent->setExpression(
        App::ObjectIdentifier(dependent->QuantityLength),
        std::shared_ptr<App::Expression>(App::Expression::parse(
            dependent, _documentName + "#Sheet.Width")));
    ASSERT_TRUE(dependentDocument->recompute());
    ASSERT_DOUBLE_EQ(dependent->QuantityLength.getValue(), 10.0);

    int touched = 0;
    fastsignals::scoped_connection touchedConnection =
        dependentDocument->signalTouchedObject.connect(
            [&](const App::DocumentObject& object) {
                if (&object == dependent) {
                    ++touched;
                }
            });
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::UnknownModel;
    const auto result = _document->collaborationService().commitCompatibilityMutation(
        std::move(mutation), [this] {
            _sheet->setCell("A1", "20 mm");
            _failure->ExceptionType.setValue(1);
        });

    EXPECT_EQ(result.status, App::DocumentCommitStatus::RecomputeFailed)
        << result.message;
    EXPECT_EQ(touched, 0);
    EXPECT_FALSE(dependent->isTouched());
    EXPECT_DOUBLE_EQ(dependent->QuantityLength.getValue(), 10.0);
    ASSERT_TRUE(App::GetApplication().closeDocument(dependentName.c_str()));
}

}  // namespace
