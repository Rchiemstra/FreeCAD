// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <Base/Interpreter.h>
#include <Mod/TechDraw/App/DrawViewPart.h>
#include <Mod/TechDraw/App/Preferences.h>
#include <src/App/InitApplication.h>

#include <BRepPrimAPI_MakeBox.hxx>
#include <QCoreApplication>
#include <QFutureInterface>

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

}
