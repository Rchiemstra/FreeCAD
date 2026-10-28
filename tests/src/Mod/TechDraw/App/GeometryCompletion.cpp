// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <Base/Interpreter.h>
#include <Mod/TechDraw/App/DrawViewPart.h>
#include <Mod/TechDraw/App/DrawComplexSection.h>
#include <Mod/TechDraw/App/GeometryObject.h>
#include <Mod/TechDraw/App/Preferences.h>
#include <src/App/InitApplication.h>

#include <BRepPrimAPI_MakeBox.hxx>
#include <QCoreApplication>
#include <QFutureInterface>
#include <QThread>

#include <chrono>

namespace {

// Force completion at the old race boundary: buildGeometryObject has finished,
// but makeGeometryForShape has not returned to partExec to publish its result.
// No scheduling sleeps or production-only test hook are needed.
class EarlyCompletionView: public TechDraw::DrawViewPart
{
public:
    using DrawViewPart::partExec;
    using DrawViewPart::watchGeometryFuture;
    bool completedBeforeReturn = false;
    bool installed() const { return geometryObject && !m_tempGeometryObject; }
    void pending() { m_tempGeometryObject = std::make_shared<TechDraw::GeometryObject>("View", this); }
    bool hasPending() const { return bool(m_tempGeometryObject); }

protected:
    TechDraw::GeometryObjectPtr makeGeometryForShape(const TopoDS_Shape& shape) override
    {
        auto result = DrawViewPart::makeGeometryForShape(shape);
        if (m_tempGeometryObject) {
            onHlrFinished();
        }
        completedBeforeReturn = installed();
        return result;
    }
    void postHlrTasks() override {}
};

class JoinedSection: public TechDraw::DrawComplexSection
{
public:
    int joins = 0;
    QFutureWatcher<void> alignWatcher;
    std::atomic_bool alignBusy {false};
    void arm(const QFuture<void>& ready)
    {
        watchGeometryFuture(m_cutWatcher, ready, m_waitingForCut,
                            [this] { onSectionCutFinished(); });
        waitingForAlign(true);
        watchGeometryFuture(alignWatcher, ready, alignBusy, [this] {
            waitingForAlign(false);
            onSectionCutFinished();
        });
    }
    TopoDS_Shape prepareShape(const TopoDS_Shape&, double) override
    {
        ++joins;
        return {};
    }
    void postSectionCutTasks() override {}
protected:
    TechDraw::GeometryObjectPtr buildGeometryObject(const TopoDS_Shape&, const gp_Ax2&) override
    {
        return {};
    }
};

class GeometryCompletion: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        Base::Interpreter().runString("import TechDraw");
    }
    void SetUp() override
    {
        if (!QCoreApplication::instance()) {
            application = std::make_unique<QCoreApplication>(argc, argv);
        }
        doc = App::GetApplication().newDocument("GeometryCompletion");
        preferences = TechDraw::Preferences::getPreferenceGroup("General");
        handleFaces = preferences->GetBool("HandleFaces", true);
        preferences->SetBool("HandleFaces", false);
    }
    void TearDown() override
    {
        App::GetApplication().closeDocument(doc->getName());
        preferences->SetBool("HandleFaces", handleFaces);
    }
    int argc = 1;
    char name[28] = "TechDrawGeometryCompletion";
    char* argv[2] = {name, nullptr};
    std::unique_ptr<QCoreApplication> application;
    App::Document* doc = nullptr;
    ParameterGrp::handle preferences;
    bool handleFaces = true;
};

TEST_F(GeometryCompletion, PublishesBeforeProducerReturns)
{
    auto* view = new EarlyCompletionView;
    doc->addObject(view, "View");
    view->CoarseView.setValue(false);
    TopoDS_Shape shape = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
    // Use the same serial owner lane as GUI recompute.
    doc->executionLane()->dispatchToOwner([&] {
        EXPECT_NO_THROW(view->partExec(shape));
        EXPECT_TRUE(view->completedBeforeReturn);
        EXPECT_TRUE(view->installed());
        EXPECT_TRUE(view->hasGeometry());
        EXPECT_FALSE(view->waitingForResult());
    });
}

