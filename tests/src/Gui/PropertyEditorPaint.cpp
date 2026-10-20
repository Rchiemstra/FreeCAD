// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QPixmap>
#include <QLabel>
#include <QCheckBox>
#include <QAbstractItemDelegate>
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
            Gui::PropertyEditor::PropertyStringItem::init();
            Gui::PropertyEditor::PropertyBoolItem::init();
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

    using Probe = std::function<void(Gui::PropertyEditor::PropertyEditor&, const QModelIndex&)>;
    void withPausedOwner(const Probe& before, const Probe& during, const Probe& after);

    App::Document* doc = nullptr;
    std::string name;
};
}

void PropertyEditorPaintTest::withPausedOwner(
    const Probe& before, const Probe& during, const Probe& after) try
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

    before(editor, index);

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

    during(editor, index);

    release.set_value();
    ownerReleased = true;
    const auto result = Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] { return work.get(); });
    ASSERT_TRUE(result.committed()) << result.message;
    after(editor, index);
}

catch (const Base::Exception& error)
{
    FAIL() << error.what();
}

namespace
{
void pumpFor(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
}
}

TEST_P(PropertyEditorPaintTest, defersHalfUpdatedLinkUntilOwnerFinishes)
{
    int retries = 0;
    withPausedOwner([](auto&, auto) {}, [&](auto& editor, auto index) {
        QElapsedTimer elapsed;
        elapsed.start();
        QPixmap pixels(editor.size());
        editor.render(&pixels);
        for (int role : {Qt::DisplayRole, Qt::DecorationRole, Qt::ToolTipRole,
                         Qt::ForegroundRole, Qt::EditRole}) {
            (void)index.data(role);
        }
        EXPECT_LT(elapsed.elapsed(), 500);
        QObject::connect(editor.model(), &QAbstractItemModel::dataChanged, &editor,
                         [&, index](const QModelIndex& first, const QModelIndex&) {
                             if (first == index) {
                                 ++retries;
                             }
                         });
    }, [&](auto&, auto index) {
        pumpFor(1200);
        EXPECT_GT(retries, 0);
        EXPECT_TRUE(index.data().toString().contains(QStringLiteral("Edge2")));
    });
}

TEST_P(PropertyEditorPaintTest, defersEditorCreation)
{
    withPausedOwner([](auto&, auto) {}, [](auto& editor, auto index) {
        editor.openPersistentEditor(index);
        EXPECT_TRUE(editor.template findChildren<QPushButton*>().empty());
    }, [](auto& editor, auto) {
        pumpFor(1200);
        EXPECT_FALSE(editor.template findChildren<QPushButton*>().empty());
    });
}

TEST_P(PropertyEditorPaintTest, defersInitialEditorData)
{
    QWidget* widget = nullptr;
    withPausedOwner([&](auto& editor, auto index) {
        auto* item = static_cast<Gui::PropertyEditor::PropertyItem*>(index.internalPointer());
        widget = item->createEditor(editor.viewport(), [] {});
        ASSERT_NE(widget, nullptr);
    }, [&](auto& editor, auto index) {
        editor.itemDelegate()->setEditorData(widget, index);
        EXPECT_TRUE(widget->findChild<QLabel*>()->text().isEmpty());
    }, [&](auto&, auto) {
        pumpFor(1200);
        EXPECT_TRUE(widget->findChild<QLabel*>()->text().contains(QStringLiteral("Edge2")));
    });
}

TEST_P(PropertyEditorPaintTest, preservesOpenEditorAcrossReadRetries)
{
    QLabel* label = nullptr;
    withPausedOwner([&](auto& editor, auto index) {
        editor.openPersistentEditor(index);
        label = editor.template findChild<QLabel*>();
        ASSERT_NE(label, nullptr);
        label->setText(QStringLiteral("uncommitted editor contents"));
    }, [&](auto& editor, auto index) {
        editor.itemDelegate()->setEditorData(label->parentWidget(), index);
        EXPECT_EQ(label->text(), QStringLiteral("uncommitted editor contents"));
        (void)index.data();
        pumpFor(180);
        EXPECT_EQ(label->text(), QStringLiteral("uncommitted editor contents"));
    }, [&](auto&, auto) {
        pumpFor(1200);
        EXPECT_EQ(label->text(), QStringLiteral("uncommitted editor contents"));
    });
}

TEST_P(PropertyEditorPaintTest, keepsSafeValuesAndNamesWhileBusy)
{
    QVariant display, edit, name;
    withPausedOwner([&](auto&, auto index) {
        display = index.data();
        edit = index.data(Qt::EditRole);
        name = index.sibling(index.row(), 0).data();
    }, [&](auto&, auto index) {
        EXPECT_EQ(index.data(), display);
        EXPECT_EQ(index.data(Qt::EditRole), edit);
        EXPECT_EQ(index.sibling(index.row(), 0).data(), name);
        EXPECT_EQ(index.data(Qt::TextAlignmentRole), QVariant());
    }, [](auto&, auto) {});
}

