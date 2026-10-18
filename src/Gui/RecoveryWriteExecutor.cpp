// SPDX-License-Identifier: LGPL-2.1-or-later
#include "RecoveryWriteExecutor.h"

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentExecutionLane.h>
#include <App/RecoveryAdmission.h>
#include <App/RecoverySnapshot.h>

#include <QCoreApplication>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <future>
#include <list>

using namespace std::chrono_literals;

namespace Gui
{

struct RecoveryWriteExecutor::Private
{
    using Pin = App::DocumentCollaborationService::LifecyclePin;
    struct Work
    {
        std::shared_ptr<App::DocumentExecutionLane> lane;
        std::shared_ptr<App::Internal::CollaborationServiceLifetimeGate> gate;
        App::RecoverySnapshotSaveOptions options;
        std::shared_ptr<App::RecoveryAdmission> admission;
        std::atomic<bool> cancelled {false};
        std::atomic<bool> flagged {false};
        std::atomic<bool> running {false};

        void releaseFlag()
        {
            if (flagged.exchange(false)) lane->endRecoverySnapshotOwnerWork();
        }
        void cancel()
        {
            cancelled = true;
            // A task still queued on its lane can no longer touch the model.
            // Let GUI close admission proceed without waiting for that task.
            if (!running.load()) releaseFlag();
        }

        void finish()
        {
            admission.reset();
            releaseFlag();
        }
        ~Work() { finish(); }
        bool run(bool onGui = false)
        {
            running = true;
            const auto retire = qScopeGuard([&] { finish(); });
            if (cancelled.load()) {
                return false;
            }
            // Acquire on the executing thread from a retained gate, never from
            // a raw document pointer. Shutdown may release the GUI pin before
            // this task starts; a sealed gate then refuses document access.
            Pin pin(gate);
            if (!pin) {
                return false;
            }
            if (!onGui) {
                admission->acquire();
            }
            if (cancelled.load()) {
                return false;
            }
            return pin.document()->canWriteRecoverySnapshot()
                && App::writeRecoverySnapshotToTransientDir(*pin.document(), options);
        }
    };
    struct Job
    {
        std::shared_ptr<Work> work;
        std::unique_ptr<Pin> pin; // Created and retired on the GUI thread.
        std::string name;
        App::DocumentRevisionIdentityBinding identity;
        Completion completion;
        std::future<bool> future;
    };

    explicit Private(RecoveryWriteExecutor* owner, unsigned cap)
        : limit(std::max(1U, cap))
    {
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, owner, [this] { poll(); });
    }

    void poll()
    {
        const auto callbacks = callbacksEnabled;
        std::list<Job> finished;
        for (auto it = jobs.begin(); it != jobs.end();) {
            auto current = it++;
            if (current->future.valid()
                && current->future.wait_for(0ms) == std::future_status::ready) {
                finished.splice(finished.end(), jobs, current);
                // Leave other ready jobs registered: this callback may close
                // one of their documents and cancel its completion.
                break;
            }
        }
        // Retire before invoking arbitrary GUI code, which may submit again,
        // close documents, or destroy the owner itself. Keep no owner access
        // after invoking the first callback.
        startPending();
        if (jobs.empty()) {
            timer.stop();
        }
        for (auto& job : finished) {
            bool written = false;
            std::exception_ptr failure;
            try { written = job.future.get(); }
            catch (...) { failure = std::current_exception(); }
            auto* doc = App::GetApplication().getDocument(job.name.c_str());
            const bool current = doc && doc->executionHandle().identity() == job.identity;
            job.pin.reset();
            if (callbacks->load() && !job.work->cancelled.load() && current && job.completion) {
                job.completion(written, failure);
            }
        }
    }

    static void runLegacy(const std::shared_ptr<Work>& work,
                          const std::shared_ptr<std::promise<bool>>& result)
    {
        try {
            if (!work->cancelled.load() && !work->admission->tryAcquire()) {
                QTimer::singleShot(10, QCoreApplication::instance(), [work, result] {
                    runLegacy(work, result);
                });
                return;
            }
            result->set_value(work->run(true));
        }
        catch (...) {
            work->finish();
            result->set_exception(std::current_exception());
        }
    }

