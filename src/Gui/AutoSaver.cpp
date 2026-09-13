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
#include <Base/Console.h>
#include <Base/TimeInfo.h>
#include <Base/Tools.h>

#include "AutoSaver.h"
#include "Application.h"
#include "Document.h"
#include "MainWindow.h"
#include "WaitCursor.h"

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
        Base::Console().error("Failed to auto-save document '%s'\n", it->first.c_str());
    }
}

void AutoSaver::setTimeout(int ms)
{
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
    std::string name = Doc.getName();
    int id = timeout > 0 ? startTimer(timeout) : 0;
    AutoSaveProperty* as = new AutoSaveProperty(&Doc);
    as->timerId = id;
    saverMap.insert(std::make_pair(name, as));
}

void AutoSaver::slotDeleteDocument(const App::Document& Doc)
{
    std::string name = Doc.getName();
    std::map<std::string, AutoSaveProperty*>::iterator it = saverMap.find(name);
    if (it != saverMap.end()) {
        if (it->second->timerId > 0) {
            killTimer(it->second->timerId);
        }
        delete it->second;
        saverMap.erase(it);
    }
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
    options.saveThumbnail = false;

    Gui::WaitCursor wc;
    getMainWindow()->showMessage(tr("Wait until the auto-recovery file has been saved…"), 5000);
    // qApp->processEvents();

    Base::TimeElapsed startTime;
    try {
        if (!App::writeRecoverySnapshotToTransientDir(*doc, options)) {
            saver.restoreFailedSaveAttempt();
            doc->reportRecoverySaveOutcome(
                doc->TransientDir.getStrValue(), false, "Recovery snapshot was not stable");
            Base::Console().warning(
                "Auto-recovery write for document '%s' did not produce a stable snapshot\n",
                name.c_str()
            );
            return;
        }
    }
    catch (...) {
        saver.restoreFailedSaveAttempt();
        doc->reportRecoverySaveOutcome(
            doc->TransientDir.getStrValue(), false, "Recovery snapshot write threw an exception");
        throw;
    }

    saver.finishSuccessfulSaveAttempt();
    doc->reportRecoverySaveOutcome(doc->TransientDir.getStrValue(), true);

    Base::Console().log(
        "Save auto-recovery file in %fs\n",
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
        Base::Console().error("Failed to auto-save document '%s'\n", saver->first.c_str());
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
                Base::Console().error("Failed to auto-save document '%s'\n", it.first.c_str());
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
    QTimer::singleShot(0, AutoSaver::instance(), [qDocumentName, identity]() {
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
