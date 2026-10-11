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
#include <App/MutationClassification.h>
#include <Base/Exception.h>
#include <App/PropertyStandard.h>
#include <Base/Interpreter.h>
#include <Base/Tools.h>
#include <Mod/Spreadsheet/App/Sheet.h>
#include <Mod/Spreadsheet/App/Cell.h>
#include <Mod/Spreadsheet/App/PropertySheet.h>
#include <src/App/InitApplication.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace App::Internal
{
// Use the existing private Document friend, solely in this test executable.
// No public API, asynchronous thread, or production granting helper is added.
class DocumentStructuralCompatibilityTestAccess
{
public:
    static void withStableRead(Document& document, const std::function<void()>& action)
    {
        document.beginCollaborationStableReadCapture();
        Base::ScopeGuard finish([&] { document.finishCollaborationStableReadCapture(); });
        action();
    }
};
}  // namespace App::Internal

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
        std::ostringstream revisionKeys;
        for (const auto& change : poll.events.front().changes) {
            revisionKeys << "kind=" << static_cast<int>(change.key.kind)
                         << ",subject=" << change.key.subject
                         << ",propertyName=" << change.key.propertyName
                         << ",stableIDpresent=" << change.stableObjectIdentity.has_value() << "; ";
        }
        ASSERT_EQ(poll.events.front().changes.size(), 2U) << revisionKeys.str();

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


    // A supported public FeaturePython producer executes on a different object
    // from its target sheet, just as ArchReport does. No private grant is used.
    App::DocumentObject* makeCellClearProducer(bool captureDerived = false)
    {
        auto* producer = _document->addObject("App::FeaturePython", "CellClearProducer");
        auto* target = static_cast<App::PropertyLink*>(
            producer->addDynamicProperty("App::PropertyLink", "Target"));
        target->setValue(_sheet);
        Base::PyGILStateLocker gil;
        PyObjectRef globals(PyDict_New());
        PyObjectRef object(producer->getPyObject());
        PyObjectRef moduleName(PyUnicode_FromString("spreadsheet_cell_clear_tests"));
        if (!globals.get() || !object.get() || !moduleName.get()
            || PyDict_SetItemString(globals.get(), "__name__", moduleName.get()) != 0
            || PyDict_SetItemString(globals.get(), "__builtins__", PyEval_GetBuiltins()) != 0
            || PyDict_SetItemString(globals.get(), "producer", object.get()) != 0) {
            throw std::runtime_error("could not create cell-clear producer globals");
        }
        PyObjectRef installed(PyRun_String(
            "class CellClearProxy:\n"
            "    armed = False\n"
            "    errors = ()\n"
            "    clear_address = 'A1'\n"
            "    capture_derived = False\n"
            "    derived_snapshot = ()\n"
            "    derived_input = '=6+7'\n"
            "    def execute(self, obj):\n"
            "        if not self.armed:\n"
            "            return\n"
            "        self.armed = False\n"
            "        try:\n"
            "            obj.Target.clear(self.clear_address)\n"
            "        except Exception as exc:\n"
            "            self.errors = (str(exc), obj.Target.getContents('A1'),\n"
            "                           'A1' in obj.Target.PropertiesList,\n"
            "                           obj.Target.getAlias('A1'),\n"
            "                           getattr(obj.Target, 'A1', None),\n"
            "                           'B1' in obj.Target.PropertiesList,\n"
            "                           getattr(obj.Target, 'B1', None))\n"
            "            raise\n"
            "        obj.Target.set('A1', '13')\n"
            "        if self.capture_derived:\n"
            "            obj.Target.set('A1', 'Amount')\n"
            "            obj.Target.set('A2', self.derived_input)\n"
            "        obj.Target.recompute()\n"
            "        if self.capture_derived:\n"
            "            self.derived_snapshot = (getattr(obj.Target, 'A1', None),\n"
            "                getattr(obj.Target, 'A2', None),\n"
            "                'A3' in obj.Target.PropertiesList, obj.Target.getContents('A3'))\n"
            "            obj.Target.purgeTouched()\n"
            "producer.Proxy = CellClearProxy()\n",
            Py_file_input, globals.get(), globals.get()));
        if (!installed.get()) {
            PyErr_Print();
            throw std::runtime_error("could not install cell-clear producer proxy");
        }
        if (captureDerived) {
            PyObjectRef proxy(PyObject_GetAttrString(object.get(), "Proxy"));
            if (!proxy.get()
                || PyObject_SetAttrString(proxy.get(), "capture_derived", Py_True) != 0) {
                throw std::runtime_error("could not enable derived cache capture");
            }
        }
        _document->recompute();
        return producer;
    }

    void setProducerClearAddress(App::DocumentObject* producer, const char* address)
    {
        Base::PyGILStateLocker gil;
        PyObjectRef object(producer->getPyObject());
        PyObjectRef proxy(PyObject_GetAttrString(object.get(), "Proxy"));
        PyObjectRef value(PyUnicode_FromString(address));
        ASSERT_NE(proxy.get(), nullptr);
        ASSERT_NE(value.get(), nullptr);
        ASSERT_EQ(PyObject_SetAttrString(proxy.get(), "clear_address", value.get()), 0);
    }

    // The proxy records these values in its first exception handler, before
    // FeaturePython/coordinator error handling, rollback, or deferred observers.
    void expectProducerErrorSnapshot(App::DocumentObject* producer, const char* content,
                                     bool hasA1, const char* alias, long cache,
                                     bool renamed = false)
    {
        Base::PyGILStateLocker gil;
        PyObjectRef object(producer->getPyObject());
        PyObjectRef proxy(PyObject_GetAttrString(object.get(), "Proxy"));
        ASSERT_NE(proxy.get(), nullptr);
        PyObjectRef errors(PyObject_GetAttrString(proxy.get(), "errors"));
        ASSERT_NE(errors.get(), nullptr);
        ASSERT_TRUE(PyTuple_Check(errors.get()));
        ASSERT_EQ(PyTuple_Size(errors.get()), 7);
        ASSERT_TRUE(PyUnicode_Check(PyTuple_GetItem(errors.get(), 0)));
        EXPECT_GT(PyUnicode_GetLength(PyTuple_GetItem(errors.get(), 0)), 0);
        EXPECT_STREQ(PyUnicode_AsUTF8(PyTuple_GetItem(errors.get(), 1)), content);
        EXPECT_EQ(PyObject_IsTrue(PyTuple_GetItem(errors.get(), 2)), hasA1 ? 1 : 0);
        if (alias) {
            EXPECT_STREQ(PyUnicode_AsUTF8(PyTuple_GetItem(errors.get(), 3)), alias);
        }
        if (hasA1) {
            EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(errors.get(), 4)), cache);
        }
        if (renamed) {
            EXPECT_EQ(PyObject_IsTrue(PyTuple_GetItem(errors.get(), 5)), 1);
            EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(errors.get(), 6)), cache);
        }
        EXPECT_FALSE(PyErr_Occurred());
    }

    void armCellClearProducer(App::DocumentObject* producer)
    {
        Base::PyGILStateLocker gil;
        PyObjectRef object(producer->getPyObject());
        PyObjectRef proxy(PyObject_GetAttrString(object.get(), "Proxy"));
        ASSERT_NE(proxy.get(), nullptr);
        ASSERT_EQ(PyObject_SetAttrString(proxy.get(), "armed", Py_True), 0);
    }

    App::DocumentCommitResult commitProducer(App::DocumentObject* producer,
                                            bool fail = false)
    {
        armCellClearProducer(producer);
        App::CollaborationCompatibilityMutation mutation;
        mutation.scope = App::CollaborationCompatibilityScope::UnknownModel;
        return _document->collaborationService().commitCompatibilityMutation(
            std::move(mutation), [&, fail] {
                producer->touch();
                if (fail) {
                    _failure->ExceptionType.setValue(1);
                }
            });
    }

    void useLiteralDerivedInput(App::DocumentObject* producer)
    {
        Base::PyGILStateLocker gil;
        PyObjectRef object(producer->getPyObject());
        PyObjectRef proxy(PyObject_GetAttrString(object.get(), "Proxy"));
        PyObjectRef value(PyUnicode_FromString("13"));
        ASSERT_NE(proxy.get(), nullptr);
        ASSERT_NE(value.get(), nullptr);
        ASSERT_EQ(PyObject_SetAttrString(proxy.get(), "derived_input", value.get()), 0);
    }

    void expectUnadoptedDerivedReuseDenied(short flags, long initialValue)
    {
        // Settle ALL ordinary executes before inserting the custom A2. Only
        // A1 is seeded/cleared; clearing A2 would test the old removal gate.
        _sheet->setCell("A1", "7");
        ASSERT_TRUE(_document->recompute());
        auto* producer = makeCellClearProducer(true);
        useLiteralDerivedInput(producer);
        ASSERT_EQ(_sheet->getPropertyByName("A2"), nullptr);
        ASSERT_EQ(_sheet->getCell(App::CellAddress("A2")), nullptr);
        auto* raw = _sheet->addDynamicProperty(
            "App::PropertyInteger", "A2", nullptr, nullptr, flags);
        ASSERT_NE(raw, nullptr);
        static_cast<App::PropertyInteger*>(raw)->setValue(initialValue);
        const auto propertyId = raw->getID();
        const auto propertyType = raw->getTypeId();
        App::CellAddress mapped;
        ASSERT_FALSE(_sheet->getCellAddress(raw, mapped));
        _cursor = _document->collaborationRevisions().pollPublications(_cursor).nextCursor;

        int snapshots = 0;
        long observedValue = -1;
        bool observedOwnership = true;
        // cellUpdated is the existing synchronous native signal. On success
        // it follows the setter; on rejection execute's error path emits it.
        // Capture the FIRST A2 observation while the producer is still inside
        // target.recompute(), before producer return, rollback, or observers.
        fastsignals::scoped_connection connection = _sheet->cellUpdated.connect(
            [&](App::CellAddress address) {
                if (address != App::CellAddress("A2") || snapshots != 0
                    || !producer->testStatus(App::ObjectStatus::Recompute)) {
                    return;
                }
                ++snapshots;
                EXPECT_TRUE(_document->hasPendingTransaction());
                EXPECT_TRUE(_document->testStatus(App::Document::Recomputing));
                EXPECT_FALSE(_sheet->testStatus(App::ObjectStatus::Recompute));
                auto* current = _sheet->App::PropertyContainer::getDynamicPropertyByName("A2");
                EXPECT_EQ(current, raw);
                // Resolve the raw canonical pointer before any lifetime/type
                // dereference, including a callback after a failed operation.
                if (current != raw) {
                    return;
                }
                EXPECT_EQ(current->getID(), propertyId);
                EXPECT_EQ(current->getTypeId(), propertyType);
                EXPECT_EQ(current->getContainer(), _sheet);
                EXPECT_STREQ(current->getName(), "A2");
                if (current->getTypeId() != App::PropertyInteger::getClassTypeId()) {
                    return;
                }
                observedValue = static_cast<App::PropertyInteger*>(current)->getValue();
                observedOwnership = _sheet->getCellAddress(current, mapped);
                EXPECT_EQ(observedValue, initialValue);
                EXPECT_FALSE(observedOwnership);
            });
        const auto result = commitProducer(producer);
        EXPECT_EQ(result.status, App::DocumentCommitStatus::RecomputeFailed) << result.message;
        EXPECT_EQ(snapshots, 1);
        EXPECT_EQ(observedValue, initialValue);
        EXPECT_FALSE(observedOwnership);
        EXPECT_TRUE(_document->collaborationRevisions().pollPublications(_cursor).events.empty());
        auto* retained = _sheet->App::PropertyContainer::getDynamicPropertyByName("A2");
        ASSERT_EQ(retained, raw);
        ASSERT_NE(retained, nullptr);
        EXPECT_EQ(retained->getID(), propertyId);
        ASSERT_EQ(retained->getTypeId(), propertyType);
        EXPECT_EQ(retained->getContainer(), _sheet);
        EXPECT_STREQ(retained->getName(), "A2");
        EXPECT_EQ(static_cast<App::PropertyInteger*>(retained)->getValue(), initialValue);
        EXPECT_FALSE(_sheet->getCellAddress(retained, mapped));
    }

    void expectAdoptedDerivedReuseRefreshes(bool persistent)
    {
        _sheet->setCell("A1", "7");
        App::Property* adopted = nullptr;
        if (persistent) {
            adopted = _sheet->addDynamicProperty("App::PropertyInteger", "A2");
            ASSERT_NE(adopted, nullptr);
            static_cast<App::PropertyInteger*>(adopted)->setValue(101);
            App::CellAddress mapped;
            ASSERT_FALSE(_sheet->getCellAddress(adopted, mapped));
        }
        _sheet->setCell("A2", "7");
        ASSERT_TRUE(_document->recompute()); // Legacy same-type adoption.
        if (!persistent) {
            adopted = _sheet->App::PropertyContainer::getDynamicPropertyByName("A2");
        }
        ASSERT_NE(adopted, nullptr);
        ASSERT_EQ(_sheet->App::PropertyContainer::getDynamicPropertyByName("A2"), adopted);
        const auto propertyId = adopted->getID();
        const auto propertyType = adopted->getTypeId();
        ASSERT_EQ(propertyType, App::PropertyInteger::getClassTypeId());
        App::CellAddress mapped;
        ASSERT_TRUE(_sheet->getCellAddress(adopted, mapped));
        ASSERT_EQ(mapped, App::CellAddress("A2"));
        EXPECT_EQ(static_cast<App::PropertyInteger*>(adopted)->getValue(), 7);
        EXPECT_EQ(adopted->getType() & App::Prop_NoPersist,
                  persistent ? 0 : App::Prop_NoPersist);
        EXPECT_EQ(adopted->testStatus(App::Property::ReadOnly), !persistent);
        EXPECT_EQ(adopted->testStatus(App::Property::Hidden), !persistent);
        auto* producer = makeCellClearProducer(true);
        useLiteralDerivedInput(producer);
        auto* dependent = freecad_cast<Spreadsheet::Sheet*>(
            _document->addObject("Spreadsheet::Sheet", "Dependent"));
        ASSERT_NE(dependent, nullptr);
        auto* anchor = static_cast<App::PropertyLink*>(
            dependent->addDynamicProperty("App::PropertyLink", "ProducerAnchor"));
        anchor->setValue(producer);
        dependent->setCell("A1", "=Sheet.A2*2");
        ASSERT_TRUE(_document->recompute());
        ASSERT_EQ(static_cast<App::PropertyInteger*>(dependent->getPropertyByName("A1"))
                      ->getValue(), 14);
        int snapshots = 0;
        fastsignals::scoped_connection connection = _sheet->cellUpdated.connect(
            [&](App::CellAddress address) {
                if (address != App::CellAddress("A2") || snapshots != 0
                    || !producer->testStatus(App::ObjectStatus::Recompute)) {
                    return;
                }
                ++snapshots;
                auto* current = _sheet->App::PropertyContainer::getDynamicPropertyByName("A2");
                EXPECT_EQ(current, adopted);
                if (current != adopted) {
                    return;
                }
                EXPECT_EQ(current->getID(), propertyId);
                EXPECT_EQ(current->getTypeId(), propertyType);
                EXPECT_EQ(static_cast<App::PropertyInteger*>(current)->getValue(), 13);
                EXPECT_TRUE(_sheet->getCellAddress(current, mapped));
                EXPECT_EQ(mapped, App::CellAddress("A2"));
            });
        armCellClearProducer(producer);
        producer->touch();
        ASSERT_TRUE(_document->recompute()); // ONE public call, no repair.
        EXPECT_EQ(snapshots, 1);
        auto* current = _sheet->App::PropertyContainer::getDynamicPropertyByName("A2");
        ASSERT_EQ(current, adopted);
        EXPECT_EQ(current->getID(), propertyId);
        EXPECT_EQ(current->getTypeId(), propertyType);
        EXPECT_EQ(static_cast<App::PropertyInteger*>(current)->getValue(), 13);
        ASSERT_TRUE(_sheet->getCellAddress(current, mapped));
        EXPECT_EQ(mapped, App::CellAddress("A2"));
        auto* output = freecad_cast<App::PropertyInteger*>(dependent->getPropertyByName("A1"));
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(output->getValue(), 26);
        for (auto* object : {producer, static_cast<App::DocumentObject*>(_sheet),
                             static_cast<App::DocumentObject*>(dependent)}) {
            EXPECT_TRUE(object->isValid()) << _document->getErrorDescription(object);
            EXPECT_FALSE(object->isTouched());
            EXPECT_FALSE(object->isError());
        }
        EXPECT_FALSE(_document->mustExecute());
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

// A user typo such as a self-referencing formula makes the sheet itself fail
// the commit recompute. Rollback verification then re-executes the restored
// sheet outside the commit transaction; that must not poison the document.
TEST_F(SpreadsheetCollaborationCompatibilityTest,
       cyclicFormulaRollsBackWithoutBlockingLaterCommits)
{
    const auto cyclic = commitCell("B1", "=B1 + 1");

    EXPECT_EQ(cyclic.status, App::DocumentCommitStatus::RecomputeFailed) << cyclic.message;
    const auto* cell = _sheet->getCell(App::CellAddress("B1"));
    EXPECT_TRUE(cell == nullptr || !cell->isUsed());
    const auto poll = _document->collaborationRevisions().pollPublications(_cursor);
    EXPECT_TRUE(poll.events.empty());

    const auto next = commitCell("A1", "=1+2");
    EXPECT_EQ(next.status, App::DocumentCommitStatus::Committed) << next.message;
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


TEST_F(SpreadsheetCollaborationCompatibilityTest,
       ordinaryCellClearDeduplicatesNativeAlias)
{
    _sheet->setCell("A1", "7");
    _sheet->setAlias(App::CellAddress("A1"), "NativeAlias");
    ASSERT_TRUE(_document->recompute());
    auto* canonical = _sheet->getPropertyByName("A1");
    ASSERT_NE(canonical, nullptr);
    ASSERT_EQ(_sheet->getPropertyByName("NativeAlias"), canonical);
    auto* note = _sheet->addDynamicProperty("App::PropertyString", "UserNote");
    int removed = 0;
    fastsignals::scoped_connection connection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const App::Property& property) {
                if (property.getContainer() == _sheet) {
                    ++removed;
                    EXPECT_EQ(removed, 1);
                    EXPECT_STREQ(property.getName(), "A1");
                    // Ordinary clear keeps its legacy cell/alias-before-notify
                    // order, including the raw cache still present at notify.
                    EXPECT_EQ(_sheet->getCell(App::CellAddress("A1")), nullptr);
                    EXPECT_EQ(_sheet->getPropertyByName("NativeAlias"), nullptr);
                    EXPECT_EQ(_sheet->getPropertyByName("A1"), canonical);
                    App::CellAddress mapped;
                    EXPECT_FALSE(_sheet->getCellAddress(canonical, mapped));
                }
            });
    EXPECT_FALSE(_sheet->removeDynamicProperty("NativeAlias"));
    EXPECT_EQ(removed, 0);
    EXPECT_EQ(_sheet->getPropertyByName("A1"), canonical);
    EXPECT_EQ(_sheet->getPropertyByName("NativeAlias"), canonical);
    _sheet->clear(App::CellAddress("A1"));
    EXPECT_EQ(removed, 1);
    EXPECT_EQ(_sheet->getCell(App::CellAddress("A1")), nullptr);
    EXPECT_EQ(_sheet->getPropertyByName("A1"), nullptr);
    EXPECT_EQ(_sheet->getPropertyByName("NativeAlias"), nullptr);
    EXPECT_EQ(_sheet->getPropertyByName("UserNote"), note);
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       linkedProducerRefreshesCacheBeforeDependentInOnePublicRecompute)
{
    _sheet->setCell("A1", "Amount");
    _sheet->setCell("A2", "7");
    _sheet->setCell("A3", "obsolete");
    ASSERT_TRUE(_document->recompute());
    auto* producer = makeCellClearProducer(true);
    setProducerClearAddress(producer, "A1:A3");
    auto* dependent = freecad_cast<Spreadsheet::Sheet*>(
        _document->addObject("Spreadsheet::Sheet", "Dependent"));
    ASSERT_NE(dependent, nullptr);
    auto* anchor = static_cast<App::PropertyLink*>(
        dependent->addDynamicProperty("App::PropertyLink", "ProducerAnchor"));
    anchor->setValue(producer);
    dependent->setCell("A1", "=Sheet.A2*2");
    ASSERT_TRUE(_document->recompute());
    ASSERT_EQ(static_cast<App::PropertyInteger*>(dependent->getPropertyByName("A1"))
                  ->getValue(), 14);

    int cacheCallbacks = 0;
    auto* note = _sheet->addDynamicProperty("App::PropertyString", "UserNote");
    auto* custom = _sheet->addDynamicProperty(
        "App::PropertyString", "CustomCache", nullptr, nullptr, App::Prop_NoPersist);
    fastsignals::scoped_connection connection = _sheet->cellUpdated.connect(
        [&](App::CellAddress address) {
            if (address != App::CellAddress("A2")) {
                return;
            }
            ++cacheCallbacks;
            EXPECT_TRUE(_document->hasPendingTransaction());
            EXPECT_TRUE(producer->testStatus(App::ObjectStatus::Recompute));
            // Derived refresh must not mark the target as an executing feature
            // or let synchronous native signals borrow its schema privilege.
            EXPECT_FALSE(_sheet->testStatus(App::ObjectStatus::Recompute));
            EXPECT_THROW(_sheet->removeDynamicProperty("UserNote"), Base::Exception);
            EXPECT_THROW(_sheet->removeDynamicProperty("CustomCache"), Base::Exception);
            EXPECT_THROW(_sheet->addDynamicProperty("App::PropertyString", "Borrowed",
                nullptr, nullptr, App::Prop_NoPersist), Base::Exception);
            EXPECT_THROW(custom->setStatus(App::Property::Hidden, true), Base::Exception);
            EXPECT_THROW(_sheet->clear(App::CellAddress("A1")), Base::Exception);
            EXPECT_THROW(_document->recomputeFeature(_sheet), Base::Exception);
            EXPECT_THROW(_document->recomputeFeature(producer), Base::Exception);
            EXPECT_THROW(_sheet->recomputeCells(App::Range("A1:A2")), Base::Exception);
            EXPECT_THROW(_document->recompute(), Base::Exception);
        });

    armCellClearProducer(producer);
    producer->touch();
    ASSERT_TRUE(_document->recompute()); // ONE public call; no observer repair/retry.
    {
        Base::PyGILStateLocker gil;
        PyObjectRef object(producer->getPyObject());
        PyObjectRef proxy(PyObject_GetAttrString(object.get(), "Proxy"));
        ASSERT_NE(proxy.get(), nullptr);
        PyObjectRef snapshot(PyObject_GetAttrString(proxy.get(), "derived_snapshot"));
        ASSERT_NE(snapshot.get(), nullptr);
        ASSERT_TRUE(PyTuple_Check(snapshot.get()));
        ASSERT_EQ(PyTuple_Size(snapshot.get()), 4);
        // Snapshot is taken INSIDE native producer execute, before purgeTouched,
        // producer return, coordinator continuation, or deferred observers.
        EXPECT_TRUE(PyUnicode_Check(PyTuple_GetItem(snapshot.get(), 0)));
        if (PyUnicode_Check(PyTuple_GetItem(snapshot.get(), 0))) {
            EXPECT_STREQ(PyUnicode_AsUTF8(PyTuple_GetItem(snapshot.get(), 0)), "Amount");
        }
        EXPECT_TRUE(PyLong_Check(PyTuple_GetItem(snapshot.get(), 1)));
        if (PyLong_Check(PyTuple_GetItem(snapshot.get(), 1))) {
            EXPECT_EQ(PyLong_AsLong(PyTuple_GetItem(snapshot.get(), 1)), 13);
        }
        EXPECT_EQ(PyObject_IsTrue(PyTuple_GetItem(snapshot.get(), 2)), 0);
        EXPECT_STREQ(PyUnicode_AsUTF8(PyTuple_GetItem(snapshot.get(), 3)), "");
        PyObjectRef errors(PyObject_GetAttrString(proxy.get(), "errors"));
        ASSERT_NE(errors.get(), nullptr);
        EXPECT_EQ(PyTuple_Size(errors.get()), 0);
        EXPECT_FALSE(PyErr_Occurred());
    }
    EXPECT_EQ(cacheCallbacks, 1);
    auto* result = freecad_cast<App::PropertyInteger*>(dependent->getPropertyByName("A1"));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getValue(), 26);
    auto* cache = freecad_cast<App::PropertyInteger*>(_sheet->getPropertyByName("A2"));
    ASSERT_NE(cache, nullptr);
    EXPECT_EQ(cache->getValue(), 13);
    EXPECT_EQ(_sheet->getPropertyByName("A3"), nullptr);
    EXPECT_EQ(_sheet->getCell(App::CellAddress("A3")), nullptr);
    EXPECT_EQ(_sheet->getPropertyByName("UserNote"), note);
    EXPECT_EQ(_sheet->getPropertyByName("CustomCache"), custom);
    EXPECT_EQ(_sheet->getPropertyByName("Borrowed"), nullptr);
    EXPECT_FALSE(custom->testStatus(App::Property::Hidden));
    const auto& outList = dependent->getOutList();
    EXPECT_NE(std::ranges::find(outList, _sheet), outList.end());
    const auto& inList = _sheet->getInList();
    EXPECT_NE(std::ranges::find(inList, dependent), inList.end());
    for (auto* object : {producer, static_cast<App::DocumentObject*>(_sheet),
                         static_cast<App::DocumentObject*>(dependent)}) {
        EXPECT_TRUE(object->isValid()) << _document->getErrorDescription(object);
        EXPECT_FALSE(object->isTouched());
        EXPECT_FALSE(object->isError());
    }
    EXPECT_FALSE(_document->mustExecute());
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       linkedProducerClearSupportsAdoptedPersistentCell)
{
    auto* adopted = _sheet->addDynamicProperty("App::PropertyInteger", "A1");
    ASSERT_NE(adopted, nullptr);
    const auto adoptedId = adopted->getID();
    const auto adoptedType = adopted->getTypeId();
    ASSERT_EQ(adoptedType, App::PropertyInteger::getClassTypeId());
    _sheet->setCell("A1", "7");
    _sheet->setAlias(App::CellAddress("A1"), "NativeAlias");
    ASSERT_TRUE(_document->recompute());
    ASSERT_EQ(_sheet->getPropertyByName("A1"), adopted);
    EXPECT_EQ(adopted->getID(), adoptedId);
    EXPECT_EQ(adopted->getTypeId(), adoptedType);
    ASSERT_EQ(adopted->getType() & App::Prop_NoPersist, 0);
    EXPECT_FALSE(adopted->testStatus(App::Property::ReadOnly));
    EXPECT_FALSE(adopted->testStatus(App::Property::Hidden));
    App::CellAddress address;
    ASSERT_TRUE(_sheet->getCellAddress(adopted, address));
    ASSERT_EQ(address, App::CellAddress("A1"));
    auto* note = _sheet->addDynamicProperty("App::PropertyString", "UserNote");
    auto* customCache = _sheet->addDynamicProperty(
        "App::PropertyString", "CustomCache", nullptr, nullptr, App::Prop_NoPersist);
    auto* producer = makeCellClearProducer();
    const auto identity = _document->collaborationObjectIdentity(*_sheet);
    _cursor = _document->collaborationRevisions().pollPublications(_cursor).nextCursor;
    const auto structureKey = App::DocumentRevisionKey::objectStructure("Sheet");
    const auto wildcardKey = App::DocumentRevisionKey::unknownModelMutation();
    const auto structureBefore = _document->collaborationRevisions().current(structureKey);
    const auto wildcardBefore = _document->collaborationRevisions().current(wildcardKey);
    int removed = 0;
    fastsignals::scoped_connection connection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const App::Property& property) {
                if (property.getContainer() == _sheet) {
                    ++removed;
                    EXPECT_STREQ(property.getName(), "A1");
                    EXPECT_EQ(property.getID(), adoptedId);
                    EXPECT_EQ(property.getTypeId(), adoptedType);
                    EXPECT_FALSE(_document->hasPendingTransaction());
                }
            });
    const auto result = commitProducer(producer);
    ASSERT_EQ(result.status, App::DocumentCommitStatus::Committed) << result.message;
    EXPECT_EQ(removed, 1);
    auto* fresh = freecad_cast<App::PropertyInteger*>(_sheet->getPropertyByName("A1"));
    ASSERT_NE(fresh, nullptr);
    // Public native IDs distinguish the removed lifetime from its same-name,
    // same-type replacement without relying on allocator pointer inequality.
    EXPECT_NE(fresh->getID(), adoptedId);
    EXPECT_EQ(fresh->getTypeId(), adoptedType);
    EXPECT_EQ(fresh->getValue(), 13);
    EXPECT_EQ(_sheet->getPropertyByName("NativeAlias"), nullptr);
    EXPECT_EQ(_sheet->getPropertyByName("UserNote"), note);
    EXPECT_EQ(_sheet->getPropertyByName("CustomCache"), customCache);
    EXPECT_EQ(_document->collaborationObjectIdentity(*_sheet), identity);
    EXPECT_FALSE(_document->mustExecute());
    const auto poll = _document->collaborationRevisions().pollPublications(_cursor);
    ASSERT_EQ(poll.events.size(), 1U);
    EXPECT_EQ(_document->collaborationRevisions().current(structureKey), structureBefore + 1);
    EXPECT_EQ(_document->collaborationRevisions().current(wildcardKey), wildcardBefore + 1);
    const auto& changes = poll.events.front().changes;
    EXPECT_EQ(std::ranges::count_if(changes, [&](const auto& change) {
        return change.key == App::DocumentRevisionKey::objectStructure("Sheet")
            && change.stableObjectIdentity == identity;
    }), 1);
    EXPECT_EQ(std::ranges::count_if(changes, [](const auto& change) {
        return change.key == App::DocumentRevisionKey::unknownModelMutation();
    }), 1);
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       linkedProducerCannotClearUnadoptedPersistentCell)
{
    auto* raw = _sheet->addDynamicProperty("App::PropertyInteger", "A1");
    static_cast<App::PropertyInteger*>(raw)->setValue(7);
    App::CellAddress address;
    ASSERT_FALSE(_sheet->getCellAddress(raw, address));
    auto* producer = makeCellClearProducer();
    _cursor = _document->collaborationRevisions().pollPublications(_cursor).nextCursor;
    int removed = 0;
    fastsignals::scoped_connection connection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const App::Property& property) {
                if (property.getContainer() == _sheet) {
                    ++removed;
                }
            });
    const auto result = commitProducer(producer);
    EXPECT_EQ(result.status, App::DocumentCommitStatus::RecomputeFailed) << result.message;
    auto* retained = freecad_cast<App::PropertyInteger*>(_sheet->getPropertyByName("A1"));
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->getValue(), 7);
    EXPECT_EQ(_sheet->getCell(App::CellAddress("A1")), nullptr);
    EXPECT_EQ(removed, 0);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(_cursor).events.empty());
    // Finish the permanently denied fixture after preserving its evidence;
    // a pending producer must not affect the later healthy commit assertion.
    _document->removeObject(producer->getNameInDocument());
    bool hasError = true;
    static_cast<void>(_document->recompute({}, false, &hasError));
    ASSERT_FALSE(hasError);
    ASSERT_FALSE(_document->mustExecute());
    EXPECT_EQ(commitCell("B1", "9").status, App::DocumentCommitStatus::Committed);
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       linkedProducerCannotClearUnadoptedNoPersistCell)
{
    auto* raw = _sheet->addDynamicProperty("App::PropertyInteger", "A1", nullptr, nullptr, App::Prop_NoPersist);
    static_cast<App::PropertyInteger*>(raw)->setValue(7);
    App::CellAddress address;
    ASSERT_FALSE(_sheet->getCellAddress(raw, address));
    auto* producer = makeCellClearProducer();
    _cursor = _document->collaborationRevisions().pollPublications(_cursor).nextCursor;
    int removed = 0;
    fastsignals::scoped_connection connection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const App::Property& property) {
                if (property.getContainer() == _sheet) {
                    ++removed;
                }
            });
    const auto result = commitProducer(producer);
    EXPECT_EQ(result.status, App::DocumentCommitStatus::RecomputeFailed) << result.message;
    auto* retained = freecad_cast<App::PropertyInteger*>(_sheet->getPropertyByName("A1"));
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->getValue(), 7);
    EXPECT_EQ(_sheet->getCell(App::CellAddress("A1")), nullptr);
    EXPECT_EQ(removed, 0);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(_cursor).events.empty());
    // Finish the permanently denied fixture after preserving its evidence;
    // a pending producer must not affect the later healthy commit assertion.
    _document->removeObject(producer->getNameInDocument());
    bool hasError = true;
    static_cast<void>(_document->recompute({}, false, &hasError));
    ASSERT_FALSE(hasError);
    ASSERT_FALSE(_document->mustExecute());
    EXPECT_EQ(commitCell("B1", "9").status, App::DocumentCommitStatus::Committed);
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       cellClearCallbacksCannotBorrowNativeRemoval)
{
    _sheet->setCell("A1", "7");
    _sheet->setCell("B1", "9");
    ASSERT_TRUE(_document->recompute());
    auto* note = _sheet->addDynamicProperty("App::PropertyString", "UserNote");
    auto* custom = _sheet->addDynamicProperty(
        "App::PropertyString", "CustomCache", nullptr, nullptr, App::Prop_NoPersist);
    auto* otherSheet = freecad_cast<Spreadsheet::Sheet*>(
        _document->addObject("Spreadsheet::Sheet", "OtherSheet"));
    ASSERT_NE(otherSheet, nullptr);
    otherSheet->setCell("A1", "21");
    const auto foreignName = App::GetApplication().getUniqueDocumentName("cellClearForeign");
    auto* foreignDocument = App::GetApplication().newDocument(foreignName.c_str());
    Base::ScopeGuard closeForeign([&] {
        App::GetApplication().closeDocument(foreignName.c_str());
    });
    auto* foreignSheet = freecad_cast<Spreadsheet::Sheet*>(
        foreignDocument->addObject("Spreadsheet::Sheet", "ForeignSheet"));
    ASSERT_NE(foreignSheet, nullptr);
    foreignSheet->setCell("A1", "22");
    ASSERT_TRUE(foreignDocument->recompute());
    auto* producer = makeCellClearProducer();
    // Sheet::onChanged emits this actual C++ signal synchronously while
    // cells.clear is finishing. Document before-change observers are deferred.
    _sheet->setCopyOrCutRanges({App::Range("A1:A1")});
    int callbacks = 0;
    fastsignals::scoped_connection connection = _sheet->rangeUpdated.connect(
        [&](const App::Range&) {
            if (callbacks != 0) {
                return;
            }
            ++callbacks;
            EXPECT_TRUE(_document->testStatus(App::Document::Recomputing));
            EXPECT_TRUE(_document->hasPendingTransaction());
            EXPECT_TRUE(producer->testStatus(App::ObjectStatus::Recompute));
            // rangeUpdated is synchronous in onChanged(Cells), after physical
            // cache removal and stored cell/alias cleanup, before regeneration.
            EXPECT_EQ(_sheet->getPropertyByName("A1"), nullptr);
            EXPECT_EQ(_sheet->getCell(App::CellAddress("A1")), nullptr);
            ASSERT_NE(_sheet->getPropertyByName("B1"), nullptr);
            ASSERT_NE(_sheet->getCell(App::CellAddress("B1")), nullptr);
            EXPECT_THROW(_sheet->clear(App::CellAddress("B1")), Base::Exception);
            EXPECT_THROW(otherSheet->clear(App::CellAddress("A1")), Base::Exception);
            EXPECT_THROW(foreignSheet->clear(App::CellAddress("A1")), Base::Exception);
            EXPECT_THROW(_sheet->clear(App::CellAddress("A1")), Base::Exception);
            EXPECT_THROW(_sheet->removeDynamicProperty("UserNote"), Base::Exception);
            EXPECT_THROW(_sheet->removeDynamicProperty("CustomCache"), Base::Exception);
            EXPECT_THROW(_sheet->addDynamicProperty("App::PropertyString", "Borrowed",
                nullptr, nullptr, App::Prop_NoPersist), Base::Exception);
            EXPECT_THROW(note->setStatus(App::Property::Hidden, true), Base::Exception);
            EXPECT_THROW(custom->setStatus(App::Property::Hidden, true), Base::Exception);
            auto* sibling = _sheet->getPropertyByName("B1");
            ASSERT_NE(sibling, nullptr);
            EXPECT_THROW(sibling->setStatus(App::Property::LockDynamic, true), Base::Exception);
        });
    const auto result = commitProducer(producer);
    ASSERT_EQ(result.status, App::DocumentCommitStatus::Committed) << result.message;
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(_sheet->getPropertyByName("UserNote"), note);
    EXPECT_EQ(_sheet->getPropertyByName("CustomCache"), custom);
    EXPECT_EQ(_sheet->getPropertyByName("Borrowed"), nullptr);
    EXPECT_FALSE(note->testStatus(App::Property::Hidden));
    EXPECT_FALSE(custom->testStatus(App::Property::Hidden));
    auto* b1 = freecad_cast<App::PropertyInteger*>(_sheet->getPropertyByName("B1"));
    ASSERT_NE(b1, nullptr);
    EXPECT_EQ(b1->getValue(), 9);
    EXPECT_FALSE(b1->testStatus(App::Property::LockDynamic));
    auto* otherValue = freecad_cast<App::PropertyInteger*>(otherSheet->getPropertyByName("A1"));
    auto* foreignValue = freecad_cast<App::PropertyInteger*>(foreignSheet->getPropertyByName("A1"));
    ASSERT_NE(otherValue, nullptr);
    ASSERT_NE(foreignValue, nullptr);
    EXPECT_EQ(otherValue->getValue(), 21);
    EXPECT_EQ(foreignValue->getValue(), 22);
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       linkedProducerClearRollbackRestoresAliasAndDependencies)
{
    _sheet->setCell("A1", "=1+6");
    _sheet->setAlias(App::CellAddress("A1"), "NativeAlias");
    _sheet->setCell("B1", "=A1*2");
    ASSERT_TRUE(_document->recompute());
    const auto expression = _sheet->getCell(App::CellAddress("A1"))->getExpression()->toString();
    auto* producer = makeCellClearProducer();
    _failure->Source1.setValue(producer);
    ASSERT_TRUE(_document->recompute());
    _cursor = _document->collaborationRevisions().pollPublications(_cursor).nextCursor;
    int removed = 0;
    int appended = 0;
    fastsignals::scoped_connection removeConnection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const App::Property& property) {
                if (property.getContainer() == _sheet) { ++removed; }
            });
    fastsignals::scoped_connection appendConnection =
        App::GetApplication().signalAppendDynamicProperty.connect(
            [&](const App::Property& property) {
                if (property.getContainer() == _sheet) { ++appended; }
            });
    const auto result = commitProducer(producer, true);
    EXPECT_EQ(result.status, App::DocumentCommitStatus::RecomputeFailed) << result.message;
    auto* restored = freecad_cast<App::PropertyInteger*>(_sheet->getPropertyByName("A1"));
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->getValue(), 7);
    EXPECT_EQ(_sheet->getPropertyByName("NativeAlias"), restored);
    ASSERT_NE(_sheet->getCell(App::CellAddress("A1")), nullptr);
    EXPECT_EQ(_sheet->getCell(App::CellAddress("A1"))->getExpression()->toString(), expression);
    EXPECT_EQ(removed, 0);
    EXPECT_EQ(appended, 0);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(_cursor).events.empty());
    EXPECT_EQ(commitCell("A1", "8").status, App::DocumentCommitStatus::Committed);
    auto* dependent = freecad_cast<App::PropertyInteger*>(_sheet->getPropertyByName("B1"));
    ASSERT_NE(dependent, nullptr);
    EXPECT_EQ(dependent->getValue(), 16);
}


