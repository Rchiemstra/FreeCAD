// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#define FC_OS_MACOSX 1
#include "App/ProgramOptionsUtilities.h"

#include <src/App/InitApplication.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <Base/PyObjectBase.h>
#include <stdexcept>
#include <Base/Exception.h>
#include <Base/Interpreter.h>

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <algorithm>
#include <initializer_list>
#include <list>
#include <string>
#include <vector>


using namespace App::Util;

using Spr = std::pair<std::string, std::string>;


class ApplicationTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
};

TEST_F(ApplicationTest, fCustomSyntaxLookup)
{
    Spr res {customSyntax("-display")};
    Spr exp {"display", "null"};
    EXPECT_EQ(res, exp);
};
TEST_F(ApplicationTest, fCustomSyntaxMac)
{
    Spr res {customSyntax("-psn_stuff")};
    Spr exp {"psn", "stuff"};
    EXPECT_EQ(res, exp);
};
TEST_F(ApplicationTest, fCustomSyntaxWidgetCount)
{
    Spr res {customSyntax("-widgetcount")};
    Spr exp {"widgetcount", ""};
    EXPECT_EQ(res, exp);
}
TEST_F(ApplicationTest, fCustomSyntaxNotFound)
{
    Spr res {customSyntax("-displayx")};
    Spr exp {"", ""};
    EXPECT_EQ(res, exp);
};
TEST_F(ApplicationTest, fCustomSyntaxAmpersand)
{
    Spr res {customSyntax("@freddie")};
    Spr exp {"response-file", "freddie"};
    EXPECT_EQ(res, exp);
};
TEST_F(ApplicationTest, fCustomSyntaxEmptyIn)
{
    Spr res {customSyntax("")};
    Spr exp {"", ""};
    EXPECT_EQ(res, exp);
};

