// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"
#ifndef _PreComp_
# include <BRepBuilderAPI_Copy.hxx>
# include <QCheckBox>
# include <QDialog>
# include <QDialogButtonBox>
# include <QHBoxLayout>
# include <QHeaderView>
# include <QLabel>
# include <QMessageBox>
# include <QPushButton>
# include <QTableWidget>
# include <QVBoxLayout>
# include <QtConcurrent>
# include <Inventor/nodes/SoCoordinate3.h>
# include <Inventor/nodes/SoDrawStyle.h>
# include <Inventor/nodes/SoGroup.h>
# include <Inventor/nodes/SoLineSet.h>
# include <Inventor/nodes/SoMaterial.h>
# include <Inventor/nodes/SoPickStyle.h>
# include <Inventor/nodes/SoPointSet.h>
# include <Inventor/nodes/SoSeparator.h>
# include <set>
#endif

#include "TaskInterferenceCheck.h"

#include <cstring>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/Property.h>
#include <Base/Console.h>
#include <Base/Quantity.h>
#include <Base/Unit.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Control.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/QuantitySpinBox.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Utilities.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <Mod/Assembly/App/AssemblyObject.h>
#include <Mod/Part/App/TopoShape.h>
#include <Mod/Part/Gui/ViewProviderExt.h>
#include <Mod/Part/Gui/ViewProviderPreviewExtension.h>

#include <QFutureWatcher>
#include <QMetaObject>
#include <cmath>

using namespace Assembly;
using namespace AssemblyGui;

namespace
{

QString kindText(Part::InterferenceKind kind, bool excluded)
{
    if (excluded) {
        return QObject::tr("Excluded");
    }
    switch (kind) {
        case Part::InterferenceKind::Penetration:
            return QObject::tr("Penetration");
        case Part::InterferenceKind::Contact:
            return QObject::tr("Contact");
        case Part::InterferenceKind::ClearanceViolation:
            return QObject::tr("Clearance");
        case Part::InterferenceKind::InvalidInput:
            return QObject::tr("Invalid");
        case Part::InterferenceKind::Inconclusive:
            return QObject::tr("Inconclusive");
        default:
            return QObject::tr("Other");
    }
}

SbColor colorForKind(Part::InterferenceKind kind)
{
    switch (kind) {
        case Part::InterferenceKind::Penetration:
            return SbColor(0.9F, 0.15F, 0.15F);
        case Part::InterferenceKind::Contact:
            return SbColor(1.0F, 0.55F, 0.1F);
        case Part::InterferenceKind::ClearanceViolation:
            return SbColor(0.95F, 0.75F, 0.15F);
        default:
            return SbColor(0.6F, 0.6F, 0.6F);
    }
}

bool isViolationKind(Part::InterferenceKind kind)
{
    return kind == Part::InterferenceKind::Penetration || kind == Part::InterferenceKind::Contact
        || kind == Part::InterferenceKind::ClearanceViolation;
}

bool hasValidClosestPoints(const Part::InterferenceResult& detection)
{
    return detection.minimumDistance >= 0.0 && std::isfinite(detection.minimumDistance)
        && (detection.kind == Part::InterferenceKind::Clear
            || detection.kind == Part::InterferenceKind::ClearanceViolation
            || detection.kind == Part::InterferenceKind::Contact
            || detection.kind == Part::InterferenceKind::Penetration);
}

}  // namespace

TaskInterferenceCheck::TaskInterferenceCheck(AssemblyObject* assembly, QWidget* parent)
    : QWidget(parent)
    , assembly(assembly)
{
    setupUi();
    connectDocumentSignals();

    if (assembly) {
        clearanceSpin->setValue(
            Base::Quantity(assembly->getInterferenceClearance(), Base::Unit::Length)
        );
    }

    attachPreviewToViewer();
    updateRowActionState();
}

TaskInterferenceCheck::~TaskInterferenceCheck()
{
    closeManageExclusionsDialog();
    disconnectDocumentSignals();
    session.requestCancel();
    discardResults();
    detachPreviewFromViewer();
}