TEST_F(SpreadsheetCollaborationCompatibilityTest,
       cellClearPreflightRetainsLockedCellAndAlias)
{
    _sheet->setCell("A1", "7");
    _sheet->setAlias(App::CellAddress("A1"), "NativeAlias");
    ASSERT_TRUE(_document->recompute());
    auto* producer = makeCellClearProducer();
    auto* property = _sheet->getPropertyByName("A1");
    ASSERT_NE(property, nullptr);
    property->setStatus(App::Property::LockDynamic, true);
    _cursor = _document->collaborationRevisions().pollPublications(_cursor).nextCursor;
    int removed = 0;
    fastsignals::scoped_connection connection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const App::Property& prop) {
                if (prop.getContainer() == _sheet) { ++removed; }
            });
    const auto denied = commitProducer(producer);
    EXPECT_EQ(denied.status, App::DocumentCommitStatus::RecomputeFailed) << denied.message;
    expectProducerErrorSnapshot(producer, "7", true, "NativeAlias", 7);
    auto* retained = _sheet->getPropertyByName("A1");
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(_sheet->getPropertyByName("NativeAlias"), retained);
    EXPECT_TRUE(retained->testStatus(App::Property::LockDynamic));
    App::CellAddress mapped;
    EXPECT_TRUE(_sheet->getCellAddress(retained, mapped));
    EXPECT_EQ(mapped, App::CellAddress("A1"));
    EXPECT_EQ(removed, 0);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(_cursor).events.empty());
    retained->setStatus(App::Property::LockDynamic, false);
    _cursor = _document->collaborationRevisions().pollPublications(_cursor).nextCursor;
    const auto healthy = commitProducer(producer);
    ASSERT_EQ(healthy.status, App::DocumentCommitStatus::Committed) << healthy.message;
    EXPECT_EQ(removed, 1);
    auto* fresh = freecad_cast<App::PropertyInteger*>(_sheet->getPropertyByName("A1"));
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(fresh->getValue(), 13);
    EXPECT_EQ(_sheet->getPropertyByName("NativeAlias"), nullptr);
    expectSingleSpreadsheetPublication();
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       readOnlyCellClearFailsBeforeInputOrSchemaMutation)
{
    _sheet->setCell("A1", "7");
    _sheet->setAlias(App::CellAddress("A1"), "NativeAlias");
    ASSERT_TRUE(_document->recompute());
    auto* property = _sheet->getPropertyByName("A1");
    App::beginCollaborationReadOnlyMutationTarget(*_document);
    Base::ScopeGuard end([&] { App::endCollaborationReadOnlyMutationTarget(*_document); });
    EXPECT_THROW(_sheet->clear(App::CellAddress("A1")), Base::Exception);
    EXPECT_EQ(_sheet->getPropertyByName("A1"), property);
    EXPECT_EQ(_sheet->getPropertyByName("NativeAlias"), property);
    ASSERT_NE(_sheet->getCell(App::CellAddress("A1")), nullptr);
    EXPECT_TRUE(_sheet->getCell(App::CellAddress("A1"))->isUsed());
}


