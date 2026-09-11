// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentChangesWidget.h"

#include <QCheckBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStringList>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <App/Application.h>
#include <App/Document.h>

#include "Application.h"
#include "Document.h"
#include "StatusBarLabel.h"

namespace Gui
{

namespace
{

QString dispositionText(const App::DocumentSaveDisposition disposition)
{
    switch (disposition) {
        case App::DocumentSaveDisposition::Written:
            return DocumentChangesWidget::tr("Written");
        case App::DocumentSaveDisposition::Unchanged:
            return DocumentChangesWidget::tr("Unchanged");
        case App::DocumentSaveDisposition::CopyWritten:
            return DocumentChangesWidget::tr("Copy written");
        case App::DocumentSaveDisposition::Failed:
            return DocumentChangesWidget::tr("Failed");
    }
    return DocumentChangesWidget::tr("Failed");
}

}  // namespace

DocumentChangesWidget::DocumentChangesWidget(StatusBarLabel* status, QWidget* parent)
    : QWidget(parent)
    , statusLabel(status)
{
    setObjectName(QStringLiteral("DocumentChanges"));
    setWindowTitle(tr("Document Changes"));
    setAccessibleName(tr("Document Changes"));

    auto* layout = new QVBoxLayout(this);
    auto* summary = new QFormLayout;
    pathValue = new QLabel(this);
    pathValue->setObjectName(QStringLiteral("documentCanonicalPath"));
    pathValue->setAccessibleName(tr("Active document canonical path"));
    pathValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathValue->setWordWrap(true);
    stateValue = new QLabel(this);
    stateValue->setObjectName(QStringLiteral("documentFileStateValue"));
    stateValue->setAccessibleName(tr("Document file state"));
    categoriesValue = new QLabel(this);
    categoriesValue->setObjectName(QStringLiteral("documentPendingCategories"));
    categoriesValue->setAccessibleName(tr("Pending persistent change categories"));
    readinessValue = new QLabel(this);
    readinessValue->setObjectName(QStringLiteral("documentMutationReadiness"));
    readinessValue->setAccessibleName(tr("Document mutation readiness"));
    readinessValue->setWordWrap(true);
    operationValue = new QLabel(tr("No active agent operation reported"), this);
    operationValue->setObjectName(QStringLiteral("agentOperationValue"));
    operationValue->setWordWrap(true);
    pauseAgentWrites = new QCheckBox(tr("Pause agent writes"), this);
    pauseAgentWrites->setObjectName(QStringLiteral("pauseAgentWrites"));
    pauseAgentWrites->setAccessibleName(tr("Pause agent writes after the current operation"));
    // The MCP addon owns admission state and reveals/connects this local-only
    // control when it is installed. Core GUI never depends on the addon.
    pauseAgentWrites->hide();
    summary->addRow(tr("Path:"), pathValue);
    summary->addRow(tr("File state:"), stateValue);
    summary->addRow(tr("Pending:"), categoriesValue);
    summary->addRow(tr("Mutation readiness:"), readinessValue);
    summary->addRow(tr("Agent operation:"), operationValue);
    layout->addLayout(summary);
    layout->addWidget(pauseAgentWrites);

    cameraNote = new QLabel(
        tr("View changed locally. The file is unchanged. This view will be stored with the next "
           "document save."),
        this);
    cameraNote->setObjectName(QStringLiteral("cameraOnlyChangeNote"));
    cameraNote->setWordWrap(true);
    cameraNote->setAccessibleName(tr("Camera-only change"));
    layout->addWidget(cameraNote);

    auto* historyTitle = new QLabel(tr("Session history"), this);
    historyTitle->setAccessibleName(tr("Document change history"));
    layout->addWidget(historyTitle);
    historyList = new QListWidget(this);
    historyList->setObjectName(QStringLiteral("documentChangeHistory"));
    historyList->setAccessibleName(tr("Session-local document change history"));
    layout->addWidget(historyList, 1);
    clearHistoryButton = new QPushButton(tr("Clear history"), this);
    clearHistoryButton->setObjectName(QStringLiteral("clearDocumentChangeHistory"));
    clearHistoryButton->setAccessibleName(tr("Clear session-local document change history"));
    layout->addWidget(clearHistoryButton);
    connect(clearHistoryButton, &QPushButton::clicked, this, [this]() {
        if (activeDocument) {
            histories[activeDocument].clear();
        }
        refreshHistory();
    });

    refreshTimer = new QTimer(this);
    refreshTimer->setInterval(500);
    connect(refreshTimer, &QTimer::timeout, this, [this]() {
        if (activeDocument) {
            refresh();
        }
    });
    refreshTimer->start();

    if (statusLabel) {
        statusLabel->setObjectName(QStringLiteral("documentFileState"));
        statusLabel->setAccessibleName(tr("Document file state"));
        statusLabel->setTextFormat(Qt::RichText);
        statusLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    }

    if (Application::Instance) {
        newDocumentConnection = Application::Instance->signalNewDocument.connect(
            [this](const Gui::Document& document, bool) {
                observeDocument(const_cast<Gui::Document&>(document));
            });
        activeDocumentConnection = Application::Instance->signalActiveDocument.connect(
            [this](const Gui::Document& document) {
                bindDocument(const_cast<Gui::Document*>(&document));
            });
        deletedDocumentConnection = Application::Instance->signalDeleteDocument.connect(
            [this](const Gui::Document& document) {
                auto* appDocument = document.getDocument();
                forgetDocument(document);
                histories.erase(appDocument);
                cameraActivity.erase(appDocument);
                if (activeGuiDocument == &document) {
                    bindDocument(nullptr);
                }
            });
        for (auto* appDocument : App::GetApplication().getDocuments()) {
            if (auto* guiDocument = Application::Instance->getDocument(appDocument)) {
                observeDocument(*guiDocument);
            }
        }
        bindDocument(Application::Instance->activeDocument());
    }
    else {
        refresh();
    }
}

DocumentChangesWidget::~DocumentChangesWidget()
{
    newDocumentConnection.disconnect();
    activeDocumentConnection.disconnect();
    deletedDocumentConnection.disconnect();
    for (auto& [document, connections] : observedDocuments) {
        static_cast<void>(document);
        connections.fileState.disconnect();
        connections.saveOutcome.disconnect();
        connections.camera.disconnect();
    }
}

void DocumentChangesWidget::setDockContainer(QDockWidget* dock)
{
    dockContainer = dock;
}

void DocumentChangesWidget::showPanel()
{
    if (!dockContainer) {
        return;
    }
    dockContainer->show();
    dockContainer->raise();
}

void DocumentChangesWidget::observeDocument(Gui::Document& document)
{
    auto* appDocument = document.getDocument();
    if (!appDocument || observedDocuments.contains(appDocument)) {
        return;
    }
    ObservedDocumentConnections connections;
    connections.fileState = appDocument->signalFileChangeStateChanged().connect(
        [this](const App::Document& changed) { onFileStateChanged(changed); });
    connections.saveOutcome = appDocument->signalSaveOutcome().connect(
        [this](const App::Document& changed, const App::DocumentSaveOutcome& outcome) {
            onSaveOutcome(changed, outcome);
        });
    connections.camera = document.signalCameraActivity.connect(
        [this](const Gui::Document& changed) { onCameraActivity(changed); });
    observedDocuments.emplace(appDocument, std::move(connections));
}

void DocumentChangesWidget::forgetDocument(const Gui::Document& document)
{
    auto* appDocument = document.getDocument();
    const auto found = observedDocuments.find(appDocument);
    if (found == observedDocuments.end()) {
        return;
    }
    found->second.fileState.disconnect();
    found->second.saveOutcome.disconnect();
    found->second.camera.disconnect();
    observedDocuments.erase(found);
}

void DocumentChangesWidget::bindDocument(Gui::Document* document)
{
    activeGuiDocument = document;
    activeDocument = document ? document->getDocument() : nullptr;
    if (document) {
        observeDocument(*document);
    }
    refresh();
    refreshHistory();
}

void DocumentChangesWidget::refresh()
{
    if (!activeDocument) {
        pathValue->setText(tr("No active document"));
        stateValue->setText(tr("No document"));
        categoriesValue->setText(tr("None"));
        readinessValue->setText(tr("No document"));
        cameraNote->hide();
        if (statusLabel) {
            statusLabel->clear();
            statusLabel->setToolTip({});
        }
        return;
    }

    const QString path = QString::fromStdString(activeDocument->FileName.getStrValue());
    pathValue->setText(path.isEmpty() ? tr("Not saved") : path);

    QString state;
    if (activeDocument->lastCanonicalSaveFailed()) {
        state = tr("Save failed");
    }
    else {
        switch (activeDocument->getFileChangeState()) {
            case App::DocumentFileState::NotSaved:
                state = tr("Not saved");
                break;
            case App::DocumentFileState::Clean:
                state = tr("Saved");
                break;
            case App::DocumentFileState::Modified:
                state = tr("Unsaved");
                break;
        }
    }
    stateValue->setText(state);

    QStringList categories;
    const auto pending = activeDocument->getPendingFileChanges();
    if (pending.testFlag(App::DocumentFileChange::Model)) {
        categories.append(tr("Model"));
    }
    if (pending.testFlag(App::DocumentFileChange::Appearance)) {
        categories.append(tr("Appearance"));
    }
    categoriesValue->setText(categories.isEmpty() ? tr("None") : categories.join(tr(", ")));

    const auto readiness = activeDocument->getMutationReadiness();
    readinessValue->setText(
        readiness.ready ? tr("Ready") : QString::fromStdString(readiness.diagnostic));
    cameraNote->setVisible(cameraActivity[activeDocument]);

    if (statusLabel) {
        statusLabel->setText(QStringLiteral("<a href=\"document-changes\">%1</a>")
                                 .arg(state.toHtmlEscaped()));
        statusLabel->setToolTip(path.isEmpty() ? tr("Document has no canonical path") : path);
    }
}

void DocumentChangesWidget::refreshHistory()
{
    historyList->clear();
    if (!activeDocument) {
        return;
    }
    const auto found = histories.find(activeDocument);
    if (found == histories.end()) {
        return;
    }
    for (const auto& entry : found->second) {
        historyList->addItem(entry);
    }
    historyList->scrollToBottom();
}

void DocumentChangesWidget::appendHistory(const App::Document& document, const QString& entry)
{
    auto& history = histories[&document];
    const QString timestamped = tr("%1 — %2").arg(
        QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), entry);
    history.push_back(timestamped);
    while (history.size() > 100) {
        history.pop_front();
    }
    if (activeDocument == &document) {
        refreshHistory();
    }
}