void TaskInterferenceCheck::setupUi()
{
    auto* layout = new QVBoxLayout(this);

    statusLabel = new QLabel(tr("Ready. Run a scan to inspect the active assembly."), this);
    layout->addWidget(statusLabel);
    progressLabel = new QLabel(QString(), this);
    layout->addWidget(progressLabel);

    auto* controls = new QHBoxLayout;
    clearanceSpin = new Gui::QuantitySpinBox(this);
    clearanceSpin->setUnit(Base::Unit::Length);
    clearanceSpin->setMinimum(0.0);
    clearanceSpin->setMaximum(1.0e6);
    clearanceSpin->setValue(Base::Quantity(0.0, Base::Unit::Length));
    controls->addWidget(new QLabel(tr("Clearance:"), this));
    controls->addWidget(clearanceSpin);
    includeHiddenCheck = new QCheckBox(tr("Include hidden"), this);
    showExcludedCheck = new QCheckBox(tr("Show excluded"), this);
    controls->addWidget(includeHiddenCheck);
    controls->addWidget(showExcludedCheck);
    controls->addStretch();
    layout->addLayout(controls);

    auto* buttons = new QHBoxLayout;
    runButton = new QPushButton(tr("Run"), this);
    cancelButton = new QPushButton(tr("Cancel scan"), this);
    cancelButton->setEnabled(false);
    selectPairButton = new QPushButton(tr("Select pair"), this);
    excludeButton = new QPushButton(tr("Exclude source pair"), this);
    restoreButton = new QPushButton(tr("Restore source pair"), this);
    manageExclusionsButton = new QPushButton(tr("Manage exclusions…"), this);
    buttons->addWidget(runButton);
    buttons->addWidget(cancelButton);
    buttons->addWidget(selectPairButton);
    buttons->addWidget(excludeButton);
    buttons->addWidget(restoreButton);
    buttons->addWidget(manageExclusionsButton);
    layout->addLayout(buttons);

    summaryLabel = new QLabel(tr("No results."), this);
    summaryLabel->setWordWrap(true);
    layout->addWidget(summaryLabel);

    resultsTable = new QTableWidget(0, 5, this);
    resultsTable->setHorizontalHeaderLabels(
        {tr("Status"), tr("Occurrence A"), tr("Occurrence B"), tr("Min clearance"), tr("Overlap volume")}
    );
    resultsTable->horizontalHeader()->setStretchLastSection(true);
    resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    resultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    resultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(resultsTable);

    connect(runButton, &QPushButton::clicked, this, &TaskInterferenceCheck::onRun);
    connect(cancelButton, &QPushButton::clicked, this, &TaskInterferenceCheck::onCancelScan);
    connect(selectPairButton, &QPushButton::clicked, this, &TaskInterferenceCheck::onSelectPair);
    connect(excludeButton, &QPushButton::clicked, this, &TaskInterferenceCheck::onExcludePair);
    connect(restoreButton, &QPushButton::clicked, this, &TaskInterferenceCheck::onRestorePair);
    connect(manageExclusionsButton, &QPushButton::clicked, this, &TaskInterferenceCheck::onManageExclusions);
    connect(showExcludedCheck, &QCheckBox::toggled, this, &TaskInterferenceCheck::onShowExcludedToggled);
    connect(
        includeHiddenCheck,
        &QCheckBox::toggled,
        this,
        &TaskInterferenceCheck::onIncludeHiddenToggled
    );
    connect(
        clearanceSpin,
        qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
        this,
        [this](double) {
            if (hasResults()) {
                markStale("Clearance changed");
            }
        }
    );
    connect(
        resultsTable,
        &QTableWidget::itemSelectionChanged,
        this,
        &TaskInterferenceCheck::onRowChanged
    );
}

bool TaskInterferenceCheck::accept()
{
    closeManageExclusionsDialog();
    session.requestCancel();
    discardResults();
    detachPreviewFromViewer();
    return true;
}

bool TaskInterferenceCheck::reject()
{
    closeManageExclusionsDialog();
    session.requestCancel();
    discardResults();
    detachPreviewFromViewer();
    return true;
}

bool TaskInterferenceCheck::isSelectPairEnabled() const
{
    return selectPairButton && selectPairButton->isEnabled();
}

bool TaskInterferenceCheck::isExcludePairEnabled() const
{
    return excludeButton && excludeButton->isEnabled();
}

void TaskInterferenceCheck::testDeliverScanFinished(
    std::uint64_t generation,
    const Assembly::InterferenceScanResult& result
)
{
    onScanFinished(generation, result);
}

QString TaskInterferenceCheck::testStatusText() const
{
    return statusLabel ? statusLabel->text() : QString();
}

QString TaskInterferenceCheck::testProgressText() const
{
    return progressLabel ? progressLabel->text() : QString();
}

int TaskInterferenceCheck::testTableRowCount() const
{
    return resultsTable ? resultsTable->rowCount() : 0;
}

QString TaskInterferenceCheck::testTableCellText(int row, int column) const
{
    if (!resultsTable) {
        return {};
    }
    const auto* item = resultsTable->item(row, column);
    return item ? item->text() : QString();
}

std::size_t TaskInterferenceCheck::testResultPairCount() const
{
    return lastResult.pairs.size();
}

Base::Unit TaskInterferenceCheck::testClearanceUnit() const
{
    return clearanceSpin ? clearanceSpin->unit() : Base::Unit();
}

double TaskInterferenceCheck::testClearanceRawMm() const
{
    return clearanceSpin ? clearanceSpin->rawValue() : 0.0;
}

void TaskInterferenceCheck::testSetClearanceQuantity(const Base::Quantity& quantity)
{
    if (clearanceSpin) {
        clearanceSpin->setValue(quantity);
    }
}

Gui::View3DInventorViewer* TaskInterferenceCheck::viewer() const
{
    if (attachedViewer) {
        return attachedViewer;
    }
    auto* mainWindow = Gui::getMainWindow();
    if (!mainWindow) {
        return nullptr;
    }
    auto* view = qobject_cast<Gui::View3DInventor*>(mainWindow->activeWindow());
    return view ? view->getViewer() : nullptr;
}

Gui::Document* TaskInterferenceCheck::assemblyGuiDocument() const
{
    if (!assembly || !assembly->getDocument() || !Gui::Application::Instance) {
        return nullptr;
    }
    return Gui::Application::Instance->getDocument(assembly->getDocument());
}

