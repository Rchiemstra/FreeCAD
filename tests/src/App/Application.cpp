// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#define FC_OS_MACOSX 1
#include "App/ProgramOptionsUtilities.h"

#include <src/App/InitApplication.h>

#include <App/Application.h>
#include <App/Document.h>

#include <QScopeGuard>

#include <algorithm>
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