    void startPending()
    {
        unsigned running = 0;
        for (const auto& job : jobs) {
            running += job.future.valid();
        }
        for (auto& job : jobs) {
            if (running == limit) break;
            if (job.future.valid()) continue;
            auto work = job.work;
            try {
                if (work->lane) {
                    job.future = work->lane->postToOwner(
                        [work] { return work->run(); }, /*allowWhileCommandActive=*/false);
                }
                else {
                    // Legacy documents have no lane: queue on the GUI owner,
                    // polling admission without blocking. This continuation
                    // retains no executor, including across save signals.
                    auto result = std::make_shared<std::promise<bool>>();
                    job.future = result->get_future();
                    QTimer::singleShot(0, QCoreApplication::instance(), [work, result] {
                        runLegacy(work, result);
                    });
                }
            }
            catch (...) {
                work->finish();
                job.pin.reset();
                std::promise<bool> result;
                job.future = result.get_future();
                result.set_exception(std::current_exception());
            }
            ++running;
        }
    }

    QTimer timer;
    const unsigned limit;
    bool accepting {true};
    std::shared_ptr<std::atomic<bool>> callbacksEnabled = std::make_shared<std::atomic<bool>>(true);
    std::list<Job> jobs;
};

RecoveryWriteExecutor::RecoveryWriteExecutor(QObject* parent, unsigned maxConcurrent)
    : QObject(parent), d(std::make_unique<Private>(this, maxConcurrent))
{}

RecoveryWriteExecutor::~RecoveryWriteExecutor()
{
    shutdown();
}

bool RecoveryWriteExecutor::schedule(App::Document& document,
                                    const App::RecoverySnapshotSaveOptions& options,
                                    Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    auto* lane = document.executionLane();
    if (lane && (!lane->isIdle() || lane->shutdownRequested())) return false;
    auto gate = document.collaborationService().lifetimeGate();
    if (!d->accepting || std::any_of(d->jobs.begin(), d->jobs.end(), [&](const auto& job) {
            return job.work->gate == gate;
        })) {
        return false;
    }
    auto pin = std::make_unique<Private::Pin>(gate);
    if (!*pin) return false;
    auto work = std::make_shared<Private::Work>();
    work->gate = std::move(gate);
    work->options = options;
    work->admission = std::make_shared<App::RecoveryAdmission>();
    if (lane) {
        work->lane = lane->shared_from_this();
        work->flagged = true;
        lane->beginRecoverySnapshotOwnerWork();
    }
    d->jobs.push_back({work, std::move(pin), document.getName(),
                       document.executionHandle().identity(), std::move(completion), {}});
    d->startPending();
    d->timer.start();
    return true;
}

bool RecoveryWriteExecutor::cancelDocument(App::Document& document, bool completedOnly)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto gate = document.collaborationService().lifetimeGate();
    bool cancelled = false;
    for (auto it = d->jobs.begin(); it != d->jobs.end();) {
        if (it->work->gate != gate) { ++it; continue; }
        if (completedOnly
            && (!it->future.valid() || it->future.wait_for(0ms) != std::future_status::ready)) {
            ++it;
            continue;
        }
        cancelled = true;
        it->work->cancel();
        it->completion = {};
        it->pin.reset();
        if (!it->future.valid()) it = d->jobs.erase(it);
        else ++it; // Any running task holds its own pin.
    }
    return cancelled;
}

bool RecoveryWriteExecutor::isAccepting() const
{
    return d->accepting;
}

bool RecoveryWriteExecutor::shutdown(std::chrono::milliseconds budget)
{
    Q_ASSERT(QThread::currentThread() == thread());
    d->accepting = false;
    *d->callbacksEnabled = false;
    d->timer.stop();
    for (auto& job : d->jobs) {
        job.work->cancel();
        job.completion = {};
    }
    const auto deadline = std::chrono::steady_clock::now() + std::max(budget, 0ms);
    bool drained = true;
    // No event pumping or GIL handoff: neither GUI re-entry nor reacquiring a
    // GIL held by a stuck writer may extend this deadline. Tasks needing the
    // GUI or GIL can retire after this call returns. All waits share one budget.
    for (auto& job : d->jobs) {
        if (job.future.valid()
            && job.future.wait_until(deadline) != std::future_status::ready) {
            drained = false;
        }
    }
    // postToOwner futures never join a thread. Cancelled tasks keep only Work
    // (gate, lane, options), with no owner/callback pointers. Running IO holds
    // its own thread-local lifecycle pin even after these GUI pins go away.
    d->jobs.clear();
    return drained;
}
}