std::vector<std::pair<std::string, std::string>>
TaskInterferenceCheck::currentExclusionSourceIds() const
{
    std::vector<std::pair<std::string, std::string>> excluded;
    if (!assembly) {
        return excluded;
    }
    auto objectKey = [](const App::DocumentObject* obj) -> std::string {
        if (!obj || !obj->isAttachedToDocument()) {
            return {};
        }
        return std::string(obj->getDocument()->getName()) + "#" + obj->getNameInDocument();
    };
    for (const auto& rule : assembly->getInterferenceExclusionRules()) {
        if (!rule.valid) {
            continue;
        }
        const std::string a = objectKey(rule.first);
        const std::string b = objectKey(rule.second);
        if (a.empty() || b.empty()) {
            continue;
        }
        excluded.emplace_back(a <= b ? std::make_pair(a, b) : std::make_pair(b, a));
    }
    return excluded;
}

App::DocumentObject* TaskInterferenceCheck::resolveSourceId(const std::string& sourceId) const
{
    const auto sep = sourceId.find('#');
    if (sep == std::string::npos) {
        return nullptr;
    }
    auto* doc = App::GetApplication().getDocument(sourceId.substr(0, sep).c_str());
    if (!doc) {
        return nullptr;
    }
    return doc->getObject(sourceId.substr(sep + 1).c_str());
}

void TaskInterferenceCheck::attachPreviewToViewer()
{
    detachPreviewFromViewer();
    auto* mainWindow = Gui::getMainWindow();
    if (!mainWindow) {
        return;
    }
    auto* viewWin = qobject_cast<Gui::View3DInventor*>(mainWindow->activeWindow());
    if (!viewWin) {
        return;
    }
    auto* view = viewWin->getViewer();
    if (!view) {
        return;
    }
    previewRoot = new SoSeparator;
    previewRoot->ref();
    auto* pick = new SoPickStyle;
    pick->style = SoPickStyle::UNPICKABLE;
    previewRoot->addChild(pick);
    if (auto* scene = dynamic_cast<SoGroup*>(view->getSceneGraph())) {
        scene->addChild(previewRoot);
        attachedViewer = view;
        attachedView = viewWin;
        connect(viewWin, &QObject::destroyed, this, [this]() {
            attachedViewer = nullptr;
            attachedView.clear();
            if (previewRoot) {
                previewRoot->unref();
                previewRoot = nullptr;
            }
            markStale("View closed");
        });
    }
    else {
        previewRoot->unref();
        previewRoot = nullptr;
    }
}

void TaskInterferenceCheck::detachPreviewFromViewer()
{
    clearPreview();
    if (previewRoot && attachedViewer) {
        if (auto* scene = dynamic_cast<SoGroup*>(attachedViewer->getSceneGraph())) {
            const int idx = scene->findChild(previewRoot);
            if (idx >= 0) {
                scene->removeChild(idx);
            }
        }
    }
    if (previewRoot) {
        previewRoot->unref();
        previewRoot = nullptr;
    }
    attachedViewer = nullptr;
    attachedView.clear();
}

void TaskInterferenceCheck::discardResults()
{
    lastResult = {};
    clearPreview();
    if (resultsTable) {
        resultsTable->setRowCount(0);
    }
    if (summaryLabel) {
        summaryLabel->setText(tr("No results."));
    }
    if (progressLabel) {
        progressLabel->clear();
    }
    updateRowActionState();
}

void TaskInterferenceCheck::setScanControlsEnabled(bool enabled)
{
    if (clearanceSpin) {
        clearanceSpin->setEnabled(enabled);
    }
    if (includeHiddenCheck) {
        includeHiddenCheck->setEnabled(enabled);
    }
    if (runButton) {
        runButton->setEnabled(enabled);
    }
    if (manageExclusionsButton) {
        manageExclusionsButton->setEnabled(enabled && assembly);
    }
    updateRowActionState();
}

void TaskInterferenceCheck::updateRowActionState()
{
    const bool idle = !session.isBusy();
    const int pairIndex = currentPairIndex();
    const bool hasPair = idle && pairIndex >= 0 && assembly;
    bool canExclude = false;
    bool canRestore = false;
    if (hasPair) {
        const auto& pair = lastResult.pairs[static_cast<std::size_t>(pairIndex)];
        canExclude = !pair.excluded && isViolationKind(pair.detection.kind);
        canRestore = pair.excluded;
    }
    if (selectPairButton) {
        selectPairButton->setEnabled(hasPair);
    }
    if (excludeButton) {
        excludeButton->setEnabled(canExclude);
    }
    if (restoreButton) {
        restoreButton->setEnabled(canRestore);
    }
    if (resultsTable) {
        resultsTable->setEnabled(idle);
    }
}

int TaskInterferenceCheck::currentPairIndex() const
{
    if (!resultsTable || resultsTable->currentRow() < 0) {
        return -1;
    }
    auto* statusItem = resultsTable->item(resultsTable->currentRow(), 0);
    if (!statusItem) {
        return -1;
    }
    const int pairIndex = statusItem->data(Qt::UserRole).toInt();
    if (pairIndex < 0 || pairIndex >= static_cast<int>(lastResult.pairs.size())) {
        return -1;
    }
    return pairIndex;
}

