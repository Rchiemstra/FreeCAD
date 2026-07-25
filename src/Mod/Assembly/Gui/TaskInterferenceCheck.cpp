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
#include <functional>

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

TaskInterferenceCheck::TaskInterferenceCheck(App::DocumentObject* host, QWidget* parent)
    : TaskInterferenceCheck(host, {}, {}, parent)
{}

TaskInterferenceCheck::TaskInterferenceCheck(
    App::DocumentObject* host,
    const InterferenceComponentOccurrence& componentA,
    const InterferenceComponentOccurrence& componentB,
    QWidget* parent
)
    : QWidget(parent)
    , host(host)
    , selectedA(componentA)
    , selectedB(componentB)
    , selectedComponentsMode(
          componentA.component != nullptr && componentB.component != nullptr
          && !componentA.occurrencePrefix.empty() && !componentB.occurrencePrefix.empty()
          && componentA.occurrencePrefix != componentB.occurrencePrefix
      )
    , scopeLockedToSelection(selectedComponentsMode)
{
    setupUi();
    connectDocumentSignals();

    if (host) {
        clearanceSpin->setValue(
            Base::Quantity(Assembly::getInterferenceClearance(host), Base::Unit::Length)
        );
    }

    refreshScanScope();
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

    statusLabel = new QLabel(tr("Ready."), this);
    layout->addWidget(statusLabel);
    scopeLabel = new QLabel(tr("Scan scope: —"), this);
    scopeLabel->setWordWrap(true);
    layout->addWidget(scopeLabel);
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

void TaskInterferenceCheck::testEnsureDetachedPreviewRoot()
{
    if (previewRoot) {
        return;
    }
    previewRoot = new SoSeparator;
    previewRoot->ref();
    auto* pick = new SoPickStyle;
    pick->style = SoPickStyle::UNPICKABLE;
    previewRoot->addChild(pick);
}

bool TaskInterferenceCheck::testHasPreviewRoot() const
{
    return previewRoot != nullptr;
}

void TaskInterferenceCheck::testSelectResultRow(int row)
{
    if (!resultsTable || row < 0 || row >= resultsTable->rowCount()) {
        return;
    }
    resultsTable->selectRow(row);
    onRowChanged();
}

int TaskInterferenceCheck::testPreviewShapeCount() const
{
    if (!previewRoot) {
        return 0;
    }
    int count = 0;
    for (int i = 0; i < previewRoot->getNumChildren(); ++i) {
        if (dynamic_cast<PartGui::SoPreviewShape*>(previewRoot->getChild(i))) {
            ++count;
        }
    }
    return count;
}

bool TaskInterferenceCheck::testPreviewShapeTranslation(
    int shapeIndex,
    double& x,
    double& y,
    double& z
) const
{
    if (!previewRoot || shapeIndex < 0) {
        return false;
    }
    int seen = -1;
    for (int i = 0; i < previewRoot->getNumChildren(); ++i) {
        auto* shape = dynamic_cast<PartGui::SoPreviewShape*>(previewRoot->getChild(i));
        if (!shape) {
            continue;
        }
        ++seen;
        if (seen != shapeIndex) {
            continue;
        }
        SbVec3f translation;
        SbRotation rotation;
        SbVec3f scale;
        SbRotation scaleOrientation;
        shape->transform.getValue().getTransform(translation, rotation, scale, scaleOrientation);
        x = translation[0];
        y = translation[1];
        z = translation[2];
        return true;
    }
    return false;
}

void TaskInterferenceCheck::testOpenManageExclusions()
{
    onManageExclusions();
}

bool TaskInterferenceCheck::testManageExclusionsOpen() const
{
    return manageExclusionsDialog != nullptr;
}

bool TaskInterferenceCheck::testHasHost() const
{
    return host != nullptr;
}

QString TaskInterferenceCheck::testScopeText() const
{
    return scopeLabel ? scopeLabel->text() : QString();
}

bool TaskInterferenceCheck::testIsSelectedPairMode() const
{
    return selectedComponentsMode;
}

void TaskInterferenceCheck::testRefreshScanScope()
{
    refreshScanScope();
}

void TaskInterferenceCheck::testSetSelectionHandles(
    std::vector<InterferenceSelectionHandle> handles
)
{
    testSelectionHandles = std::move(handles);
    hasTestSelectionOverride = true;
}

void TaskInterferenceCheck::testClearSelectionHandles()
{
    testSelectionHandles.clear();
    hasTestSelectionOverride = false;
}

void TaskInterferenceCheck::testNotifySelectionChanged()
{
    onSelectionChanged();
}

void TaskInterferenceCheck::testSetPreparationBarrier(std::function<void()> barrier)
{
    testPreparationBarrierFn = std::move(barrier);
}

void TaskInterferenceCheck::testClearPreparationBarrier()
{
    testPreparationBarrierFn = {};
}

void TaskInterferenceCheck::testSetIncludeHidden(bool enabled)
{
    if (includeHiddenCheck) {
        includeHiddenCheck->setChecked(enabled);
    }
}

void TaskInterferenceCheck::testRunScan()
{
    onRun();
}

bool TaskInterferenceCheck::testIsPreparing() const
{
    return preparingScan;
}

bool TaskInterferenceCheck::testIsCancelEnabled() const
{
    return cancelButton && cancelButton->isEnabled();
}

void TaskInterferenceCheck::testAttachPreviewToScene(SoGroup* scene)
{
    attachPreviewToScene(scene);
}

void TaskInterferenceCheck::testDetachPreview()
{
    detachPreviewFromViewer();
}

int TaskInterferenceCheck::testPreviewIndexInScene(SoGroup* scene) const
{
    if (!scene || !previewRoot) {
        return -1;
    }
    return scene->findChild(previewRoot);
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

Gui::Document* TaskInterferenceCheck::hostGuiDocument() const
{
    if (!host || !host->getDocument() || !Gui::Application::Instance) {
        return nullptr;
    }
    return Gui::Application::Instance->getDocument(host->getDocument());
}

std::vector<std::pair<std::string, std::string>>
TaskInterferenceCheck::currentExclusionSourceIds() const
{
    std::vector<std::pair<std::string, std::string>> excluded;
    if (!host) {
        return excluded;
    }
    auto objectKey = [](const App::DocumentObject* obj) -> std::string {
        if (!obj || !obj->isAttachedToDocument()) {
            return {};
        }
        return std::string(obj->getDocument()->getName()) + "#" + obj->getNameInDocument();
    };
    for (const auto& rule : Assembly::getInterferenceExclusionRules(host)) {
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
    auto* scene = dynamic_cast<SoGroup*>(view->getSceneGraph());
    if (!scene) {
        return;
    }
    attachPreviewToScene(scene, view, viewWin);
}

void TaskInterferenceCheck::attachPreviewToScene(
    SoGroup* scene,
    Gui::View3DInventorViewer* view,
    Gui::View3DInventor* viewWin
)
{
    detachPreviewFromViewer();
    if (!scene) {
        return;
    }
    previewRoot = new SoSeparator;
    previewRoot->ref();
    auto* pick = new SoPickStyle;
    pick->style = SoPickStyle::UNPICKABLE;
    previewRoot->addChild(pick);
    scene->addChild(previewRoot);
    attachedScene = scene;
    attachedViewer = view;
    attachedView = viewWin;
    if (viewWin) {
        connect(viewWin, &QObject::destroyed, this, [this]() {
            attachedViewer = nullptr;
            attachedView.clear();
            attachedScene = nullptr;
            if (previewRoot) {
                previewRoot->unref();
                previewRoot = nullptr;
            }
            markStale("View closed");
        });
    }
}

void TaskInterferenceCheck::detachPreviewFromViewer()
{
    clearPreview();
    if (previewRoot && attachedScene) {
        const int idx = attachedScene->findChild(previewRoot);
        if (idx >= 0) {
            attachedScene->removeChild(idx);
        }
    }
    if (previewRoot) {
        previewRoot->unref();
        previewRoot = nullptr;
    }
    attachedScene = nullptr;
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
        manageExclusionsButton->setEnabled(enabled && host);
    }
    updateRowActionState();
}

void TaskInterferenceCheck::updateRowActionState()
{
    const bool idle = !session.isBusy();
    const int pairIndex = currentPairIndex();
    const bool hasPair = idle && pairIndex >= 0 && host;
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
    if (!host || !host->getDocument()) {
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
                if (!host || !isResultAffectingProperty(prop)) {
                    return;
                }
                if (&obj == host || watchedDocuments.count(obj.getDocument()) != 0) {
                    markStale("Object change");
                }
            }
        ));
        connections.push_back(doc->signalDeletedObject.connect([this](const App::DocumentObject& obj) {
            if (!host) {
                return;
            }
            if (&obj == host) {
                host = nullptr;
                closeManageExclusionsDialog();
                disconnectDocumentSignals();
                markStale("Host deleted");
                setScanControlsEnabled(false);
                return;
            }
            if (watchedDocuments.count(obj.getDocument()) != 0) {
                markStale("Object deleted");
            }
        }));
    };

    attachDocument(host->getDocument());
    for (App::DocumentObject* obj : host->getOutListRecursive()) {
        if (obj) {
            attachDocument(obj->getDocument());
        }
    }

    connections.push_back(App::GetApplication().signalDeleteDocument.connect(
        [this](const App::Document& deleted) {
            if (!host) {
                return;
            }
            if (host->getDocument() == &deleted) {
                host = nullptr;
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

    connections.push_back(Gui::Selection().signalSelectionChanged.connect(
        [this](const Gui::SelectionChanges&) {
            onSelectionChanged();
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

void TaskInterferenceCheck::onSelectionChanged()
{
    if (scopeLockedToSelection) {
        return;
    }
    if (session.isBusy()) {
        // Defer: when the active generation finishes, refreshScanScope syncs to the
        // current selection (locked dialogs never follow global selection).
        selectionDirtyWhileBusy = true;
        return;
    }
    refreshScanScope();
}

void TaskInterferenceCheck::refreshScanScope()
{
    if (!scopeLabel) {
        return;
    }
    if (!host) {
        selectedComponentsMode = false;
        scopeLabel->setText(tr("Scan scope: no active part."));
        if (statusLabel && !session.isBusy()) {
            statusLabel->setText(tr("Select an App::Part or activate an assembly."));
        }
        return;
    }

    const bool includeHidden = includeHiddenCheck && includeHiddenCheck->isChecked();

    if (!scopeLockedToSelection) {
        selectedComponentsMode = false;
        selectedA = {};
        selectedB = {};

        std::vector<InterferenceSelectionHandle> handles;
        if (hasTestSelectionOverride) {
            handles = testSelectionHandles;
        }
        else {
            auto selection = Gui::Selection().getSelectionEx(
                "",
                App::DocumentObject::getClassTypeId(),
                Gui::ResolveMode::NoResolve
            );
            for (auto& sel : selection) {
                App::DocumentObject* obj = sel.getObject();
                if (!obj) {
                    continue;
                }
                const auto subs = sel.getSubNames();
                if (subs.empty()) {
                    handles.push_back({obj, {}});
                    continue;
                }
                for (const auto& sub : subs) {
                    handles.push_back({obj, sub});
                }
            }
        }

        const auto scope = resolveInterferenceSelectionScope(host, handles);
        if (scope.mode == InterferenceScanScopeMode::SelectedPair) {
            selectedA = scope.first;
            selectedB = scope.second;
            selectedComponentsMode = true;
        }
    }

    if (selectedComponentsMode) {
        scopeLabel->setText(
            tr("Scan scope: selected pair — '%1' ↔ '%2' (faces are pick handles; complete "
               "occurrences including nested solids).")
                .arg(QString::fromStdString(selectedA.displayPath))
                .arg(QString::fromStdString(selectedB.displayPath))
        );
        if (statusLabel && !session.isBusy() && !hasResults()) {
            statusLabel->setText(tr("Ready to check the selected component pair."));
        }
        return;
    }

    const auto components = listInterferenceComponentOccurrences(host, includeHidden);
    if (components.empty()) {
        scopeLabel->setText(
            includeHidden ? tr("Scan scope: no component occurrences under this part.")
                          : tr("Scan scope: no visible component occurrences (enable Include "
                               "hidden to include hidden components).")
        );
    }
    else {
        QStringList names;
        names.reserve(static_cast<int>(components.size()));
        for (const auto& occ : components) {
            names << QString::fromStdString(occ.displayPath);
        }
        QString listed = names.join(QStringLiteral(", "));
        constexpr int maxLen = 180;
        if (listed.size() > maxLen) {
            listed = listed.left(maxLen - 1) + QChar(0x2026);
        }
        scopeLabel->setText(
            tr("Scan scope: all %1 %2 component occurrence(s) — %3")
                .arg(components.size())
                .arg(includeHidden ? tr("components (including hidden)") : tr("visible"))
                .arg(listed)
        );
    }
    if (statusLabel && !session.isBusy() && !hasResults()) {
        statusLabel->setText(
            tr("Ready to check all listed component occurrences under the active part.")
        );
    }
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
        runButton->setEnabled(host != nullptr);
    }
    updateRowActionState();
}

void TaskInterferenceCheck::onRun()
{
    if (!host) {
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

    if (std::abs(Assembly::getInterferenceClearance(host) - clearance) > 1e-12) {
        if (auto* guiDoc = hostGuiDocument()) {
            guiDoc->openCommand("Set interference clearance");
            Assembly::setInterferenceClearance(host, clearance);
            guiDoc->commitCommand();
        }
        else {
            Assembly::setInterferenceClearance(host, clearance);
        }
    }

    auto handle = session.beginScan();
    setScanControlsEnabled(false);
    // DocumentObject-backed extraction blocks the GUI thread; Cancel cannot
    // interrupt this phase. Advertise Preparing distinctly from Scanning.
    preparingScan = true;
    cancelButton->setEnabled(false);
    statusLabel->setText(tr("Preparing scan…"));
    progressLabel->setText(tr("Progress: preparing geometry…"));
    clearPreview();
    lastResult = {};
    rebuildTable();

    refreshScanScope();
    const bool includeHidden = includeHiddenCheck->isChecked();
    auto excluded = currentExclusionSourceIds();
    auto cancel = handle.cancel;
    const auto generation = handle.generation;
    QPointer<TaskInterferenceCheck> self(this);

    InterferenceScanOptions prepOptions;
    prepOptions.clearance = clearance;
    prepOptions.cancelFlag = cancel.get();
    prepOptions.detectionOptions.clearance = clearance;
    prepOptions.detectionOptions.cancelFlag = cancel.get();

    if (!host || !host->isAttachedToDocument()) {
        preparingScan = false;
        session.requestCancel();
        (void)session.finishScan(generation);
        discardResults();
        cancelButton->setEnabled(false);
        setScanControlsEnabled(false);
        if (statusLabel) {
            statusLabel->setText(tr("Scan aborted (host unavailable)."));
        }
        return;
    }

    // Snapshot DocumentObject-backed geometry on this thread before the worker.
    // Optional test barrier runs mid-preparation (after occurrence listing).
    auto snapshot = prepareInterferenceComponentScanSnapshot(
        host,
        includeHidden,
        prepOptions,
        testPreparationBarrierFn
    );
    preparingScan = false;
    if (!host || !host->isAttachedToDocument() || snapshot.cancelled
        || (cancel && cancel->load(std::memory_order_relaxed))) {
        // finishScan clears busy; false means cancelled/stale — still clean up UI.
        (void)session.finishScan(generation);
        discardResults();
        cancelButton->setEnabled(false);
        setScanControlsEnabled(host != nullptr);
        if (statusLabel) {
            statusLabel->setText(tr("Scan cancelled."));
        }
        if (selectionDirtyWhileBusy && !scopeLockedToSelection) {
            selectionDirtyWhileBusy = false;
            refreshScanScope();
        }
        return;
    }

    const bool betweenSelected = selectedComponentsMode;
    std::vector<InterferenceLeaf> leavesA;
    std::vector<InterferenceLeaf> leavesB;
    InterferenceComponentScanSnapshot acrossSnapshot;
    if (betweenSelected) {
        for (std::size_t i = 0; i < snapshot.leaves.size(); ++i) {
            const auto& leaf = snapshot.leaves[i];
            if (leaf.occurrenceSubName.rfind(selectedA.occurrencePrefix, 0) == 0) {
                leavesA.push_back(leaf);
            }
            else if (leaf.occurrenceSubName.rfind(selectedB.occurrencePrefix, 0) == 0) {
                leavesB.push_back(leaf);
            }
        }
        if (statusLabel) {
            statusLabel->setText(
                tr("Scanning selected components '%1' and '%2'…")
                    .arg(QString::fromStdString(selectedA.displayPath))
                    .arg(QString::fromStdString(selectedB.displayPath))
            );
        }
    }
    else {
        acrossSnapshot = std::move(snapshot);
        if (statusLabel) {
            statusLabel->setText(tr("Scanning all applicable component occurrences…"));
        }
    }

    // Worker phase: Cancel is meaningful for pair classification.
    cancelButton->setEnabled(true);
    if (progressLabel) {
        progressLabel->setText(tr("Progress: starting…"));
    }

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
            self->onScanFinished(generation, result);
        }
    );

    QFuture<InterferenceScanResult> future = QtConcurrent::run(
        [leavesA = std::move(leavesA),
         leavesB = std::move(leavesB),
         acrossSnapshot = std::move(acrossSnapshot),
         betweenSelected,
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
            if (betweenSelected) {
                return runInterferenceScanBetweenLeafSets(leavesA, leavesB, options, excluded);
            }
            return runInterferenceScanAcrossComponents(acrossSnapshot, options, excluded);
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
        setScanControlsEnabled(host != nullptr);
        if (session.isStale()) {
            statusLabel->setText(tr("Scan finished but results are stale. Run again."));
        }
        else {
            statusLabel->setText(tr("Scan cancelled."));
            discardResults();
        }
        updateRowActionState();
        if (selectionDirtyWhileBusy && !scopeLockedToSelection) {
            selectionDirtyWhileBusy = false;
            refreshScanScope();
        }
        return;
    }

    cancelButton->setEnabled(false);
    setScanControlsEnabled(host != nullptr);
    lastResult = result;

    if (lastResult.cancelled) {
        statusLabel->setText(tr("Scan cancelled."));
        discardResults();
        if (selectionDirtyWhileBusy && !scopeLockedToSelection) {
            selectionDirtyWhileBusy = false;
            refreshScanScope();
        }
        return;
    }

    statusLabel->setText(lastResult.complete ? tr("Scan complete.") : tr("Scan incomplete."));
    progressLabel->clear();
    rebuildTable();
    updateSummary();
    updateRowActionState();
    if (selectionDirtyWhileBusy && !scopeLockedToSelection) {
        selectionDirtyWhileBusy = false;
        refreshScanScope();
    }
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
    refreshScanScope();
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
        // Capture placement before setupCoinGeometry strips TopLoc_Location.
        // Apply transform even if meshing fails so placed/nested previews stay world-correct.
        const Part::TopoShape topo(shape);
        const SbMatrix transform = Base::convertTo<SbMatrix>(topo.getTransform());
        node->transform.setValue(transform);
        try {
            // Deep-copy so BRepTools::Clean / remesh cannot mutate shared document geometry.
            const TopoDS_Shape previewShape =
                BRepBuilderAPI_Copy(shape, Standard_True, Standard_False).Shape();
            PartGui::ViewProviderPartExt::setupCoinGeometry(previewShape, node, 0.5, 28.65);
            // setupCoinGeometry must not clear SoPreviewShape::transform; re-assert for safety.
            node->transform.setValue(transform);
        }
        catch (...) {
            // Keep the node (and its transform) so markers still show if mesh conversion fails.
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
    if (pairIndex < 0 || !host || !host->isAttachedToDocument()) {
        return;
    }
    const auto& pair = lastResult.pairs[static_cast<std::size_t>(pairIndex)];
    const auto& leafA = lastResult.leaves[pair.leafIndexA];
    const auto& leafB = lastResult.leaves[pair.leafIndexB];
    Gui::Selection().clearSelection();
    const char* docName = host->getDocument()->getName();
    const char* asmName = host->getNameInDocument();
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
    if (pairIndex < 0 || !host) {
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

    if (auto* guiDoc = hostGuiDocument()) {
        guiDoc->openCommand("Exclude interference source pair");
        Assembly::addInterferenceExclusion(host, sourceA, sourceB);
        guiDoc->commitCommand();
    }
    else {
        Assembly::addInterferenceExclusion(host, sourceA, sourceB);
    }
    onRun();
}

void TaskInterferenceCheck::onRestorePair()
{
    const int pairIndex = currentPairIndex();
    if (pairIndex < 0 || !host) {
        return;
    }
    const auto& pair = lastResult.pairs[static_cast<std::size_t>(pairIndex)];
    auto* sourceA = resolveSourceId(lastResult.leaves[pair.leafIndexA].sourceId);
    auto* sourceB = resolveSourceId(lastResult.leaves[pair.leafIndexB].sourceId);
    if (!sourceA || !sourceB) {
        return;
    }
    if (auto* guiDoc = hostGuiDocument()) {
        guiDoc->openCommand("Restore interference source pair");
        Assembly::removeInterferenceExclusion(host, sourceA, sourceB);
        guiDoc->commitCommand();
    }
    else {
        Assembly::removeInterferenceExclusion(host, sourceA, sourceB);
    }
    onRun();
}

void TaskInterferenceCheck::onManageExclusions()
{
    if (!host) {
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
        if (!host) {
            return;
        }
        const auto rules = Assembly::getInterferenceExclusionRules(host);
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
        if (!host) {
            dialog->reject();
            return;
        }
        const int index = selectedRuleIndex();
        auto rules = Assembly::getInterferenceExclusionRules(host);
        if (index < 0 || index >= static_cast<int>(rules.size())) {
            return;
        }
        auto& rule = rules[static_cast<std::size_t>(index)];
        if (!rule.valid || !rule.first || !rule.second) {
            return;
        }
        if (auto* guiDoc = hostGuiDocument()) {
            guiDoc->openCommand("Restore interference source pair");
            Assembly::removeInterferenceExclusion(host, rule.first, rule.second);
            guiDoc->commitCommand();
        }
        else {
            Assembly::removeInterferenceExclusion(host, rule.first, rule.second);
        }
        fillTable();
    });

    connect(removeBtn, &QPushButton::clicked, dialog, [this, dialog, selectedRuleIndex, fillTable]() {
        if (!host) {
            dialog->reject();
            return;
        }
        const int index = selectedRuleIndex();
        if (index < 0) {
            return;
        }
        if (auto* guiDoc = hostGuiDocument()) {
            guiDoc->openCommand("Remove interference exclusion rule");
            Assembly::removeInterferenceExclusionAt(host, static_cast<std::size_t>(index));
            guiDoc->commitCommand();
        }
        else {
            Assembly::removeInterferenceExclusionAt(host, static_cast<std::size_t>(index));
        }
        fillTable();
    });

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(box, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(box, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    layout->addWidget(box);

    // Non-blocking modal: allows document/task teardown while the dialog is open
    // (exec() would freeze the event loop and hide that lifecycle path).
    dialog->setModal(true);
    connect(dialog, &QDialog::finished, this, [this, dialog](int) {
        if (manageExclusionsDialog == dialog) {
            manageExclusionsDialog.clear();
        }
    });
    dialog->open();
}

TaskInterferenceCheckDialog::TaskInterferenceCheckDialog(App::DocumentObject* host)
{
    widget = new TaskInterferenceCheck(host);
    taskbox = new Gui::TaskView::TaskBox(
        Gui::BitmapFactory().pixmap("Assembly_CheckInterference"),
        tr("Check Interference"),
        true,
        nullptr
    );
    taskbox->groupLayout()->addWidget(widget);
    Content.push_back(taskbox);
}

TaskInterferenceCheckDialog::TaskInterferenceCheckDialog(
    App::DocumentObject* host,
    const InterferenceComponentOccurrence& componentA,
    const InterferenceComponentOccurrence& componentB
)
{
    widget = new TaskInterferenceCheck(host, componentA, componentB);
    taskbox = new Gui::TaskView::TaskBox(
        Gui::BitmapFactory().pixmap("Assembly_CheckInterference"),
        tr("Check Selected Components"),
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