TEST_F(GeometryCompletion, ReadyFutureCompletesOnDocumentOwner)
{
    int argc = 1;
    char name[] = "TechDrawGeometryCompletion";
    char* argv[] = {name, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (!QCoreApplication::instance()) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    auto* view = new EarlyCompletionView;
    doc->addObject(view, "View");
    QFutureWatcher<void> watcher;
    QFutureInterface<void> ready;
    ready.reportStarted();
    ready.reportFinished();
    std::atomic_bool busy {false};
    std::promise<bool> completed;
    auto result = completed.get_future();
    const auto connection = view->watchGeometryFuture(watcher, ready.future(), busy, [&] {
        busy = false;
        completed.set_value(doc->executionLane()->isOwnerThread());
    });
    EXPECT_TRUE(busy);
    QCoreApplication::processEvents();
    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(result.get());
    doc->executionLane()->dispatchToOwner([] {}); // Fence before destroying the watcher.
    EXPECT_FALSE(busy);
    QObject::disconnect(connection);
}

TEST_F(GeometryCompletion, MissingGeometryClearsBusyWithoutThrowing)
{
    auto* view = static_cast<TechDraw::DrawViewPart*>(
        doc->addObject("TechDraw::DrawViewPart", "View"));
    doc->executionLane()->dispatchToOwner([&] {
        view->waitingForHlr(true);
        view->waitingForFaces(true);
        EXPECT_NO_THROW(view->onHlrFinished());
        EXPECT_FALSE(view->waitingForResult());
    });
}

// Both futures are already finished and both GUI notifications are delivered
// while the owner is held. Future::isRunning() cannot distinguish the callbacks.
TEST_F(GeometryCompletion, QueuedSectionCompletionsJoinOncePerCycle)
{
    auto* view = new JoinedSection;
    doc->addObject(view, "Section");
    view->CoarseView.setValue(true);
    for (int cycle = 1; cycle <= 2; ++cycle) {
        QFutureInterface<void> ready;
        ready.reportStarted();
        ready.reportFinished();
        std::promise<void> release;
        auto gate = release.get_future();
        auto held = doc->executionLane()->postToOwner([&] { gate.wait(); });
        view->arm(ready.future());
        QCoreApplication::processEvents();
        release.set_value();
        held.get();
        doc->executionLane()->dispatchToOwner([&] { EXPECT_EQ(view->joins, cycle); });
    }
}

TEST_F(GeometryCompletion, RemovedViewDrainsCompletionAndCanBeRestored)
{
    auto* view = new EarlyCompletionView;
    doc->addObject(view, "View");
    doc->setTransactionMode(1);
    QFutureWatcher<void> watcher;
    QFutureInterface<void> ready;
    ready.reportStarted();
    ready.reportFinished();
    std::atomic_bool busy {false};
    bool installed = false;
    view->pending();
    view->watchGeometryFuture(watcher, ready.future(), busy, [&] { installed = true; });
    doc->openTransaction("Remove view");
    doc->removeObject("View");
    doc->commitTransaction();
    ASSERT_FALSE(doc->containsObject(view));
    view->purgeTouched();
    ASSERT_FALSE(view->isTouched());
    QCoreApplication::processEvents();
    doc->executionLane()->dispatchToOwner([&] {
        EXPECT_FALSE(busy);
        EXPECT_FALSE(view->hasPending());
        EXPECT_FALSE(installed);
        EXPECT_TRUE(view->isTouched());
    });
    ASSERT_TRUE(doc->undo());
    EXPECT_EQ(doc->getObject("View"), view);
    EXPECT_FALSE(busy);
    EXPECT_TRUE(view->isTouched());
}

TEST_F(GeometryCompletion, OwnerConstructedWatcherDeliversOnGuiThread)
{
    EarlyCompletionView* view = nullptr;
    std::unique_ptr<QFutureWatcher<void>> watcher;
    QFutureInterface<void> ready;
    ready.reportStarted();
    ready.reportFinished();
    std::atomic_bool busy {false};
    std::promise<bool> completed;
    auto result = completed.get_future();
    doc->executionLane()->dispatchToOwner([&] {
        view = new EarlyCompletionView;
        doc->addObject(view, "View");
        watcher = std::make_unique<QFutureWatcher<void>>();
        EXPECT_NE(watcher->thread(), QCoreApplication::instance()->thread());
        view->watchGeometryFuture(*watcher, ready.future(), busy, [&] {
            completed.set_value(doc->executionLane()->isOwnerThread());
        });
    });
    EXPECT_EQ(watcher->thread(), QCoreApplication::instance()->thread());
    QCoreApplication::processEvents();
    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(result.get());
    doc->executionLane()->dispatchToOwner([] {});
    EXPECT_FALSE(busy);
}

}
