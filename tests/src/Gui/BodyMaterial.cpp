// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <stdexcept>

#include <QApplication>

#include <App/Application.h>
#include <App/Document.h>
#include <App/GeoFeature.h>
#include <Base/Interpreter.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/ViewProviderGeometryObject.h>
#include "CollaborationGuiTestHelpers.h"
#include <src/App/InitApplication.h>

namespace
{

class BodyMaterialTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        static int argc = 1;
        static char executable[] = "Gui_tests_run";
        static char* argv[] = {executable, nullptr};
        if (!QApplication::instance()) {
            new QApplication(argc, argv);
        }
        tests::initApplication();
        if (!Gui::Application::Instance) {
            Gui::Application::initApplication();
            Gui::Application::initOpenInventor();
            new Gui::Application(true);
        }
        Base::Interpreter().runString("import Part; import PartDesign; import PartDesignGui");
    }

    void SetUp() override
    {
        App::DocumentInitFlags flags;
        flags.createView = false;
        _documentName = App::GetApplication().getUniqueDocumentName("bodyMaterial");
        _document = App::GetApplication().newDocument(_documentName.c_str(), "Body material", flags);
        ASSERT_NE(_document, nullptr);
        Base::Interpreter().runString(
            "doc = App.ActiveDocument\n"
            "body = doc.addObject('PartDesign::Body', 'ColorBody')\n"
            "feature = body.newObject('PartDesign::Feature', 'ColorFeature')\n"
            "feature.Shape = Part.makeBox(10, 10, 10)\n");
        Gui::Test::recomputeWithoutBlockingGui(*_document);
        _guiDocument = Gui::Application::Instance->getDocument(_document);
        ASSERT_NE(_guiDocument, nullptr);
        _body = freecad_cast<App::GeoFeature*>(_document->getObject("ColorBody"));
        ASSERT_NE(_body, nullptr);
        _bodyView = freecad_cast<Gui::ViewProviderGeometryObject*>(
            _guiDocument->getViewProvider(_body));
        _featureView = freecad_cast<Gui::ViewProviderGeometryObject*>(
            _guiDocument->getViewProvider(_document->getObject("ColorFeature")));
        ASSERT_NE(_bodyView, nullptr);
        ASSERT_NE(_featureView, nullptr);
        // Use an explicit material rather than the preference-dependent default.
        // In a fresh profile that default matches the empty-material sentinel,
        // which ViewProviderGeometryObject intentionally does not apply.
        auto material = _body->getMaterialAppearance();
        material.diffuseColor = Base::Color(0.4F, 0.5F, 0.6F);
        _body->setMaterialAppearance(material);
        Gui::Test::recomputeWithoutBlockingGui(*_document);
        // Finish setup catch-up before giving the feature its caller-set color.
        _guiDocument->catchUpIdleLivePresentation();
        QApplication::processEvents();
        ASSERT_EQ(_body->getMaterialAppearance().diffuseColor, material.diffuseColor);
        ASSERT_EQ(_bodyView->ShapeAppearance[0].diffuseColor, material.diffuseColor);
        Base::Interpreter().runString("feature.ViewObject.ShapeColor = (1.0, 0.0, 0.0)");
        ASSERT_EQ(_featureView->ShapeAppearance[0].diffuseColor, red);
    }

    void TearDown() override
    {
        if (_document && App::GetApplication().getDocument(_documentName.c_str())) {
            App::GetApplication().closeDocument(_documentName.c_str());
            QApplication::processEvents();
        }
    }

    App::DocumentCommitResult commit(std::function<void()> callback)
    {
        return Gui::Test::commitCompatibilityMutationWithoutBlockingGui(
            *_document,
            {App::CollaborationCompatibilityScope::Structural, {}, {}},
            std::move(callback));
    }

    const Base::Color red {1.0F, 0.0F, 0.0F};
    App::Document* _document {nullptr};
    Gui::Document* _guiDocument {nullptr};
    App::GeoFeature* _body {nullptr};
    Gui::ViewProviderGeometryObject* _bodyView {nullptr};
    Gui::ViewProviderGeometryObject* _featureView {nullptr};
    std::string _documentName;
};

}  // namespace

TEST_F(BodyMaterialTest, callerColorSurvivesHideShowCatchUp)
{
    ASSERT_TRUE(_bodyView->Visibility.getValue());
    ASSERT_TRUE(_bodyView->isShow());
    for (const bool visible : {false, true}) {
        const auto result = commit([&] { _body->Visibility.setValue(visible); });
        ASSERT_TRUE(result.committed()) << result.message;
        // Exercise Document's real deferred-visibility/full-property refresh.
        // The event pump may already have delivered the scheduled catch-up.
        _guiDocument->catchUpIdleLivePresentation();
        EXPECT_EQ(_bodyView->Visibility.getValue(), visible);
        EXPECT_EQ(_bodyView->isShow(), visible);
        EXPECT_EQ(_featureView->ShapeAppearance[0].diffuseColor, red);
    }
}

TEST_F(BodyMaterialTest, equalMaterialAssignmentReappliesFeatureColor)
{
    const App::Material material = _body->getMaterialAppearance();
    ASSERT_NE(material.diffuseColor, red);
    // The material dialog writes unconditionally, even for an equal value.
    _body->setMaterialAppearance(material);
    EXPECT_EQ(_featureView->ShapeAppearance[0].diffuseColor, material.diffuseColor);
}