TEST_F(SpreadsheetCollaborationCompatibilityTest,
       stableReadCellClearCannotAcquireRecomputeAuthority)
{
    _sheet->setCell("A1", "7");
    _sheet->setAlias(App::CellAddress("A1"), "NativeAlias");
    ASSERT_TRUE(_document->recompute());
    auto* property = _sheet->getPropertyByName("A1");
    App::Internal::DocumentStructuralCompatibilityTestAccess::withStableRead(*_document, [&] {
        EXPECT_THROW(_sheet->clear(App::CellAddress("A1")), Base::Exception);
        EXPECT_EQ(_sheet->getPropertyByName("A1"), property);
        EXPECT_EQ(_sheet->getPropertyByName("NativeAlias"), property);
        ASSERT_NE(_sheet->getCell(App::CellAddress("A1")), nullptr);
        EXPECT_TRUE(_sheet->getCell(App::CellAddress("A1"))->isUsed());
    });
    EXPECT_NO_THROW(_sheet->clear(App::CellAddress("A1")));
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       cellClearRejectsRenamedCacheWithWrongAddressMapping)
{
    _sheet->setCell("A1", "7");
    ASSERT_TRUE(_document->recompute());
    auto* producer = makeCellClearProducer();
    auto* property = _sheet->getPropertyByName("A1");
    ASSERT_TRUE(_sheet->renameDynamicProperty(property, "B1"));
    App::CellAddress mapped;
    ASSERT_TRUE(_sheet->getCellAddress(property, mapped));
    ASSERT_EQ(mapped, App::CellAddress("A1"));
    _cursor = _document->collaborationRevisions().pollPublications(_cursor).nextCursor;
    int removed = 0;
    fastsignals::scoped_connection connection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const App::Property& prop) {
                if (prop.getContainer() == _sheet) { ++removed; }
            });
    for (const char* address : {"A1", "B1"}) {
        setProducerClearAddress(producer, address);
        const auto denied = commitProducer(producer);
        EXPECT_EQ(denied.status, App::DocumentCommitStatus::RecomputeFailed) << denied.message;
        expectProducerErrorSnapshot(producer, "7", false, nullptr, 7, true);
        auto* retained = _sheet->getPropertyByName("B1");
        ASSERT_NE(retained, nullptr);
        EXPECT_EQ(static_cast<App::PropertyInteger*>(retained)->getValue(), 7);
        ASSERT_TRUE(_sheet->getCellAddress(retained, mapped));
        EXPECT_EQ(mapped, App::CellAddress("A1"));
        ASSERT_NE(_sheet->getCell(App::CellAddress("A1")), nullptr);
        EXPECT_TRUE(_sheet->getCell(App::CellAddress("A1"))->isUsed());
        EXPECT_EQ(removed, 0);
        EXPECT_TRUE(_document->collaborationRevisions().pollPublications(_cursor).events.empty());
    }
    property = _sheet->getPropertyByName("B1");
    ASSERT_TRUE(_sheet->renameDynamicProperty(property, "A1"));
    setProducerClearAddress(producer, "A1");
    _cursor = _document->collaborationRevisions().pollPublications(_cursor).nextCursor;
    const auto healthy = commitProducer(producer);
    ASSERT_EQ(healthy.status, App::DocumentCommitStatus::Committed) << healthy.message;
    EXPECT_EQ(removed, 1);
    expectSingleSpreadsheetPublication();
}


