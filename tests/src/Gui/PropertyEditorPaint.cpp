// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QPixmap>
#include <QScopeGuard>
#include <QThread>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentObject.h>
#include <App/PropertyLinks.h>
#include <Gui/propertyeditor/PropertyEditor.h>
#include <src/App/InitApplication.h>

#include <chrono>
#include <functional>
#include <future>
#include <utility>

#include "CollaborationGuiTestHelpers.h"

namespace
{
// Let the real setters update the primary list, then pause before they update
// the shadow list. This reproduces the invariant violation without changing
// production code or relying on the scheduler to hit a short mutation window.
template<typename LinkProperty>
class PausedShadowUpdate: public LinkProperty
{
public:
    std::function<void()> beforeShadowUpdate;

    void updateElementReference(App::DocumentObject* feature, bool reverse, bool notify) override
    {
        if (auto pause = std::exchange(beforeShadowUpdate, {})) {
            pause();
        }
        LinkProperty::updateElementReference(feature, reverse, notify);
    }
};

class PaintLinkSub: public PausedShadowUpdate<App::PropertyLinkSub>
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();
};
class PaintLinkSubList: public PausedShadowUpdate<App::PropertyLinkSubList>
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();
};
TYPESYSTEM_SOURCE_P(PaintLinkSub)
TYPESYSTEM_SOURCE_P(PaintLinkSubList)

void PaintLinkSub::init()
{
    initSubclass(classTypeId, "App::PaintLinkSub", "App::PropertyLinkSub", &create);
}

void PaintLinkSubList::init()
{
    initSubclass(classTypeId, "App::PaintLinkSubList", "App::PropertyLinkSubList", &create);
}

class PropertyEditorPaintTest: public ::testing::TestWithParam<const char*>
{
protected:
    static void SetUpTestSuite()
    {
        if (!QApplication::instance()) {
            static int argc = 1;
            static char name[] = "Gui_tests_run";
            static char* argv[] = {name, nullptr};
            new QApplication(argc, argv);
        }
        tests::initApplication();
        if (PaintLinkSub::getClassTypeId().isBad()) {
            PaintLinkSub::init();
            Gui::PropertyEditor::PropertyLinkItem::init();
            Gui::PropertyEditor::PropertyLinkListItem::init();
        }
        if (PaintLinkSubList::getClassTypeId().isBad()) {
            PaintLinkSubList::init();
        }
    }

    void SetUp() override
    {
        App::DocumentInitFlags flags;
        flags.createView = false;
        name = App::GetApplication().getUniqueDocumentName("editor_paint");
        doc = App::GetApplication().newDocument(name.c_str(), "testUser", flags);
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(name.c_str());
    }

    App::Document* doc = nullptr;
    std::string name;
};
}

TEST_P(PropertyEditorPaintTest, defersHalfUpdatedLinkUntilOwnerFinishes) try
{
    auto* object = doc->addObject("App::DocumentObject", "Owner");
    auto* target = doc->addObject("App::DocumentObject", "Target");
    auto* property = object->addDynamicProperty(GetParam(), "References", "Test");
    auto* list = dynamic_cast<PaintLinkSubList*>(property);
    auto* sub = dynamic_cast<PaintLinkSub*>(property);
    ASSERT_TRUE(list || sub);
    if (list) {
        list->setValue(target, "Edge1");
    }
    else {
        sub->setValue(target, {"Edge1"});
    }

    Gui::PropertyEditor::PropertyEditor editor;
    editor.buildUp({{"References", {property}}});
    editor.resize(400, 200);
    editor.expandAll();
    editor.show();
    const auto group = editor.model()->index(0, 0);
    const auto index = editor.model()->index(0, 1, group);
    ASSERT_TRUE(index.isValid());
    ASSERT_FALSE(index.data().toString().isEmpty());

    std::promise<bool> entered;
    std::promise<void> release;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future();
    auto work = doc->executionLane()->postToOwner([&] {
        App::CollaborationCompatibilityMutation mutation;
        mutation.scope = App::CollaborationCompatibilityScope::Structural;
        return doc->collaborationService().commitCompatibilityMutationWithPolicy(
            mutation,
            [&] {
                auto& pause = list ? list->beforeShadowUpdate : sub->beforeShadowUpdate;
                pause = [&] {
                    // Inspect on the owner thread, before publishing the checkpoint.
                    // No GUI-side live read is needed to prove the broken invariant.
                    const auto primarySize = list ? list->getSubValues().size()
                                                  : sub->getSubValues().size();
                    const auto shadowSize = list ? list->getShadowSubs().size()
                                                 : sub->getShadowSubs().size();
                    entered.set_value(primarySize == 2 && shadowSize == 1);
                    released.wait_for(std::chrono::seconds(10));
                };
                if (list) {
                    list->setValue(target, std::vector<std::string> {"Edge1", "Edge2"});
                }
                else {
                    sub->setValue(target, {"Edge1", "Edge2"});
                }
            },
            App::CollaborationCompatibilityRecomputePolicy::Deferred);
    });
    bool ownerReleased = false;
    const auto finish = qScopeGuard([&] {
        if (!ownerReleased) {
            release.set_value();
        }
        if (work.valid()) {
            Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] { work.wait(); });
        }
    });
    QElapsedTimer elapsed;
    elapsed.start();
    while (enteredFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready
           && elapsed.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    ASSERT_EQ(enteredFuture.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    ASSERT_TRUE(enteredFuture.get()) << "setter did not pause between primary and shadow updates";

    elapsed.restart();
    // This used to abort in getSubValues, deterministically, from drawRow.
    QPixmap pixels(editor.size());
    editor.render(&pixels);
    for (int role : {Qt::DisplayRole, Qt::DecorationRole, Qt::ToolTipRole,
                     Qt::ForegroundRole, Qt::EditRole}) {
        EXPECT_FALSE(index.data(role).isValid());
    }
    EXPECT_LT(elapsed.elapsed(), 500) << "painting waited for the document owner";

    // The standalone editor is not connected to document notifications. The
    // model must retry its reads itself once the owner releases admission.
    int retries = 0;
    QObject::connect(editor.model(), &QAbstractItemModel::dataChanged, &editor,
                     [&](const QModelIndex& first, const QModelIndex&) {
                         if (first == index) {
                             ++retries;
                         }
                     });
    release.set_value();
    ownerReleased = true;
    const auto result = Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] { return work.get(); });
    ASSERT_TRUE(result.committed()) << result.message;
    elapsed.restart();
    while (retries == 0 && elapsed.elapsed() < 2000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    EXPECT_GT(retries, 0);
    EXPECT_TRUE(index.data().toString().contains(QStringLiteral("Edge2")));
}

catch (const Base::Exception& error)
{
    FAIL() << error.what();
}

INSTANTIATE_TEST_SUITE_P(LinkProperties, PropertyEditorPaintTest,
                        ::testing::Values("App::PaintLinkSub", "App::PaintLinkSubList"));