TEST_F(BodyMaterialTest, deferredEqualMaterialAssignmentReappliesFeatureColor)
{
    const App::Material material = _body->getMaterialAppearance();
    ASSERT_NE(material.diffuseColor, red);
    const auto result = commit([&] { _body->setMaterialAppearance(material); });
    ASSERT_TRUE(result.committed()) << result.message;
    _guiDocument->catchUpIdleLivePresentation();
    EXPECT_EQ(_featureView->ShapeAppearance[0].diffuseColor, material.diffuseColor);
}

TEST_F(BodyMaterialTest, changedMaterialAssignmentUpdatesFeatureColor)
{
    App::Material material = _body->getMaterialAppearance();
    material.diffuseColor = Base::Color(0.2F, 0.7F, 0.3F);
    _body->setMaterialAppearance(material);
    EXPECT_EQ(_bodyView->ShapeAppearance[0].diffuseColor, material.diffuseColor);
    EXPECT_EQ(_featureView->ShapeAppearance[0].diffuseColor, material.diffuseColor);
}

TEST_F(BodyMaterialTest, observerEqualMaterialAssignmentDuringCatchUpReappliesFeatureColor)
{
    const App::Material material = _body->getMaterialAppearance();
    bool assigned = false;
    fastsignals::scoped_connection observer =
        Gui::Application::Instance->signalChangedObject.connect(
            [&](const Gui::ViewProvider& provider, const App::Property& property) {
                if (&provider == _bodyView && &property == &_bodyView->Visibility
                    && _bodyView->Visibility.getValue() && !assigned
                    && !_guiDocument->deferLivePresentationUpdates()) {
                    assigned = true;
                    _body->setMaterialAppearance(material);
                }
            });
    for (const bool visible : {false, true}) {
        const auto result = commit([&] { _body->Visibility.setValue(visible); });
        ASSERT_TRUE(result.committed()) << result.message;
        _guiDocument->catchUpIdleLivePresentation();
    }
    ASSERT_TRUE(assigned);
    EXPECT_EQ(_featureView->ShapeAppearance[0].diffuseColor, material.diffuseColor);
}

TEST_F(BodyMaterialTest, throwingVisibilityCallbackRetainsPendingMaterialAssignment)
{
    const App::Material material = _body->getMaterialAppearance();
    int interruptions = 0;
    fastsignals::scoped_connection observer =
        Gui::Application::Instance->signalChangedObject.connect(
            [&](const Gui::ViewProvider& provider, const App::Property& property) {
                if (&provider == _bodyView && &property == &_bodyView->Visibility
                    && !_guiDocument->deferLivePresentationUpdates()) {
                    ++interruptions;
                    throw std::runtime_error("Interrupted Body visibility catch-up");
                }
            });
    const auto result = commit([&] {
        _body->Visibility.setValue(false);
        _body->setMaterialAppearance(material);
    });
    ASSERT_TRUE(result.committed()) << result.message;
    _guiDocument->catchUpIdleLivePresentation();
    ASSERT_GT(interruptions, 0);
    observer.disconnect();
    // A later refresh must deliver the assignment skipped by the exception.
    _guiDocument->catchUpIdleLivePresentation();
    EXPECT_EQ(_featureView->ShapeAppearance[0].diffuseColor, material.diffuseColor);
}

TEST_F(BodyMaterialTest, equalSyntheticMaterialReplayDoesNotCopyAppearance)
{
    int copies = 0;
    fastsignals::scoped_connection observer =
        Gui::Application::Instance->signalBeforeChangeObject.connect(
            [&](const Gui::ViewProvider& provider, const App::Property& property) {
                if (&provider == _bodyView && &property == &_bodyView->ShapeAppearance) {
                    ++copies;
                }
            });
    _guiDocument->catchUpIdleLivePresentation();
    EXPECT_EQ(copies, 0);
    EXPECT_EQ(_featureView->ShapeAppearance[0].diffuseColor, red);
}

TEST_F(BodyMaterialTest, nestedBeforeChangeViewAssignmentKeepsSyntheticProvenance)
{
    const App::Material material = _body->getMaterialAppearance();
    ASSERT_NE(material.diffuseColor, red);
    bool assigned = false;
    bool nestedAssignmentCopied = false;
    fastsignals::scoped_connection observer =
        Gui::Application::Instance->signalBeforeChangeObject.connect(
            [&](const Gui::ViewProvider& provider, const App::Property& property) {
                if (&provider == _bodyView && &property == &_bodyView->ShapeAppearance
                    && !assigned
                    && !_guiDocument->deferLivePresentationUpdates()) {
                    assigned = true;
                    _bodyView->ShapeAppearance.setValue(material);
                    nestedAssignmentCopied =
                        _featureView->ShapeAppearance[0].diffuseColor == material.diffuseColor;
                    // A later caller override must survive the outer replay.
                    auto appearance = _featureView->ShapeAppearance[0];
                    appearance.diffuseColor = red;
                    _featureView->ShapeAppearance.setValue(appearance);
                }
            });
    for (const bool visible : {false, true}) {
        const auto result = commit([&] { _body->Visibility.setValue(visible); });
        ASSERT_TRUE(result.committed()) << result.message;
        _guiDocument->catchUpIdleLivePresentation();
    }
    ASSERT_TRUE(assigned);
    EXPECT_TRUE(nestedAssignmentCopied);
    EXPECT_EQ(_featureView->ShapeAppearance[0].diffuseColor, red);
}