bool TaskInterferenceCheck::isResultAffectingProperty(const App::Property& prop) const
{
    const char* name = prop.getName();
    if (!name) {
        return false;
    }
    return std::strcmp(name, "Placement") == 0 || std::strcmp(name, "LinkPlacement") == 0
        || std::strcmp(name, "Group") == 0 || std::strcmp(name, "Visibility") == 0
        || std::strcmp(name, "VisibilityList") == 0 || std::strcmp(name, "ShowElement") == 0
        || std::strcmp(name, "LinkedObject") == 0 || std::strcmp(name, "ElementList") == 0
        || std::strcmp(name, "ElementCount") == 0 || std::strcmp(name, "PlacementList") == 0
        || std::strcmp(name, "ScaleList") == 0 || std::strcmp(name, "Scale") == 0
        || std::strcmp(name, "ScaleVector") == 0 || std::strcmp(name, "Shape") == 0
        || std::strcmp(name, "Tip") == 0 || std::strcmp(name, "InterferenceClearance") == 0
        || std::strcmp(name, "InterferenceExcludedSources") == 0;
}

QString TaskInterferenceCheck::formatLength(double mm) const
{
    return QString::fromStdString(Base::Quantity(mm, Base::Unit::Length).getUserString());
}

QString TaskInterferenceCheck::formatVolume(double cubicMm) const
{
    return QString::fromStdString(Base::Quantity(cubicMm, Base::Unit::Volume).getUserString());
}

QString TaskInterferenceCheck::formatPairDistance(const Part::InterferenceResult& detection) const
{
    if (detection.minimumDistance < 0.0 || !std::isfinite(detection.minimumDistance)) {
        if (!detection.diagnostic.empty()) {
            return QString::fromStdString(detection.diagnostic);
        }
        return QStringLiteral("—");
    }
    return formatLength(detection.minimumDistance);
}

QString TaskInterferenceCheck::formatPairVolume(const Part::InterferenceResult& detection) const
{
    if (detection.kind != Part::InterferenceKind::Penetration
        || !std::isfinite(detection.overlapVolume) || detection.overlapVolume <= 0.0) {
        return QStringLiteral("—");
    }
    return formatVolume(detection.overlapVolume);
}

void TaskInterferenceCheck::closeManageExclusionsDialog()
{
    if (manageExclusionsDialog) {
        manageExclusionsDialog->reject();
        manageExclusionsDialog.clear();
    }
}

void TaskInterferenceCheck::connectDocumentSignals()
{
    connections.clear();
    watchedDocuments.clear();
    if (!assembly || !assembly->getDocument()) {
        return;
    }

    auto attachDocument = [this](App::Document* doc) {
        if (!doc || !watchedDocuments.insert(doc).second) {
            return;
        }
        connections.push_back(doc->signalRecomputed.connect(
            [this](const App::Document& recomputed, const std::vector<App::DocumentObject*>&) {
                if (watchedDocuments.count(&recomputed) != 0) {
                    markStale("Document recompute");
                }
            }
        ));
        connections.push_back(doc->signalChangedObject.connect(
            [this](const App::DocumentObject& obj, const App::Property& prop) {
                if (!assembly || !isResultAffectingProperty(prop)) {
                    return;
                }
                if (&obj == assembly || watchedDocuments.count(obj.getDocument()) != 0) {
                    markStale("Object change");
                }
            }
        ));
        connections.push_back(doc->signalDeletedObject.connect([this](const App::DocumentObject& obj) {
            if (!assembly) {
                return;
            }
            if (&obj == assembly) {
                assembly = nullptr;
                closeManageExclusionsDialog();
                disconnectDocumentSignals();
                markStale("Assembly deleted");
                setScanControlsEnabled(false);
                return;
            }
            if (watchedDocuments.count(obj.getDocument()) != 0) {
                markStale("Object deleted");
            }
        }));
    };

    attachDocument(assembly->getDocument());
    for (App::DocumentObject* obj : assembly->getOutListRecursive()) {
        if (obj) {
            attachDocument(obj->getDocument());
        }
    }

    connections.push_back(App::GetApplication().signalDeleteDocument.connect(
        [this](const App::Document& deleted) {
            if (!assembly) {
                return;
            }
            if (assembly->getDocument() == &deleted) {
                assembly = nullptr;
                closeManageExclusionsDialog();
                disconnectDocumentSignals();
                markStale("Document closed");
                setScanControlsEnabled(false);
                return;
            }
            if (watchedDocuments.count(&deleted) != 0) {
                closeManageExclusionsDialog();
                markStale("Linked document closed");
            }
        }
    ));
}

void TaskInterferenceCheck::disconnectDocumentSignals()
{
    for (auto& conn : connections) {
        conn.disconnect();
    }
    connections.clear();
    watchedDocuments.clear();
}