// Exercise public creation with faults in the actual constructor, in one process.
TEST_F(ApplicationTest, ConstructorFailureCleansOwnedResourcesAndAllowsRetry)
{
    auto& app = App::GetApplication();
    App::DocumentInitFlags flags;
    flags.createView = false;
    struct Probe {
        int target;
        int reached = 0;
        bool simulateConflict = false;
        bool pathStateCorrect = false;
        std::string conflict;
        std::string created;
        const App::DocumentCollaborationService* service = nullptr;
        std::shared_ptr<App::Internal::CollaborationServiceLifetimeGate> gate;
        PyObject* wrapper = nullptr;
    };
    const auto name = app.getUniqueDocumentName("constructionFailure");
    const auto reset = qScopeGuard([&] {
        App::Internal::setDocumentConstructionTestHook(nullptr, nullptr);
        App::Internal::setDocumentConstructionDirectoryOutcomeForTesting(0);
        if (app.getDocument(name.c_str())) app.closeDocument(name.c_str());
    });
    for (int scenario = 0; scenario != 6; ++scenario) {
        Probe probe {scenario == 5 ? 4 : scenario + 1};
        probe.simulateConflict = scenario == 5;
        const auto before = App::Internal::documentConstructionLifetimeCountForTesting();
        App::Internal::setDocumentConstructionTestHook(
            [](App::Document& doc, int stage, const char* path, void* context) {
                auto& p = *static_cast<Probe*>(context);
                if (stage == 1) {
                    p.service = &doc.collaborationService();
                    p.gate = App::Internal::documentConstructionGateForTesting(doc);
                }
                if (stage == 2) p.wrapper = doc.getPyObject();
                if (stage == 6 && p.simulateConflict && p.conflict.empty()) {
                    p.conflict = path;
                    App::Internal::setDocumentConstructionDirectoryOutcomeForTesting(1);
                }
                if ((stage == 4 || stage == 5) && path) {
                    p.created = path;
                    p.pathStateCorrect = stage == 4 ? doc.TransientDir.getStrValue().empty()
                                                    : doc.TransientDir.getStrValue() == path;
                }
                if (stage == p.target) {
                    p.reached = stage;
                    throw std::runtime_error("injected constructor failure");
                }
            }, &probe);
        EXPECT_THROW(app.newDocument(name.c_str(), "failure", flags), std::runtime_error);
        App::Internal::setDocumentConstructionTestHook(nullptr, nullptr);
        EXPECT_EQ(probe.reached, probe.target);
        EXPECT_EQ(app.getDocument(name.c_str()), nullptr);
        EXPECT_EQ(App::Internal::documentConstructionLifetimeCountForTesting(), before);
        ASSERT_NE(probe.service, nullptr);
        EXPECT_FALSE(App::Internal::documentConstructionRegisteredForTesting(probe.service));
        ASSERT_TRUE(probe.gate);
        {
            std::lock_guard guard(probe.gate->mutex);
            EXPECT_TRUE(probe.gate->sealed);
            EXPECT_EQ(probe.gate->document, nullptr);
        }
        if (probe.wrapper) {
            Base::PyGILStateLocker gil;
            EXPECT_FALSE(static_cast<Base::PyObjectBase*>(probe.wrapper)->isValid());
            Py_DECREF(probe.wrapper);
        }
        if (probe.target >= 4) {
            ASSERT_FALSE(probe.created.empty());
            EXPECT_TRUE(probe.pathStateCorrect);
            EXPECT_FALSE(QFile::exists(QString::fromStdString(probe.created)));
        }
        if (probe.simulateConflict) {
            ASSERT_FALSE(probe.conflict.empty());
            EXPECT_NE(probe.conflict, probe.created);
        }
        auto* next = app.newDocument(name.c_str(), "retry", flags);
        ASSERT_NE(next, nullptr);
        EXPECT_EQ(app.getDocument(name.c_str()), next);
        const auto owned = QString::fromStdString(next->TransientDir.getStrValue());
        ASSERT_FALSE(owned.isEmpty());
        EXPECT_TRUE(QFile::exists(owned));
        app.closeDocument(name.c_str());
        EXPECT_FALSE(QFile::exists(owned));
        EXPECT_EQ(app.getDocument(name.c_str()), nullptr);
        EXPECT_EQ(App::Internal::documentConstructionLifetimeCountForTesting(), before);
    }

    // A lost atomic claim must retry even when construction ultimately succeeds.
    Probe conflict {0};
    conflict.simulateConflict = true;
    App::Internal::setDocumentConstructionTestHook(
        [](App::Document&, int stage, const char* path, void* context) {
            auto& p = *static_cast<Probe*>(context);
            if (stage == 6 && p.conflict.empty()) {
                p.conflict = path;
                App::Internal::setDocumentConstructionDirectoryOutcomeForTesting(1);
            }
        }, &conflict);
    auto* doc = app.newDocument(name.c_str(), "conflict retry", flags);
    App::Internal::setDocumentConstructionTestHook(nullptr, nullptr);
    ASSERT_NE(doc, nullptr);
    const auto owned = QString::fromStdString(doc->TransientDir.getStrValue());
    ASSERT_FALSE(conflict.conflict.empty());
    ASSERT_FALSE(owned.isEmpty());
    EXPECT_NE(owned, QString::fromStdString(conflict.conflict));
    EXPECT_TRUE(QFile::exists(owned));
    app.closeDocument(name.c_str());
    EXPECT_FALSE(QFile::exists(owned));

    // An ordinary mkdir error must not claim the path or retry as a collision.
    Probe error {0};
    App::Internal::setDocumentConstructionTestHook(
        [](App::Document&, int stage, const char* path, void* context) {
            auto& p = *static_cast<Probe*>(context);
            if (stage == 6) {
                ++p.reached;
                p.conflict = path;
                App::Internal::setDocumentConstructionDirectoryOutcomeForTesting(2);
            }
        }, &error);
    doc = app.newDocument(name.c_str(), "mkdir error", flags);
    App::Internal::setDocumentConstructionTestHook(nullptr, nullptr);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(error.reached, 1);
    EXPECT_TRUE(doc->TransientDir.getStrValue().empty());
    app.closeDocument(name.c_str());
    doc = app.newDocument(name.c_str(), "healthy", flags);
    ASSERT_NE(doc, nullptr);
    const auto healthy = QString::fromStdString(doc->TransientDir.getStrValue());
    ASSERT_FALSE(healthy.isEmpty());
    EXPECT_TRUE(QFile::exists(healthy));
    app.closeDocument(name.c_str());
    EXPECT_FALSE(QFile::exists(healthy));
}

