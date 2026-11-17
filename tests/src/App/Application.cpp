// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#define FC_OS_MACOSX 1
#include "App/ProgramOptionsUtilities.h"

#include <src/App/InitApplication.h>

#include <App/Application.h>
#include <App/Document.h>
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