void TaskInterferenceCheck::markStale(const char* reason)
{
    session.markStale();
    discardResults();
    if (statusLabel) {
        statusLabel->setText(tr("Results stale (%1). Run again.").arg(QString::fromUtf8(reason)));
    }
    // Keep cancel enabled while a superseded worker is still busy.
    if (cancelButton) {
        cancelButton->setEnabled(session.isBusy());
    }
    if (runButton) {
        runButton->setEnabled(assembly != nullptr);
    }
    updateRowActionState();
}

void TaskInterferenceCheck::onRun()
{
    if (!assembly) {
        return;
    }

    // Refresh involved-document watchers in case links were added after open.
    disconnectDocumentSignals();
    connectDocumentSignals();

    // Allow a new generation while a previous worker is still finishing.
    if (!attachedViewer || !previewRoot) {
        attachPreviewToViewer();
    }

    const double clearance = clearanceSpin->rawValue();
    if (!clearanceSpin->hasValidInput() || clearance < 0.0 || !std::isfinite(clearance)) {
        QMessageBox::warning(this, tr("Invalid clearance"), tr("Clearance must be nonnegative."));
        return;
    }

    if (std::abs(assembly->getInterferenceClearance() - clearance) > 1e-12) {
        if (auto* guiDoc = assemblyGuiDocument()) {
            guiDoc->openCommand("Set interference clearance");
            assembly->setInterferenceClearance(clearance);
            guiDoc->commitCommand();
        }
        else {
            assembly->setInterferenceClearance(clearance);
        }
    }

    auto handle = session.beginScan();
    setScanControlsEnabled(false);
    cancelButton->setEnabled(true);
    statusLabel->setText(tr("Scanning…"));
    progressLabel->setText(tr("Progress: starting…"));
    clearPreview();
    lastResult = {};
    rebuildTable();

    const bool includeHidden = includeHiddenCheck->isChecked();
    auto leaves = collectInterferenceLeaves(assembly, includeHidden);
    auto excluded = currentExclusionSourceIds();
    auto cancel = handle.cancel;
    const auto generation = handle.generation;
    QPointer<TaskInterferenceCheck> self(this);

    auto* watcher = new QFutureWatcher<InterferenceScanResult>(this);
    connect(
        watcher,
        &QFutureWatcher<InterferenceScanResult>::finished,
        this,
        [self, watcher, generation]() {
            watcher->deleteLater();
            if (!self) {
                return;
            }
            InterferenceScanResult result;
            if (watcher->future().isFinished()) {
                result = watcher->result();
            }
            // Generation ownership is decided only inside finishScan / onScanFinished.
            self->onScanFinished(generation, result);
        }
    );

    QFuture<InterferenceScanResult> future = QtConcurrent::run(
        [leaves = std::move(leaves),
         excluded = std::move(excluded),
         clearance,
         cancel,
         self,
         generation]() mutable {
            InterferenceScanOptions options;
            options.clearance = clearance;
            options.cancelFlag = cancel.get();
            options.detectionOptions.clearance = clearance;
            options.detectionOptions.cancelFlag = cancel.get();
            options.progress = [self, generation](int current, int total) {
                if (!self) {
                    return;
                }
                QMetaObject::invokeMethod(
                    self,
                    [self, generation, current, total]() {
                        if (!self || generation != self->session.activeGeneration()) {
                            return;
                        }
                        self->onScanProgress(current, total);
                    },
                    Qt::QueuedConnection
                );
            };
            return runInterferenceScan(leaves, options, excluded);
        }
    );
    watcher->setFuture(future);
}

void TaskInterferenceCheck::onCancelScan()
{
    session.requestCancel();
    clearPreview();
    if (session.isBusy()) {
        statusLabel->setText(tr("Cancelling…"));
    }
    else {
        discardResults();
        statusLabel->setText(tr("Scan cancelled."));
    }
}

void TaskInterferenceCheck::onScanProgress(int current, int total)
{
    if (progressLabel) {
        progressLabel->setText(tr("Progress: %1 / %2").arg(current).arg(total));
    }
}

void TaskInterferenceCheck::onScanFinished(
    std::uint64_t generation,
    const InterferenceScanResult& result
)
{
    // Superseded generations must return before any shared UI/result mutation,
    // including after a newer scan has already completed (B-then-A ordering).
    if (generation != session.activeGeneration()) {
        return;
    }

    if (!session.finishScan(generation)) {
        cancelButton->setEnabled(false);
        setScanControlsEnabled(assembly != nullptr);
        if (session.isStale()) {
            statusLabel->setText(tr("Scan finished but results are stale. Run again."));
        }
        else {
            statusLabel->setText(tr("Scan cancelled."));
            discardResults();
        }
        updateRowActionState();
        return;
    }

    cancelButton->setEnabled(false);
    setScanControlsEnabled(assembly != nullptr);
    lastResult = result;

    if (lastResult.cancelled) {
        statusLabel->setText(tr("Scan cancelled."));
        discardResults();
        return;
    }

    statusLabel->setText(lastResult.complete ? tr("Scan complete.") : tr("Scan incomplete."));
    progressLabel->clear();
    rebuildTable();
    updateSummary();
    updateRowActionState();
}