TEST_F(ApplicationTest, newDocumentObserversSeeInitializedLabel)
{
    auto& app = App::GetApplication();
    const auto name = app.getUniqueDocumentName("initializedDocumentLabel");
    std::string observedLabel;
    bool observed = false;
    auto connection = app.signalNewDocument.connect([&](const App::Document& document, bool) {
        if (name == document.getName()) {
            observed = true;
            observedLabel = document.Label.getStrValue();
        }
    });
    const auto cleanup = qScopeGuard([&] {
        connection.disconnect();
        if (app.getDocument(name.c_str())) {
            app.closeDocument(name.c_str());
        }
    });
    App::DocumentInitFlags flags;
    flags.createView = false;
    auto* document = app.newDocument(name.c_str(), "Ready for observers", flags);
    ASSERT_NE(document, nullptr);
    EXPECT_TRUE(observed);
    EXPECT_EQ(observedLabel, "Ready for observers");
    EXPECT_EQ(document->Label.getStrValue(), observedLabel);
    EXPECT_FALSE(document->hasPendingFileChanges());
}

TEST_F(ApplicationTest, newDocumentObserverLabelEditRemainsAuthoritative)
{
    auto& app = App::GetApplication();
    const auto name = app.getUniqueDocumentName("observerDocumentLabel");
    auto connection = app.signalNewDocument.connect([&](const App::Document& document, bool) {
        if (name == document.getName()) {
            const_cast<App::Document&>(document).Label.setValue("Observer label");
        }
    });
    const auto cleanup = qScopeGuard([&] {
        connection.disconnect();
        if (app.getDocument(name.c_str())) {
            app.closeDocument(name.c_str());
        }
    });
    App::DocumentInitFlags flags;
    flags.createView = false;
    auto* document = app.newDocument(name.c_str(), "Bootstrap label", flags);
    ASSERT_NE(document, nullptr);
    EXPECT_EQ(document->Label.getStrValue(), "Observer label");
    EXPECT_TRUE(document->hasPendingFileChanges());
}

namespace
{

int countNamedModule(const std::vector<std::string>& modules, const char* name)
{
    return static_cast<int>(std::count(modules.begin(), modules.end(), std::string(name)));
}

}  // namespace

TEST_F(ApplicationTest, importAndExportTypeListASinglePatternModuleOnce)
{
    App::GetApplication().addImportType("B8 single (*.b8singleonly)", "B8SingleModule");
    App::GetApplication().addExportType("B8 single (*.b8singleonly)", "B8SingleModule");

    EXPECT_EQ(
        countNamedModule(App::GetApplication().getImportModules("b8singleonly"), "B8SingleModule"),
        1);
    EXPECT_EQ(
        countNamedModule(App::GetApplication().getExportModules("b8singleonly"), "B8SingleModule"),
        1);

    App::GetApplication().addImportType("B8 a (*.b8twomod)", "B8ModA");
    App::GetApplication().addImportType("B8 b (*.b8twomod)", "B8ModB");
    const auto both = App::GetApplication().getImportModules("b8twomod");
    EXPECT_EQ(countNamedModule(both, "B8ModA"), 1);
    EXPECT_EQ(countNamedModule(both, "B8ModB"), 1);
}

TEST_F(ApplicationTest, importAndExportTypeListAModuleOnceWhenPatternsDifferOnlyByCase)
{
    App::GetApplication().addImportType(
        "B8 once (*.b8caseonce *.B8CASEONCE)", "B8OnceModule");
    App::GetApplication().addExportType(
        "B8 once (*.b8caseonce *.B8CASEONCE)", "B8OnceModule");

    EXPECT_EQ(
        countNamedModule(App::GetApplication().getImportModules("b8caseonce"), "B8OnceModule"),
        1);
    EXPECT_EQ(
        countNamedModule(App::GetApplication().getImportModules("B8CASEONCE"), "B8OnceModule"),
        1);
    EXPECT_EQ(
        countNamedModule(App::GetApplication().getExportModules("b8caseonce"), "B8OnceModule"),
        1);
    EXPECT_EQ(
        countNamedModule(App::GetApplication().getExportModules("B8CASEONCE"), "B8OnceModule"),
        1);
}

namespace
{

class ScopedScriptImports
{
public:
    explicit ScopedScriptImports(std::initializer_list<const char*> names)
        : previousPath(PySequence_List(PySys_GetObject("path")), true)
        , moduleNames(names)
    {}