void DocumentChangesWidget::onFileStateChanged(const App::Document& document)
{
    QStringList categories;
    const auto pending = document.getPendingFileChanges();
    if (pending.testFlag(App::DocumentFileChange::Model)) {
        categories.append(tr("Model"));
    }
    if (pending.testFlag(App::DocumentFileChange::Appearance)) {
        categories.append(tr("Appearance"));
    }
    const QString entry = categories.isEmpty()
        ? tr("Document reached its canonical savepoint")
        : tr("Persistent changes: %1").arg(categories.join(tr(", ")));
    // Coalesce repeated changes in the same grouped category while retaining
    // every save/copy/recovery outcome as a separate historical event.
    auto& history = histories[&document];
    if (!history.empty() && history.back().endsWith(entry)) {
        history.pop_back();
    }
    appendHistory(document, entry);
    if (activeDocument == &document) {
        refresh();
    }
}

void DocumentChangesWidget::onSaveOutcome(const App::Document& document,
                                          const App::DocumentSaveOutcome& outcome)
{
    appendHistory(
        document,
        tr("Save: %1 — %2\nPath: %3")
            .arg(dispositionText(outcome.disposition),
                 QString::fromStdString(outcome.message),
                 QString::fromStdString(outcome.targetPath.empty() ? outcome.canonicalPath
                                                                  : outcome.targetPath)));
    if (outcome.disposition == App::DocumentSaveDisposition::Written) {
        cameraActivity[&document] = false;
    }
    if (activeDocument == &document) {
        operationValue->setText(tr("Last persistence operation: %1")
                                    .arg(dispositionText(outcome.disposition)));
        refresh();
    }
    if (outcome.disposition == App::DocumentSaveDisposition::Failed
        && outcome.intent != App::DocumentSaveIntent::Copy
        && outcome.intent != App::DocumentSaveIntent::Recovery) {
        showPanel();
    }
}

void DocumentChangesWidget::onCameraActivity(const Gui::Document& document)
{
    auto* appDocument = document.getDocument();
    if (!appDocument) {
        return;
    }
    cameraActivity[appDocument] = true;
    if (activeDocument == appDocument) {
        refresh();
    }
}

}  // namespace Gui