void TaskInterferenceCheck::rebuildTable()
{
    resultsTable->setRowCount(0);
    const bool showExcluded = showExcludedCheck->isChecked();
    for (std::size_t i = 0; i < lastResult.pairs.size(); ++i) {
        const auto& pair = lastResult.pairs[i];
        if (pair.excluded && !showExcluded) {
            continue;
        }
        if (pair.detection.kind == Part::InterferenceKind::Clear) {
            continue;
        }
        const int row = resultsTable->rowCount();
        resultsTable->insertRow(row);
        auto* statusItem = new QTableWidgetItem(kindText(pair.detection.kind, pair.excluded));
        statusItem->setData(Qt::UserRole, static_cast<int>(i));
        resultsTable->setItem(row, 0, statusItem);
        resultsTable->setItem(
            row,
            1,
            new QTableWidgetItem(QString::fromStdString(lastResult.leaves[pair.leafIndexA].displayPath))
        );
        resultsTable->setItem(
            row,
            2,
            new QTableWidgetItem(QString::fromStdString(lastResult.leaves[pair.leafIndexB].displayPath))
        );
        resultsTable->setItem(row, 3, new QTableWidgetItem(formatPairDistance(pair.detection)));
        resultsTable->setItem(row, 4, new QTableWidgetItem(formatPairVolume(pair.detection)));
    }

    for (const auto& issue : lastResult.componentIssues) {
        const int row = resultsTable->rowCount();
        resultsTable->insertRow(row);
        auto* statusItem = new QTableWidgetItem(tr("Invalid leaf"));
        statusItem->setData(Qt::UserRole, -1);
        resultsTable->setItem(row, 0, statusItem);
        resultsTable->setItem(
            row,
            1,
            new QTableWidgetItem(QString::fromStdString(lastResult.leaves[issue.leafIndex].displayPath))
        );
        resultsTable->setItem(row, 2, new QTableWidgetItem(QString()));
        resultsTable->setItem(row, 3, new QTableWidgetItem(QString()));
        resultsTable->setItem(row, 4, new QTableWidgetItem(QString::fromStdString(issue.diagnostic)));
    }
    updateRowActionState();
}

void TaskInterferenceCheck::updateSummary()
{
    const auto& c = lastResult.counts;
    summaryLabel->setText(
        tr("Penetrations: %1 | Contacts: %2 | Clearance: %3 | Excluded: %4 | Invalid: %5 | "
           "Inconclusive: %6")
            .arg(c.penetrations)
            .arg(c.contacts)
            .arg(c.clearanceViolations)
            .arg(c.excludedViolations)
            .arg(c.invalidInputs)
            .arg(c.inconclusivePairs)
    );
}

void TaskInterferenceCheck::onShowExcludedToggled(bool)
{
    rebuildTable();
}

void TaskInterferenceCheck::onIncludeHiddenToggled(bool)
{
    markStale("Include-hidden changed");
}

void TaskInterferenceCheck::onRowChanged()
{
    updatePreviewForCurrentRow();
    updateRowActionState();
}

void TaskInterferenceCheck::clearPreview()
{
    if (previewRoot) {
        previewRoot->removeAllChildren();
        auto* pick = new SoPickStyle;
        pick->style = SoPickStyle::UNPICKABLE;
        previewRoot->addChild(pick);
    }
}

void TaskInterferenceCheck::updatePreviewForCurrentRow()
{
    clearPreview();
    if (!previewRoot || currentPairIndex() < 0) {
        return;
    }

    const auto& pair = lastResult.pairs[static_cast<std::size_t>(currentPairIndex())];
    const auto& leafA = lastResult.leaves[pair.leafIndexA];
    const auto& leafB = lastResult.leaves[pair.leafIndexB];
    const auto color = colorForKind(pair.detection.kind);

    auto addPreviewShape = [&](const TopoDS_Shape& shape, float transparency) {
        if (shape.IsNull()) {
            return;
        }
        auto* node = new PartGui::SoPreviewShape;
        node->color.setValue(color);
        node->transparency.setValue(transparency);
        try {
            // Capture placement before setupCoinGeometry strips TopLoc_Location.
            const Part::TopoShape topo(shape);
            const SbMatrix transform = Base::convertTo<SbMatrix>(topo.getTransform());
            // Deep-copy so BRepTools::Clean / remesh cannot mutate shared document geometry.
            const TopoDS_Shape previewShape =
                BRepBuilderAPI_Copy(shape, Standard_True, Standard_False).Shape();
            PartGui::ViewProviderPartExt::setupCoinGeometry(previewShape, node, 0.5, 28.65);
            node->transform.setValue(transform);
        }
        catch (...) {
            // Keep the node so markers still show even if mesh conversion fails.
        }
        previewRoot->addChild(node);
    };

    if (!pair.detection.commonShape.IsNull()) {
        addPreviewShape(pair.detection.commonShape, 0.45F);
    }

    if (hasValidClosestPoints(pair.detection)) {
        auto* coords = new SoCoordinate3;
        const SbVec3f p1(
            static_cast<float>(pair.detection.pointOnFirst.x),
            static_cast<float>(pair.detection.pointOnFirst.y),
            static_cast<float>(pair.detection.pointOnFirst.z)
        );
        const SbVec3f p2(
            static_cast<float>(pair.detection.pointOnSecond.x),
            static_cast<float>(pair.detection.pointOnSecond.y),
            static_cast<float>(pair.detection.pointOnSecond.z)
        );
        coords->point.setNum(2);
        coords->point.set1Value(0, p1);
        coords->point.set1Value(1, p2);

        auto* material = new SoMaterial;
        material->diffuseColor.setValue(color);
        material->emissiveColor.setValue(color);

        auto* style = new SoDrawStyle;
        style->lineWidth = 3;
        style->pointSize = 10;

        auto* points = new SoPointSet;
        auto* lines = new SoLineSet;
        lines->numVertices.set1Value(0, 2);

        previewRoot->addChild(material);
        previewRoot->addChild(style);
        previewRoot->addChild(coords);
        previewRoot->addChild(points);
        previewRoot->addChild(lines);
    }

    addPreviewShape(leafA.worldShape, 0.85F);
    addPreviewShape(leafB.worldShape, 0.85F);
}

