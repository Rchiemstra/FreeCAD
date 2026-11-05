// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <QApplication>
#include <QColor>
#include <QVariantMap>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/PropertyLinks.h>
#include <Gui/Application.h>
#include <Gui/MainWindow.h>
#include <Gui/propertyeditor/PropertyItem.h>
#include <src/App/InitApplication.h>

TEST(ExpressionDiagnosticsPresentation, errorColorOverridesLinkRestoreColor)
{
    if (!QApplication::instance()) {
        static int argc = 1;
        static char name[] = "Gui_tests_run";
        static char* argv[] = {name, nullptr};
        new QApplication(argc, argv);
    }
    tests::initApplication();
    if (!Gui::Application::Instance) {
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        new Gui::Application(true);
    }
    if (!Gui::getMainWindow()) {
        new Gui::MainWindow;
    }
    App::DocumentInitFlags flags;
    flags.createView = false;
    auto& app = App::GetApplication();
    auto* doc = app.newDocument("DiagnosticRestoreColor", "testUser", flags);
    auto* object = doc->addObject("App::FeaturePython", "Values");
    auto* link = dynamic_cast<App::PropertyXLink*>(
        object->addDynamicProperty("App::PropertyXLink", "Target"));
    ASSERT_NE(link, nullptr);
    link->setValue(std::string(), std::string("MissingTarget"), {});
    ASSERT_GT(link->checkRestore(), 1);
    auto* window = Gui::getMainWindow();
    const auto previousReasons = window->property("expressionDiagnosticsReasons");
    const auto previousColor = window->property("expressionDiagnosticsErrorColor");
    const QColor diagnosticColor(192, 32, 48);
    const QString key = QString::fromStdString(object->getFullName() + ".Target");
    window->setProperty("expressionDiagnosticsReasons", QVariantMap {{key, "Missing expression input"}});
    window->setProperty("expressionDiagnosticsErrorColor", diagnosticColor);
    {
        std::unique_ptr<Gui::PropertyEditor::PropertyItem> row(
            static_cast<Gui::PropertyEditor::PropertyItem*>(
                Gui::PropertyEditor::PropertyLinkItem::create()));
        row->setPropertyName(QStringLiteral("Target"));
        row->setPropertyData({link});
        EXPECT_EQ(row->data(1, Qt::ForegroundRole).value<QColor>(), diagnosticColor);
        EXPECT_TRUE(row->data(1, Qt::ToolTipRole).toString().contains("Missing expression input"));
    }
    window->setProperty("expressionDiagnosticsReasons", previousReasons);
    window->setProperty("expressionDiagnosticsErrorColor", previousColor);
    app.closeDocument(doc->getName());
}