TEST_F(SpreadsheetCollaborationCompatibilityTest,
       nativeExecuteClearsDirtyMissingCellWithoutNestedSchemaScope)
{
    _sheet->setCell("A1", "7");
    ASSERT_TRUE(_document->recompute());
    _cursor = _document->collaborationRevisions().pollPublications(_cursor).nextCursor;
    int removed = 0;
    fastsignals::scoped_connection connection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const App::Property& property) {
                if (property.getContainer() == _sheet) {
                    ++removed;
                    EXPECT_STREQ(property.getName(), "A1");
                }
            });
    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::UnknownModel;
    const auto result = _document->collaborationService().commitCompatibilityMutation(
        std::move(mutation), [this] {
            // Delete the stored cell through its native property. Its cache
            // survives until execute -> updateProperty -> Sheet::clear.
            _sheet->getCells()->clear(App::CellAddress("A1"));
            ASSERT_NE(_sheet->getPropertyByName("A1"), nullptr);
        });
    ASSERT_EQ(result.status, App::DocumentCommitStatus::Committed) << result.message;
    EXPECT_EQ(removed, 1);
    EXPECT_EQ(_sheet->getPropertyByName("A1"), nullptr);
    EXPECT_EQ(_sheet->getCell(App::CellAddress("A1")), nullptr);
    EXPECT_FALSE(_document->mustExecute());
    expectSingleSpreadsheetPublication();
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       nativeExecuteCallbacksCannotBorrowCellClearAuthority)
{
    _sheet->setCell("A1", "7");
    _sheet->setAlias(App::CellAddress("A1"), "NativeAlias");
    _sheet->setCell("B1", "9");
    _sheet->setCell("C1", "10");
    ASSERT_TRUE(_document->recompute());
    auto* custom = _sheet->addDynamicProperty(
        "App::PropertyString", "CustomCache", nullptr, nullptr, App::Prop_NoPersist);
    auto* sibling = _sheet->getPropertyByName("B1");
    ASSERT_NE(sibling, nullptr);
    _cursor = _document->collaborationRevisions().pollPublications(_cursor).nextCursor;
    bool clearing = false;
    int executeCallbacks = 0;
    int clearCallbacks = 0;
    int removed = 0;
    int appended = 0;
    fastsignals::scoped_connection removeConnection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const App::Property& prop) {
                if (prop.getContainer() == _sheet) {
                    ++removed;
                    EXPECT_STREQ(prop.getName(), "A1");
                }
            });
    fastsignals::scoped_connection appendConnection =
        App::GetApplication().signalAppendDynamicProperty.connect(
            [&](const App::Property& prop) {
                if (prop.getContainer() == _sheet) { ++appended; }
            });
    fastsignals::scoped_connection rangeConnection = _sheet->rangeUpdated.connect(
        [&](const App::Range&) {
            // setCopyOrCutRanges also emits; only observe actual clear cleanup.
            if (!clearing || clearCallbacks != 0) { return; }
            ++clearCallbacks;
            EXPECT_TRUE(_document->testStatus(App::Document::Recomputing));
            EXPECT_TRUE(_document->hasPendingTransaction());
            EXPECT_TRUE(_sheet->testStatus(App::ObjectStatus::Recompute));
            EXPECT_EQ(_sheet->getPropertyByName("A1"), nullptr);
            EXPECT_EQ(_sheet->getPropertyByName("NativeAlias"), nullptr);
            EXPECT_EQ(_sheet->getCell(App::CellAddress("A1")), nullptr);
            EXPECT_EQ(_sheet->getPropertyByName("B1"), sibling);
            EXPECT_THROW(_sheet->removeDynamicProperty("CustomCache"), Base::Exception);
            EXPECT_THROW(_sheet->removeDynamicProperty("B1"), Base::Exception);
            EXPECT_THROW(_sheet->addDynamicProperty("App::PropertyString", "Borrowed",
                nullptr, nullptr, App::Prop_NoPersist), Base::Exception);
            EXPECT_THROW(custom->setStatus(App::Property::Hidden, true), Base::Exception);
            EXPECT_THROW(sibling->setStatus(App::Property::LockDynamic, true), Base::Exception);
            EXPECT_THROW(_sheet->clear(App::CellAddress("B1")), Base::Exception);
            EXPECT_THROW(_sheet->clear(App::CellAddress("A1")), Base::Exception);
        });
    fastsignals::scoped_connection cellConnection = _sheet->cellUpdated.connect(
        [&](App::CellAddress address) {
            // updateProperty emits cellUpdated inside Sheet::execute's existing
            // schema scope. Nonempty setCell in apply does not emit this signal.
            if (address != App::CellAddress("C1") || executeCallbacks != 0) { return; }
            ++executeCallbacks;
            EXPECT_TRUE(_document->testStatus(App::Document::Recomputing));
            EXPECT_TRUE(_sheet->testStatus(App::ObjectStatus::Recompute));
            auto* c1 = freecad_cast<App::PropertyInteger*>(_sheet->getPropertyByName("C1"));
            ASSERT_NE(c1, nullptr);
            EXPECT_EQ(c1->getValue(), 11);
            _sheet->setCopyOrCutRanges({App::Range("A1:A1")});
            clearing = true;
            Base::ScopeGuard finish([&] { clearing = false; });
            _sheet->clear(App::CellAddress("A1"));
            clearing = false;
            // The callback dirties A1 after execute took its dirty-set snapshot.
            // Native recomputeCells now reaches updateProperty -> clear for this
            // missing cell within the same execute, without another schema scope.
            _sheet->recomputeCells(App::Range("A1:A1"));
        });
    const auto result = commitCell("C1", "11");
    ASSERT_EQ(result.status, App::DocumentCommitStatus::Committed) << result.message;
    EXPECT_EQ(executeCallbacks, 1);
    EXPECT_EQ(clearCallbacks, 1);
    EXPECT_EQ(removed, 1);
    EXPECT_EQ(appended, 0);
    EXPECT_EQ(_sheet->getPropertyByName("A1"), nullptr);
    EXPECT_EQ(_sheet->getPropertyByName("Borrowed"), nullptr);
    EXPECT_EQ(_sheet->getPropertyByName("CustomCache"), custom);
    EXPECT_FALSE(custom->testStatus(App::Property::Hidden));
    EXPECT_EQ(_sheet->getPropertyByName("B1"), sibling);
    EXPECT_FALSE(sibling->testStatus(App::Property::LockDynamic));
    EXPECT_EQ(static_cast<App::PropertyInteger*>(sibling)->getValue(), 9);
    EXPECT_FALSE(_document->mustExecute());
    expectSingleSpreadsheetPublication();
}

}  // namespace

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       linkedProducerDerivedReuseRejectsUnadoptedPersistentCell)
{
    expectUnadoptedDerivedReuseDenied(0, 17);
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       linkedProducerDerivedReuseRejectsUnadoptedNoPersistCell)
{
    expectUnadoptedDerivedReuseDenied(App::Prop_NoPersist, 101);
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       linkedProducerDerivedReuseRefreshesAdoptedPersistentCell)
{
    expectAdoptedDerivedReuseRefreshes(true);
}

TEST_F(SpreadsheetCollaborationCompatibilityTest,
       linkedProducerDerivedReuseRefreshesNativeCell)
{
    expectAdoptedDerivedReuseRefreshes(false);
}