void TaskInterferenceCheck::onSelectPair()
{
    const int pairIndex = currentPairIndex();
    if (pairIndex < 0 || !assembly || !assembly->isAttachedToDocument()) {
        return;
    }
    const auto& pair = lastResult.pairs[static_cast<std::size_t>(pairIndex)];
    const auto& leafA = lastResult.leaves[pair.leafIndexA];
    const auto& leafB = lastResult.leaves[pair.leafIndexB];
    Gui::Selection().clearSelection();
    const char* docName = assembly->getDocument()->getName();
    const char* asmName = assembly->getNameInDocument();
    if (!leafA.occurrenceSubName.empty()) {
        Gui::Selection().addSelection(docName, asmName, leafA.occurrenceSubName.c_str());
    }
    if (!leafB.occurrenceSubName.empty()) {
        Gui::Selection().addSelection(docName, asmName, leafB.occurrenceSubName.c_str());
    }
}

void TaskInterferenceCheck::onExcludePair()
{
    const int pairIndex = currentPairIndex();
    if (pairIndex < 0 || !assembly) {
        return;
    }
    const auto& pair = lastResult.pairs[static_cast<std::size_t>(pairIndex)];
    auto* sourceA = resolveSourceId(lastResult.leaves[pair.leafIndexA].sourceId);
    auto* sourceB = resolveSourceId(lastResult.leaves[pair.leafIndexB].sourceId);
    if (!sourceA || !sourceB) {
        return;
    }

    int affected = 0;
    const auto& idA = lastResult.leaves[pair.leafIndexA].sourceId;
    const auto& idB = lastResult.leaves[pair.leafIndexB].sourceId;
    for (const auto& other : lastResult.pairs) {
        if (!isViolationKind(other.detection.kind)) {
            continue;
        }
        const auto& a = lastResult.leaves[other.leafIndexA].sourceId;
        const auto& b = lastResult.leaves[other.leafIndexB].sourceId;
        if ((a == idA && b == idB) || (a == idB && b == idA)) {
            ++affected;
        }
    }

    const auto answer = QMessageBox::question(
        this,
        tr("Exclude source pair"),
        tr("Exclude sources '%1' and '%2' for all occurrences?\nThis currently affects %3 "
           "violation row(s).")
            .arg(QString::fromUtf8(sourceA->Label.getValue()))
            .arg(QString::fromUtf8(sourceB->Label.getValue()))
            .arg(affected)
    );
    if (answer != QMessageBox::Yes) {
        return;
    }

    if (auto* guiDoc = assemblyGuiDocument()) {
        guiDoc->openCommand("Exclude interference source pair");
        assembly->addInterferenceExclusion(sourceA, sourceB);
        guiDoc->commitCommand();
    }
    else {
        assembly->addInterferenceExclusion(sourceA, sourceB);
    }
    onRun();
}

void TaskInterferenceCheck::onRestorePair()
{
    const int pairIndex = currentPairIndex();
    if (pairIndex < 0 || !assembly) {
        return;
    }
    const auto& pair = lastResult.pairs[static_cast<std::size_t>(pairIndex)];
    auto* sourceA = resolveSourceId(lastResult.leaves[pair.leafIndexA].sourceId);
    auto* sourceB = resolveSourceId(lastResult.leaves[pair.leafIndexB].sourceId);
    if (!sourceA || !sourceB) {
        return;
    }
    if (auto* guiDoc = assemblyGuiDocument()) {
        guiDoc->openCommand("Restore interference source pair");
        assembly->removeInterferenceExclusion(sourceA, sourceB);
        guiDoc->commitCommand();
    }
    else {
        assembly->removeInterferenceExclusion(sourceA, sourceB);
    }
    onRun();
}

