/***************************************************************************
 *   Copyright (c) 2015 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include <QApplication>
#include <QTimer>
#include <QThread>

#include <App/Application.h>
#include <App/Document.h>
#include <App/RecoverySnapshot.h>
#include <App/MutationClassification.h>
#include <App/RecoveryAdmission.h>
#include <Base/Console.h>
#include <Base/TimeInfo.h>
#include <Base/Tools.h>

#include "AutoSaver.h"
#include "Utilities.h"
#include "Application.h"
#include "Document.h"
#include "DocumentExecutionIngress.h"
#include "MainWindow.h"

FC_LOG_LEVEL_INIT("App", true, true)

using namespace Gui;

namespace
{

bool isGuiDocumentStableForAutoSave(const App::Document* doc)
{
    if (!doc) {
        return false;
    }

    if (auto* app = Gui::Application::Instance) {
        if (auto* guiDoc = app->getDocument(doc)) {
            if (guiDoc->isPerformingTransaction()) {
                return false;
            }
        }
    }

    return true;
}

}  // namespace

AutoSaver* AutoSaver::self = nullptr;
const int AutoSaveTimeout = 900000;

AutoSaver::AutoSaver(QObject* parent)
    : QObject(parent)
    , timeout(AutoSaveTimeout)
    , compressed(true)
{
    App::GetApplication().signalNewDocument.connect([this](const App::Document& doc, bool) {
        slotCreateDocument(doc);
    });
    App::GetApplication().signalDeleteDocument.connect([this](const App::Document& doc) {
        slotDeleteDocument(doc);
    });
}

AutoSaver::~AutoSaver() = default;

AutoSaver* AutoSaver::instance()
{
    if (!self) {
        self = new AutoSaver(QApplication::instance());
    }
    return self;
}

void AutoSaver::flushPendingSave(const QString& documentName)
{
    // Queued retries may outlive their document. Re-check that it still exists
    // and let saveDocument() consume any dirty state still pending for this pass.
    const auto name = documentName.toStdString();
    auto it = saverMap.find(name);
    if (it == saverMap.end()) {
        return;
    }

    try {
        saveDocument(it->first, *it->second);
    }
    catch (...) {
        Base::Console().error("Failed to auto-save document '{}'\n", it->first);
    }
}

void AutoSaver::setTimeout(int ms)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, ms]() { setTimeout(ms); },
            Qt::QueuedConnection);
        return;
    }

    timeout = Base::clamp<int>(ms, 0, 3600000);  // between 0 and 60 min

    // go through the attached documents and apply the new timeout
    for (auto& it : saverMap) {
        if (it.second->timerId > 0) {
            killTimer(it.second->timerId);
        }
        int id = timeout > 0 ? startTimer(timeout) : 0;
        it.second->timerId = id;
    }
}

void AutoSaver::setCompressed(bool on)
{
    this->compressed = on;
}

void AutoSaver::slotCreateDocument(const App::Document& Doc)
{
    const std::string name = Doc.getName();
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, name]() {
                if (App::GetApplication().getDocument(name.c_str())) {
                    slotCreateDocument(*App::GetApplication().getDocument(name.c_str()));
                }
            },
            Qt::QueuedConnection);
        return;
    }

    int id = timeout > 0 ? startTimer(timeout) : 0;
    AutoSaveProperty* as = new AutoSaveProperty(App::GetApplication().getDocument(name.c_str()));
    as->timerId = id;
    saverMap.insert(std::make_pair(name, as));
}

void AutoSaver::slotDeleteDocument(const App::Document& Doc)
{
    const std::string name = Doc.getName();
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, name]() { slotDeleteDocumentByName(name); },
            Qt::QueuedConnection);
        return;
    }
    slotDeleteDocumentByName(name);
}

void AutoSaver::slotDeleteDocumentByName(const std::string& name)
{
    std::map<std::string, AutoSaveProperty*>::iterator it = saverMap.find(name);
    if (it != saverMap.end()) {
        if (it->second->timerId > 0) {
            killTimer(it->second->timerId);
        }
        delete it->second;
        saverMap.erase(it);
    }
}

void AutoSaver::queueRecoveryAdmissionRetry(const std::string& name,
                                             const std::uint64_t documentInstanceId,
                                             const std::uint64_t lifecycleEpoch)
{
    scheduleGuiSingleShot(10, [name, documentInstanceId, lifecycleEpoch] {
        AutoSaver::instance()->flushPendingSaveForIdentity(
            QString::fromStdString(name), documentInstanceId, lifecycleEpoch);
    });
}

void AutoSaver::saveDocument(const std::string& name, AutoSaveProperty& saver)
{
    Q_ASSERT(QThread::currentThread() == thread());

    App::Document* doc = App::GetApplication().getDocument(name.c_str());
    if (!doc) {
        return;
    }

    // Start an attempt without consuming dirty work. A recovery point becomes
    // authoritative only after the complete stable write succeeds.
    if (!saver.beginSaveAttempt()) {
        return;
    }

    if (!doc->canWriteRecoverySnapshot() || !isGuiDocumentStableForAutoSave(doc)) {
        // Keep timer-triggered saves out of unstable document states. Instead
        // of trying to save during recompute or an open transaction, just keep
        // the save pending. signalBecameStable() will queue a retry once the
        // document returns to a state where a consistent full snapshot can be
        // written.
        saver.deferSaveUntilStable();
        return;
    }

    ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document"
    );
    App::RecoverySnapshotSaveOptions options;
    options.compressed = this->compressed;
    options.saveBinaryBrep = !this->compressed || hGrp->GetBool("SaveBinaryBrep", true);
    // Recovery snapshots run on the lane owner. Thumbnails require the GUI
    // thread viewer, so keep them off this async path.
    options.saveThumbnail = false;

    if (auto* window = getMainWindow()) {
        window->showMessage(tr("Saving auto-recovery file…"), 5000);
    }

    const Base::TimeElapsed startTime;
    const auto identity = doc->collaborationIdentity();
    // AutoSaver runs on the GUI thread. Never call dispatchToOwner / sync
    // Document::save here — finish via the async recovery write and a queued
    // GUI callback when the worker completes.
    scheduleRecoverySnapshotWrite(
        *doc,
        options,
        [this,
         name,
         documentInstanceId = identity.instanceId,
         lifecycleEpoch = identity.lifecycleEpoch,
         startTime](const bool written, const std::exception_ptr failure) {
            completeRecoverySnapshotSave(
                name,
                documentInstanceId,
                lifecycleEpoch,
                startTime,
                written,
                failure);
        });
}

void AutoSaver::completeRecoverySnapshotSave(
    const std::string& name,
    const std::uint64_t documentInstanceId,
    const std::uint64_t lifecycleEpoch,
    const Base::TimeElapsed startTime,
    const bool written,
    const std::exception_ptr failure,
    std::shared_ptr<App::RecoveryAdmission> admission)
{
    Q_ASSERT(QThread::currentThread() == thread());

    const auto saverIt = saverMap.find(name);
    if (saverIt == saverMap.end()) {
        return;
    }
    AutoSaveProperty& saver = *saverIt->second;

    App::Document* doc = App::GetApplication().getDocument(name.c_str());
    if (!doc) {
        return;
    }

    const auto identity = doc->collaborationIdentity();
    if (identity.instanceId != documentInstanceId
        || identity.lifecycleEpoch != lifecycleEpoch) {
        return;
    }

    // Reporting the result is a save intent too. Keep that GUI notification
    // out of another command's audit and out of this document's busy lane.
    auto* guiDocument = Gui::Application::Instance
        ? Gui::Application::Instance->getDocument(doc)
        : nullptr;
    if (guiDocument && guiDocument->isAboutToClose()) {
        return;
    }
    const bool targetBusy = documentExecutionLaneBusy(*doc)
        || doc->collaborationNotificationsReplaying()
        || (guiDocument && guiDocument->isPerformingTransaction())
        || (guiDocument && !App::atomicPresentationMutationAdmissionActive()
            && guiDocument->deferLivePresentationUpdates());
    if (targetBusy) {
        // An already active owner may itself need process admission to finish.
        // Do not make it wait for a completion that is waiting for that lane.
        admission.reset();
    }
    else if (!admission) {
        admission = std::make_shared<App::RecoveryAdmission>();
    }
    if (targetBusy || !admission->tryAcquire()) {
        scheduleGuiSingleShot(10, [this, name, documentInstanceId, lifecycleEpoch,
                                  startTime, written, failure, admission = std::move(admission)] {
            completeRecoverySnapshotSave(
                name, documentInstanceId, lifecycleEpoch, startTime, written, failure, admission);
        });
        return;
    }

    if (failure) {
        try {
            std::rethrow_exception(failure);
        }
        catch (...) {
            saver.restoreFailedSaveAttempt();
            doc->reportRecoverySaveOutcome(
                doc->TransientDir.getStrValue(),
                false,
                "Recovery snapshot write threw an exception");
            Base::Console().error("Failed to auto-save document '{}'\n", name.c_str());
            return;
        }
    }

    if (!written) {
        // The worker could not reserve native admission; serialization never
        // started. Retain dirty work and resume only when it can be admitted.
        // Actual IO errors use failure above and remain visible failures.
        saver.deferSaveUntilStable();
        if (doc->canWriteRecoverySnapshot() && isGuiDocumentStableForAutoSave(doc)) {
            queueRecoveryAdmissionRetry(name, documentInstanceId, lifecycleEpoch);
        }
        return;
    }

    saver.finishSuccessfulSaveAttempt();
    doc->reportRecoverySaveOutcome(doc->TransientDir.getStrValue(), true);

    Base::Console().log(
        "Save auto-recovery file in {:f}s\n",
        Base::TimeElapsed::diffTimeF(startTime, Base::TimeElapsed())
    );
}

void AutoSaver::flushPendingSaveForIdentity(const QString& documentName,
                                            const std::uint64_t documentInstanceId,
                                            const std::uint64_t lifecycleEpoch)
{
    const auto name = documentName.toStdString();
    const auto saver = saverMap.find(name);
    if (saver == saverMap.end()) {
        return;
    }
    const auto* document = App::GetApplication().getDocument(name.c_str());
    if (!document) {
        return;
    }
    const auto identity = document->collaborationIdentity();
    if (identity.state != App::DocumentLifecycleState::Live
        || identity.instanceId != documentInstanceId
        || identity.lifecycleEpoch != lifecycleEpoch) {
        return;
    }

    try {
        saveDocument(saver->first, *saver->second);
    }
    catch (...) {
        Base::Console().error("Failed to auto-save document '{}'\n", saver->first.c_str());
    }
}

void AutoSaver::timerEvent(QTimerEvent* event)
{
    int id = event->timerId();
    for (auto& it : saverMap) {
        if (it.second->timerId == id) {
            try {
                saveDocument(it.first, *it.second);
                break;
            }
            catch (...) {
                Base::Console().error("Failed to auto-save document '{}'\n", it.first);
            }
        }
    }
}

// ----------------------------------------------------------------------------

AutoSaveProperty::AutoSaveProperty(const App::Document* doc)
    : timerId(-1)
{
    auto* mutableDoc = const_cast<App::Document*>(doc);
    fileChangeState = mutableDoc->signalFileChangeStateChanged().connect(
        [this](const App::Document& changedDoc) {
            slotFileChangeStateChanged(changedDoc);
        }
    );
    documentStable = mutableDoc->signalBecameStable.connect([this](const App::Document& changedDoc) {
        slotDocumentBecameStable(changedDoc);
    });

    documentName = doc->getName();
}

AutoSaveProperty::~AutoSaveProperty()
{
    fileChangeState.disconnect();
    documentStable.disconnect();
}

void AutoSaveProperty::slotFileChangeStateChanged(const App::Document& document)
{
    if (document.hasPendingFileChanges()) {
        markDirtyForAutosave();
        return;
    }
    if (!saveInProgress) {
        dirty = false;
        dirtyDuringSaveAttempt = false;
        blockedUntilStable = false;
    }
}

void AutoSaveProperty::markDirtyForAutosave()
{
    // Ordinary document changes are saved by the next timer pass. They do not
    // start a save attempt by themselves.
    dirty = true;
    if (saveInProgress) {
        dirtyDuringSaveAttempt = true;
    }
}

bool AutoSaveProperty::beginSaveAttempt()
{
    if (!dirty || saveInProgress) {
        blockedUntilStable = false;
        return false;
    }

    saveInProgress = true;
    dirtyDuringSaveAttempt = false;
    blockedUntilStable = false;
    return true;
}

void AutoSaveProperty::finishSuccessfulSaveAttempt()
{
    if (!saveInProgress) {
        return;
    }

    // The snapshot covers the dirty state that existed when the attempt
    // started. A re-entrant notification represents newer state and must be
    // retried instead of being consumed by this success.
    dirty = dirtyDuringSaveAttempt;
    saveInProgress = false;
    dirtyDuringSaveAttempt = false;
    blockedUntilStable = false;
}

void AutoSaveProperty::deferSaveUntilStable()
{
    dirty = true;
    saveInProgress = false;
    dirtyDuringSaveAttempt = false;
    blockedUntilStable = true;
}

void AutoSaveProperty::restoreFailedSaveAttempt()
{
    dirty = true;
    saveInProgress = false;
    dirtyDuringSaveAttempt = false;
    blockedUntilStable = false;
}

void AutoSaveProperty::scheduleQueuedRetry(const App::Document& document)
{
    if (!dirty) {
        return;
    }

    const QString qDocumentName = QString::fromStdString(documentName);
    const auto identity = document.collaborationIdentity();

    // Queue a later GUI-thread pass instead of flushing inline from
    // signalBecameStable(). beginSaveAttempt() re-checks dirty state when the
    // queued retry runs.
    scheduleGuiSingleShot(0, [qDocumentName, identity]() {
        AutoSaver::instance()->flushPendingSaveForIdentity(
            qDocumentName, identity.instanceId, identity.lifecycleEpoch);
    });
}

void AutoSaveProperty::slotDocumentBecameStable(const App::Document& document)
{
    // Stability only means "it is now legal to try again". Only retry saves that
    // were already due and then blocked by an unstable state; ordinary dirty
    // changes should wait for the configured autosave timer.
    if (!blockedUntilStable) {
        return;
    }

    blockedUntilStable = false;
    scheduleQueuedRetry(document);
}


#include "moc_AutoSaver.cpp"