TEST_P(PropertyEditorPaintTest, preservesBooleanEditorWhileBusy)
{
    QCheckBox* checkbox = nullptr;
    QModelIndex boolIndex;
    withPausedOwner([&](auto& editor, auto) {
        auto* owner = doc->getObject("Owner");
        auto* property = static_cast<App::PropertyBool*>(
            owner->addDynamicProperty("App::PropertyBool", "Enabled", "Test"));
        property->setValue(true);
        // Use a second view so the link index in the paused-owner harness stays valid.
        auto* boolEditor = new Gui::PropertyEditor::PropertyEditor(&editor);
        boolEditor->buildUp({{"Enabled", {property}}});
        boolIndex = boolEditor->model()->index(0, 1, boolEditor->model()->index(0, 0));
        EXPECT_TRUE(boolIndex.data(Qt::EditRole).toBool());
        boolEditor->openPersistentEditor(boolIndex);
        checkbox = boolEditor->template findChild<QCheckBox*>();
        ASSERT_NE(checkbox, nullptr);
        EXPECT_TRUE(checkbox->isChecked());
    }, [&](auto&, auto) {
        auto* boolEditor = qobject_cast<Gui::PropertyEditor::PropertyEditor*>(
            checkbox->parentWidget()->parentWidget());
        ASSERT_NE(boolEditor, nullptr);
        boolEditor->itemDelegate()->setEditorData(checkbox, boolIndex);
        EXPECT_TRUE(checkbox->isChecked());
        EXPECT_TRUE(boolIndex.data(Qt::EditRole).toBool());
    }, [&](auto&, auto) {
        pumpFor(1200);
        EXPECT_TRUE(checkbox->isChecked());
    });
}

TEST_P(PropertyEditorPaintTest, defersDirectLinkLabelRefresh)
{
    QWidget* widget = nullptr;
    Gui::PropertyEditor::PropertyItem* item = nullptr;
    withPausedOwner([&](auto& editor, auto index) {
        item = static_cast<Gui::PropertyEditor::PropertyItem*>(index.internalPointer());
        widget = item->createEditor(editor.viewport(), [] {});
        item->setEditorData(widget, index.data(Qt::EditRole));
    }, [&](auto&, auto) {
        auto* label = widget->findChild<QLabel*>();
        const auto text = label->text();
        item->setEditorData(widget, {});
        EXPECT_EQ(label->text(), text);
    }, [&](auto&, auto) {
        pumpFor(1200);
        EXPECT_TRUE(widget->findChild<QLabel*>()->text().contains(QStringLiteral("Edge2")));
    });
}

TEST_P(PropertyEditorPaintTest, defersDirectLinkValue)
{
    withPausedOwner([](auto&, auto) {}, [](auto&, auto index) {
        auto* item = static_cast<Gui::PropertyEditor::PropertyItem*>(index.internalPointer());
        EXPECT_FALSE(item->data(1, Qt::EditRole).isValid());
    }, [&](auto&, auto index) {
        // Also exercise non-command owner work, when the global presentation
        // reservation itself is available. Repeated owners
        // in a row and its ancestor must only require one document check.
        auto* item = static_cast<Gui::PropertyEditor::PropertyItem*>(index.internalPointer());
        auto properties = item->getPropertyData();
        properties.push_back(properties.front());
        item->setPropertyData(properties);
        item->parent()->setPropertyData({&doc->getObject("Target")->Label});
        EXPECT_TRUE(item->data(1, Qt::EditRole).isValid());
        std::promise<void> entered, release;
        auto enteredFuture = entered.get_future();
        auto released = release.get_future();
        auto work = doc->executionLane()->postToOwner([&] {
            auto* lane = doc->executionLane();
            lane->beginRecoverySnapshotOwnerWork();
            const auto endWork = qScopeGuard([&] { lane->endRecoverySnapshotOwnerWork(); });
            entered.set_value();
            released.wait_for(std::chrono::seconds(10));
        });
        const auto finish = qScopeGuard([&] {
            release.set_value();
            Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] { work.wait(); });
        });
        Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] { enteredFuture.wait(); });
        EXPECT_FALSE(item->data(1, Qt::EditRole).isValid());
        EXPECT_FALSE(index.sibling(index.row(), 0).data().toString().isEmpty());
    });
}

TEST_P(PropertyEditorPaintTest, retriesNotifyOnlyAfterOwnerFinishes)
{
    int notifications = 0;
    withPausedOwner([](auto&, auto) {}, [&](auto& editor, auto index) {
        QObject::connect(editor.model(), &QAbstractItemModel::dataChanged, &editor,
                         [&](const QModelIndex&, const QModelIndex&) { ++notifications; });
        (void)index.data();
        pumpFor(260);
        EXPECT_EQ(notifications, 0);
    }, [&](auto&, auto) {
        pumpFor(1200);
        EXPECT_GT(notifications, 0);
    });
}

INSTANTIATE_TEST_SUITE_P(LinkProperties, PropertyEditorPaintTest,
                        ::testing::Values("App::PaintLinkSub", "App::PaintLinkSubList"));