void TaskInterferenceCheck::onManageExclusions()
{
    if (!assembly) {
        return;
    }

    closeManageExclusionsDialog();

    auto* dialog = new QDialog(this);
    manageExclusionsDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Manage exclusions"));
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(
        tr("Source-wide exclusion rules. Dormant rules have no current violation; invalid rules "
           "remain reviewable."),
        dialog
    ));

    auto* table = new QTableWidget(0, 4, dialog);
    table->setHorizontalHeaderLabels(
        {tr("Status"), tr("Source A"), tr("Source B"), tr("Affected violations")}
    );
    table->horizontalHeader()->setStretchLastSection(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto labelOf = [](const Assembly::InterferenceExclusionRule& rule, bool first) -> QString {
        App::DocumentObject* obj = first ? rule.first : rule.second;
        const std::string& identity = first ? rule.firstIdentity : rule.secondIdentity;
        if (obj && obj->isAttachedToDocument()) {
            return QString::fromUtf8(obj->Label.getValue());
        }
        if (!identity.empty()) {
            return QObject::tr("<unresolved: %1>").arg(QString::fromStdString(identity));
        }
        if (obj) {
            return QObject::tr("<deleted>");
        }
        return QObject::tr("<unresolved>");
    };

    auto fillTable = [this, table, labelOf]() {
        table->setRowCount(0);
        if (!assembly) {
            return;
        }
        const auto rules = assembly->getInterferenceExclusionRules();
        for (std::size_t i = 0; i < rules.size(); ++i) {
            const auto& rule = rules[i];
            QString status = tr("Dormant");
            int affected = 0;
            if (!rule.valid) {
                status = tr("Invalid");
            }
            else {
                const std::string idA = rule.firstIdentity;
                const std::string idB = rule.secondIdentity;
                for (const auto& pair : lastResult.pairs) {
                    if (!pair.excluded) {
                        continue;
                    }
                    const auto& a = lastResult.leaves[pair.leafIndexA].sourceId;
                    const auto& b = lastResult.leaves[pair.leafIndexB].sourceId;
                    if ((a == idA && b == idB) || (a == idB && b == idA)) {
                        ++affected;
                    }
                }
                if (affected > 0) {
                    status = tr("Active");
                }
            }
            const int row = table->rowCount();
            table->insertRow(row);
            auto* statusItem = new QTableWidgetItem(status);
            statusItem->setData(Qt::UserRole, static_cast<int>(i));
            table->setItem(row, 0, statusItem);
            table->setItem(row, 1, new QTableWidgetItem(labelOf(rule, true)));
            table->setItem(row, 2, new QTableWidgetItem(labelOf(rule, false)));
            table->setItem(row, 3, new QTableWidgetItem(QString::number(affected)));
        }
    };
    fillTable();
    layout->addWidget(table);

    auto* buttons = new QHBoxLayout;
    auto* restoreBtn = new QPushButton(tr("Restore source pair"), dialog);
    auto* removeBtn = new QPushButton(tr("Remove rule"), dialog);
    buttons->addWidget(restoreBtn);
    buttons->addWidget(removeBtn);
    buttons->addStretch();
    layout->addLayout(buttons);

    auto selectedRuleIndex = [table]() -> int {
        if (table->currentRow() < 0) {
            return -1;
        }
        auto* item = table->item(table->currentRow(), 0);
        return item ? item->data(Qt::UserRole).toInt() : -1;
    };

    connect(restoreBtn, &QPushButton::clicked, dialog, [this, dialog, selectedRuleIndex, fillTable]() {
        if (!assembly) {
            dialog->reject();
            return;
        }
        const int index = selectedRuleIndex();
        auto rules = assembly->getInterferenceExclusionRules();
        if (index < 0 || index >= static_cast<int>(rules.size())) {
            return;
        }
        auto& rule = rules[static_cast<std::size_t>(index)];
        if (!rule.valid || !rule.first || !rule.second) {
            return;
        }
        if (auto* guiDoc = assemblyGuiDocument()) {
            guiDoc->openCommand("Restore interference source pair");
            assembly->removeInterferenceExclusion(rule.first, rule.second);
            guiDoc->commitCommand();
        }
        else {
            assembly->removeInterferenceExclusion(rule.first, rule.second);
        }
        fillTable();
    });

    connect(removeBtn, &QPushButton::clicked, dialog, [this, dialog, selectedRuleIndex, fillTable]() {
        if (!assembly) {
            dialog->reject();
            return;
        }
        const int index = selectedRuleIndex();
        if (index < 0) {
            return;
        }
        if (auto* guiDoc = assemblyGuiDocument()) {
            guiDoc->openCommand("Remove interference exclusion rule");
            assembly->removeInterferenceExclusionAt(static_cast<std::size_t>(index));
            guiDoc->commitCommand();
        }
        else {
            assembly->removeInterferenceExclusionAt(static_cast<std::size_t>(index));
        }
        fillTable();
    });

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(box, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(box, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    layout->addWidget(box);
    dialog->exec();
    if (manageExclusionsDialog == dialog) {
        manageExclusionsDialog.clear();
    }
}

TaskInterferenceCheckDialog::TaskInterferenceCheckDialog(AssemblyObject* assembly)
{
    widget = new TaskInterferenceCheck(assembly);
    taskbox = new Gui::TaskView::TaskBox(
        Gui::BitmapFactory().pixmap("Assembly_CheckInterference"),
        tr("Check Interference"),
        true,
        nullptr
    );
    taskbox->groupLayout()->addWidget(widget);
    Content.push_back(taskbox);
}

TaskInterferenceCheckDialog::~TaskInterferenceCheckDialog() = default;

bool TaskInterferenceCheckDialog::accept()
{
    return widget->accept();
}

bool TaskInterferenceCheckDialog::reject()
{
    return widget->reject();
}
