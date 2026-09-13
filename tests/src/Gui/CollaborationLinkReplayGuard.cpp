// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>
#include <QScopeGuard>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentObject.h>
#include <App/FeatureTest.h>
#include <App/Link.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/ViewProviderDocumentObject.h>
#include <src/App/InitApplication.h>

namespace
{

void initializeLinkReplayGui()
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
}

bool linkTouchedProperty(App::Link* link)
{
    auto* ext = link->getExtensionByType<App::LinkBaseExtension>(true);
    return ext && ext->_LinkTouched.isTouched();
}

void ensureViewProviders(Gui::Document* guiDocument, const std::vector<App::DocumentObject*>& objects)
{
    for (auto* object : objects) {
        ASSERT_NE(object, nullptr);
        auto* viewProvider = guiDocument->getViewProvider(object);
        ASSERT_NE(viewProvider, nullptr) << object->getNameInDocument();
    }
    QApplication::processEvents();
}

class CollaborationLinkReplayGuardTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        initializeLinkReplayGui();
    }

    void SetUp() override
    {
        App::DocumentInitFlags flags;
        flags.createView = false;
        _documentName = App::GetApplication().getUniqueDocumentName("linkReplayGuard");
        _document = App::GetApplication().newDocument(
            _documentName.c_str(), "link replay guard", flags);
        ASSERT_NE(_document, nullptr);
        _target = _document->addObject<App::FeatureTest>("LinkedBody");
        ASSERT_NE(_target, nullptr);
        _linkLeft = freecad_cast<App::Link*>(_document->addObject("App::Link", "FrontLeft"));
        _linkRight = freecad_cast<App::Link*>(_document->addObject("App::Link", "FrontRight"));
        ASSERT_NE(_linkLeft, nullptr);
        ASSERT_NE(_linkRight, nullptr);
        _linkLeft->setLink(-1, _target);
        _linkRight->setLink(-1, _target);
        _document->recompute();
        _guiDocument = Gui::Application::Instance->getDocument(_document);
        ASSERT_NE(_guiDocument, nullptr);
        ensureViewProviders(
            _guiDocument,
            {_target, _linkLeft, _linkRight});
        ASSERT_FALSE(_document->mustExecute());
        ASSERT_FALSE(_linkLeft->isTouched());
        ASSERT_FALSE(_linkRight->isTouched());
    }

    void TearDown() override
    {
        if (_document && App::GetApplication().getDocument(_documentName.c_str())) {
            App::GetApplication().closeDocument(_documentName.c_str());
            QApplication::processEvents();
        }
    }

    App::Document* _document {nullptr};
    App::FeatureTest* _target {nullptr};
    App::Link* _linkLeft {nullptr};
    App::Link* _linkRight {nullptr};
    Gui::Document* _guiDocument {nullptr};
    std::string _documentName;
};

}  // namespace

TEST_F(CollaborationLinkReplayGuardTest,
       structuralCommitLeavesAssemblyLinksCleanAfterDeferredReplay)
{
    int recomputeCount = 0;
    fastsignals::scoped_connection recomputeConnection = _document->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>&) {
            ++recomputeCount;
        });

    const auto result = _document->collaborationService().commitCompatibilityMutation(
        {App::CollaborationCompatibilityScope::Structural, {}, {}},
        [&] {
            _target->Label.setValue("seat body updated");
            _document->addObject<App::FeatureTest>("SeatSketchSibling");
        });

    ASSERT_TRUE(result.committed()) << result.message;
    EXPECT_EQ(recomputeCount, 1) << "structural commit should perform exactly one eager recompute";
    EXPECT_FALSE(_linkLeft->isTouched());
    EXPECT_FALSE(_linkRight->isTouched());
    EXPECT_FALSE(linkTouchedProperty(_linkLeft));
    EXPECT_FALSE(linkTouchedProperty(_linkRight));
    EXPECT_FALSE(_document->mustExecute());
}

TEST_F(CollaborationLinkReplayGuardTest,
       interactiveLinkedObjectChangeStillDirtiesAssemblyLinks)
{
    _target->Integer.setValue(42);
    QApplication::processEvents();

    EXPECT_TRUE(_linkLeft->isTouched());
    EXPECT_TRUE(_linkRight->isTouched());
    EXPECT_TRUE(linkTouchedProperty(_linkLeft));
    EXPECT_TRUE(linkTouchedProperty(_linkRight));
}

TEST_F(CollaborationLinkReplayGuardTest,
       outputPropertyChangeDoesNotDirtyAssemblyLinks)
{
    _target->TypeOutput.setValue(42);
    QApplication::processEvents();

    EXPECT_FALSE(_linkLeft->isTouched());
    EXPECT_FALSE(_linkRight->isTouched());
    EXPECT_FALSE(linkTouchedProperty(_linkLeft));
    EXPECT_FALSE(linkTouchedProperty(_linkRight));
}