    ~ScopedScriptImports()
    {
        EXPECT_EQ(PySys_SetObject("path", previousPath.ptr()), 0);
        for (const char* name : moduleNames) {
            if (PyDict_DelItemString(PyImport_GetModuleDict(), name) < 0) {
                PyErr_Clear();  // Failed imports are already removed by Python.
            }
        }
    }

private:
    Base::PyGILStateLocker lock;
    Py::Object previousPath;
    std::vector<const char*> moduleNames;
};

QString pythonStringLiteral(QString value)
{
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral("'"), QStringLiteral("\\'"));
    value.replace(QStringLiteral("\r"), QStringLiteral("\\r"));
    value.replace(QStringLiteral("\n"), QStringLiteral("\\n"));
    return QStringLiteral("'") + value + QStringLiteral("'");
}

std::string writeTempScript(const QTemporaryDir& dir, const QString& name, const QByteArray& content)
{
    QFile file(dir.filePath(name));
    EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    EXPECT_EQ(file.write(content), content.size());
    file.close();
    return QDir::fromNativeSeparators(file.fileName()).toStdString();
}

std::string readTempFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll().toStdString();
}

}  // namespace

TEST_F(ApplicationTest, processFilesConsoleModeThrowsAndStopsOnScriptFailure)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const ScopedScriptImports imports {"console_first_fail", "console_second_ok"};

    auto& cfg = App::Application::Config();
    const auto prevIt = cfg.find("RunMode");
    const bool hadRunMode = prevIt != cfg.end();
    const std::string prevRunMode = hadRunMode ? prevIt->second : std::string {};
    const auto cleanup = qScopeGuard([&] {
        if (hadRunMode) {
            cfg["RunMode"] = prevRunMode;
        }
        else {
            cfg.erase("RunMode");
        }
    });
    cfg["RunMode"] = "Exit";

    const QString logPath = pythonStringLiteral(
        QDir::fromNativeSeparators(tempDir.filePath(QStringLiteral("order's.txt"))));
    const std::string firstFail = writeTempScript(
        tempDir,
        QStringLiteral("console_first_fail.py"),
        QStringLiteral(
            "with open(%1, 'a', encoding='utf-8') as f:\n"
            "    f.write('first\\n')\n"
            "raise ValueError('console-boom')\n")
            .arg(logPath)
            .toUtf8());
    const std::string secondOk = writeTempScript(
        tempDir,
        QStringLiteral("console_second_ok.py"),
        QStringLiteral(
            "with open(%1, 'a', encoding='utf-8') as f:\n"
            "    f.write('second\\n')\n")
            .arg(logPath)
            .toUtf8());

    EXPECT_THROW(App::Application::processFiles({firstFail, secondOk}), Base::Exception);
    EXPECT_EQ(readTempFile(tempDir.filePath(QStringLiteral("order's.txt"))), "first\n");
}

TEST_F(ApplicationTest, processFilesGuiModeSwallowsExceptionAndContinuesWithoutRetryingScript)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const ScopedScriptImports imports {"gui_first_fail", "gui_second_ok"};

    auto& cfg = App::Application::Config();
    const auto prevIt = cfg.find("RunMode");
    const bool hadRunMode = prevIt != cfg.end();
    const std::string prevRunMode = hadRunMode ? prevIt->second : std::string {};
    const auto cleanup = qScopeGuard([&] {
        if (hadRunMode) {
            cfg["RunMode"] = prevRunMode;
        }
        else {
            cfg.erase("RunMode");
        }
    });
    cfg["RunMode"] = "Gui";

    const QString logPath = pythonStringLiteral(
        QDir::fromNativeSeparators(tempDir.filePath(QStringLiteral("order's.txt"))));
    const std::string firstFail = writeTempScript(
        tempDir,
        QStringLiteral("gui_first_fail.py"),
        QStringLiteral(
            "with open(%1, 'a', encoding='utf-8') as f:\n"
            "    f.write('first\\n')\n"
            "raise ValueError('gui-boom')\n")
            .arg(logPath)
            .toUtf8());
    const std::string secondOk = writeTempScript(
        tempDir,
        QStringLiteral("gui_second_ok.py"),
        QStringLiteral(
            "with open(%1, 'a', encoding='utf-8') as f:\n"
            "    f.write('second\\n')\n")
            .arg(logPath)
            .toUtf8());

    std::list<std::string> processed;
    EXPECT_NO_THROW(processed = App::Application::processFiles({firstFail, secondOk}));
    EXPECT_EQ(processed, std::list<std::string> {secondOk});
    EXPECT_EQ(readTempFile(tempDir.filePath(QStringLiteral("order's.txt"))), "first\nsecond\n");
}
