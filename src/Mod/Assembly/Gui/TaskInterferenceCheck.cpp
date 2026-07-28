// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"
#ifndef _PreComp_
# include <BRepBuilderAPI_Copy.hxx>
# include <QCheckBox>
# include <QComboBox>
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
#include <Base/Exception.h>
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

#include <QComboBox>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QThread>
#include <QUnhandledException>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <functional>
#include <algorithm>
#include <Mod/Spreadsheet/App/Sheet.h>

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
        case Part::InterferenceKind::Clear:
            return QObject::tr("Clear");
        case Part::InterferenceKind::InvalidInput:
            return QObject::tr("Invalid");
        case Part::InterferenceKind::Inconclusive:
            return QObject::tr("Inconclusive");
        default:
            return QObject::tr("Other");
    }
}

QString issueKindText(Assembly::InterferenceComponentIssue::Kind kind)
{
    switch (kind) {
        case Assembly::InterferenceComponentIssue::Kind::InvalidLeaf:
            return QObject::tr("Invalid geometry");
        case Assembly::InterferenceComponentIssue::Kind::InvalidRule:
            return QObject::tr("Invalid rule");
        case Assembly::InterferenceComponentIssue::Kind::FaceEnumerationCapped:
            return QObject::tr("Face enumeration capped");
        case Assembly::InterferenceComponentIssue::Kind::Other:
        default:
            return QObject::tr("Diagnostic");
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

InterferenceScanResult makeIncompleteDiagnosticResult(const char* message)
{
    InterferenceScanResult result;
    result.complete = false;
    result.cancelled = false;
    InterferenceComponentIssue issue;
    issue.kind = InterferenceComponentIssue::Kind::Other;
    issue.diagnostic =
        message && message[0] != '\0' ? message : "Unknown failure";
    result.componentIssues.push_back(issue);
    return result;
}

InterferenceScanResult incompleteResultFromBaseException(const Base::Exception& e)
{
    const std::string message = e.getMessage();
    return makeIncompleteDiagnosticResult(
        message.empty() ? e.what() : message.c_str()
    );
}

InterferenceScanResult incompleteResultFromStdException(const std::exception& e)
{
    return makeIncompleteDiagnosticResult(e.what());
}

InterferenceScanResult incompleteResultFromUnknownException()
{
    return makeIncompleteDiagnosticResult(nullptr);
}

InterferenceScanResult incompleteResultFromExceptionPtr(std::exception_ptr ptr)
{
    if (!ptr) {
        return incompleteResultFromUnknownException();
    }
    try {
        std::rethrow_exception(ptr);
    }
    catch (const Base::Exception& e) {
        return incompleteResultFromBaseException(e);
    }
    catch (const std::exception& e) {
        return incompleteResultFromStdException(e);
    }
    catch (...) {
        return incompleteResultFromUnknownException();
    }
}

InterferenceScanResult readWatcherInterferenceResult(QFutureWatcher<InterferenceScanResult>* watcher)
{
    try {
        return watcher->result();
    }
    catch (const QUnhandledException& e) {
        return incompleteResultFromExceptionPtr(e.exception());
    }
    catch (const Base::Exception& e) {
        return incompleteResultFromBaseException(e);
    }
    catch (const std::exception& e) {
        return incompleteResultFromStdException(e);
    }
    catch (...) {
        return incompleteResultFromUnknownException();
    }
}

/** Disconnects and deletes the watcher if worker launch aborts before setFuture(). */
class ScopedPendingScanWatcher
{
public:
    ScopedPendingScanWatcher(
        QFutureWatcher<InterferenceScanResult>* watcherIn,
        QObject* ownerIn,
        int& ownedWatcherCountIn
    )
        : watcher(watcherIn)
        , owner(ownerIn)
        , ownedWatcherCount(ownedWatcherCountIn)
    {
        ++ownedWatcherCount;
    }

    ~ScopedPendingScanWatcher()
    {
        if (!released && watcher) {
            QObject::disconnect(watcher, nullptr, owner, nullptr);
            delete watcher;
            watcher = nullptr;
            --ownedWatcherCount;
        }
    }

    void release()
    {
        released = true;
    }

private:
    QFutureWatcher<InterferenceScanResult>* watcher = nullptr;
    QObject* owner = nullptr;
    int& ownedWatcherCount;
    bool released = false;
};

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
    refreshClearanceSheetUi();

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
    includeHiddenCheck->setToolTip(
        tr("Include hidden components when scanning all occurrences. "
           "Disabled for selected-pair checks, which always scan both selected "
           "components even if they are hidden.")
    );
    showExcludedCheck = new QCheckBox(tr("Show excluded"), this);
    showClearFaceChecks = new QCheckBox(tr("Show clear face checks"), this);
    showClearFaceChecks->setToolTip(
        tr("Include Clear face-pair evaluations under each component pair. "
           "Off by default; App results always retain Clear hits.")
    );
    controls->addWidget(includeHiddenCheck);
    controls->addWidget(showExcludedCheck);
    controls->addWidget(showClearFaceChecks);
    controls->addStretch();
    layout->addLayout(controls);

    auto* sheetRow = new QHBoxLayout;
    sheetRow->addWidget(new QLabel(tr("Clearance sheet:"), this));
    clearanceSheetCombo = new QComboBox(this);
    clearanceSheetCombo->setMinimumWidth(160);
    clearanceSheetCombo->setToolTip(
        tr("Optional spreadsheet of face-specific design clearances. "
           "Assembly clearance is the fallback when no * default row is present.")
    );
    sheetRow->addWidget(clearanceSheetCombo, 1);
    clearanceSheetLabel = new QLabel(tr("Fallback spin when no * row."), this);
    clearanceSheetLabel->setWordWrap(true);
    sheetRow->addWidget(clearanceSheetLabel, 1);
    layout->addLayout(sheetRow);

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

    resultsTable = new QTableWidget(0, 8, this);
    resultsTable->setHorizontalHeaderLabels(
        {tr("Status"),
         tr("Occurrence A"),
         tr("Occurrence B"),
         tr("Min clearance"),
         tr("Overlap volume"),
         tr("Applied clearance"),
         tr("Faces"),
         tr("Rule")}
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
        showClearFaceChecks,
        &QCheckBox::toggled,
        this,
        &TaskInterferenceCheck::onShowClearFaceChecksToggled
    );
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
        clearanceSheetCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        &TaskInterferenceCheck::onClearanceSheetChanged
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

bool TaskInterferenceCheck::testIsRestorePairEnabled() const
{
    return restoreButton && restoreButton->isEnabled();
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

QString TaskInterferenceCheck::testSummaryText() const
{
    return summaryLabel ? summaryLabel->text() : QString();
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

QString TaskInterferenceCheck::testClearanceSheetLabel() const
{
    return clearanceSheetLabel ? clearanceSheetLabel->text() : QString();
}

void TaskInterferenceCheck::testRefreshClearanceSheetUi()
{
    refreshClearanceSheetUi();
}

void TaskInterferenceCheck::testSetShowClearFaceChecks(bool enabled)
{
    if (showClearFaceChecks) {
        showClearFaceChecks->setChecked(enabled);
    }
}

bool TaskInterferenceCheck::testShowClearFaceChecks() const
{
    return showClearFaceChecks && showClearFaceChecks->isChecked();
}

void TaskInterferenceCheck::testSetShowExcluded(bool enabled)
{
    if (showExcludedCheck) {
        showExcludedCheck->setChecked(enabled);
    }
}

bool TaskInterferenceCheck::testShowExcluded() const
{
    return showExcludedCheck && showExcludedCheck->isChecked();
}

void TaskInterferenceCheck::testSelectClearanceSheetByName(const QString& objectName)
{
    if (!clearanceSheetCombo) {
        return;
    }
    for (int i = 0; i < clearanceSheetCombo->count(); ++i) {
        if (clearanceSheetCombo->itemData(i).toString() == objectName) {
            updatingClearanceSheetUi = true;
            clearanceSheetCombo->setCurrentIndex(i);
            updatingClearanceSheetUi = false;
            onClearanceSheetChanged(i);
            return;
        }
    }
}

void TaskInterferenceCheck::testRebuildTable()
{
    rebuildTable();
}

void TaskInterferenceCheck::refreshClearanceSheetUi()
{
    if (!clearanceSheetCombo) {
        return;
    }
    updatingClearanceSheetUi = true;
    clearanceSheetCombo->clear();
    clearanceSheetCombo->addItem(tr("(none)"), QString());

    App::Document* doc = host ? host->getDocument() : nullptr;
    App::DocumentObject* linked = host ? Assembly::getInterferenceClearanceSheet(host) : nullptr;
    int selectIndex = 0;
    if (doc) {
        for (App::DocumentObject* obj : doc->getObjects()) {
            auto* sheet = freecad_cast<Spreadsheet::Sheet*>(obj);
            if (!sheet || !sheet->getNameInDocument()) {
                continue;
            }
            const QString label = QString::fromUtf8(sheet->Label.getValue());
            const QString name = QString::fromUtf8(sheet->getNameInDocument());
            clearanceSheetCombo->addItem(QStringLiteral("%1 (%2)").arg(label, name), name);
            if (sheet == linked) {
                selectIndex = clearanceSheetCombo->count() - 1;
            }
        }
    }
    clearanceSheetCombo->setCurrentIndex(selectIndex);
    if (clearanceSheetLabel) {
        if (linked && linked->getNameInDocument()) {
            clearanceSheetLabel->setText(
                tr("Using sheet \"%1\". Assembly clearance is fallback without a * row.")
                    .arg(QString::fromUtf8(linked->Label.getValue()))
            );
        }
        else {
            clearanceSheetLabel->setText(
                tr("No clearance sheet linked. Assembly clearance applies globally.")
            );
        }
    }
    updatingClearanceSheetUi = false;
}

void TaskInterferenceCheck::onClearanceSheetChanged(int index)
{
    if (updatingClearanceSheetUi || !host || !clearanceSheetCombo) {
        return;
    }
    const QString name = clearanceSheetCombo->itemData(index).toString();
    App::DocumentObject* sheet = nullptr;
    if (!name.isEmpty() && host->getDocument()) {
        sheet = host->getDocument()->getObject(name.toUtf8().constData());
    }
    App::DocumentObject* current = Assembly::getInterferenceClearanceSheet(host);
    if (sheet == current) {
        return;
    }
    try {
        if (auto* guiDoc = hostGuiDocument()) {
            guiDoc->openCommand("Set interference clearance sheet");
            Assembly::setInterferenceClearanceSheet(host, sheet);
            guiDoc->commitCommand();
        }
        else if (App::Document* doc = host->getDocument()) {
            doc->openTransaction("Set interference clearance sheet");
            Assembly::setInterferenceClearanceSheet(host, sheet);
            doc->commitTransaction();
        }
        else {
            Assembly::setInterferenceClearanceSheet(host, sheet);
        }
    }
    catch (const Base::Exception& exc) {
        Base::Console().warning("Interference clearance sheet: %s\n", exc.what());
        refreshClearanceSheetUi();
        return;
    }
    connectDocumentSignals();
    refreshClearanceSheetUi();
    if (hasResults()) {
        markStale("Clearance sheet changed");
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

bool TaskInterferenceCheck::testPreviewMarkerPoints(
    Base::Vector3d& pointOnFirst,
    Base::Vector3d& pointOnSecond
) const
{
    if (!previewRoot) {
        return false;
    }
    for (int i = 0; i < previewRoot->getNumChildren(); ++i) {
        auto* coordinates = dynamic_cast<SoCoordinate3*>(previewRoot->getChild(i));
        if (!coordinates || coordinates->point.getNum() < 2) {
            continue;
        }
        const SbVec3f first = coordinates->point[0];
        const SbVec3f second = coordinates->point[1];
        pointOnFirst = Base::Vector3d(first[0], first[1], first[2]);
        pointOnSecond = Base::Vector3d(second[0], second[1], second[2]);
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

bool TaskInterferenceCheck::testIsIncludeHiddenEnabled() const
{
    return includeHiddenCheck && includeHiddenCheck->isEnabled();
}

int TaskInterferenceCheck::testPenetrationCount() const
{
    return lastResult.counts.penetrations;
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

void TaskInterferenceCheck::testSetInjectWorkerFailureForGeneration(std::uint64_t generation)
{
    testInjectWorkerFailureGeneration = generation;
}

void TaskInterferenceCheck::testClearInjectWorkerFailure()
{
    testInjectWorkerFailureGeneration = 0;
}

void TaskInterferenceCheck::testSetWorkerInjectionControl(
    std::shared_ptr<InterferenceWorkerInjectionControl> control
)
{
    testWorkerInjectionControl = std::move(control);
}

void TaskInterferenceCheck::testClearWorkerInjectionControl()
{
    testWorkerInjectionControl.reset();
}

void TaskInterferenceCheck::testSetGuiThreadScanFailureInjection(
    const GuiThreadScanFailureInjection& injection
)
{
    testGuiThreadScanFailureInjection = injection;
}

void TaskInterferenceCheck::testClearGuiThreadScanFailureInjection()
{
    testGuiThreadScanFailureInjection = {};
}

int TaskInterferenceCheck::testOwnedScanWatcherCount() const
{
    return ownedScanWatcherCount_;
}

void TaskInterferenceCheck::throwInjectedGuiThreadScanFailure(
    GuiThreadScanFailureStage stage,
    const GuiThreadScanFailureInjection& captured
) const
{
    if (captured.stage != stage || captured.stage == GuiThreadScanFailureStage::None) {
        return;
    }
    switch (captured.kind) {
        case GuiThreadScanFailureKind::BaseException:
            throw Base::RuntimeError(captured.message.c_str());
        case GuiThreadScanFailureKind::StdException:
            throw std::runtime_error(captured.message);
        case GuiThreadScanFailureKind::Unknown:
            throw 1;
    }
}

void TaskInterferenceCheck::recoverFromSynchronousScanFailure(
    std::uint64_t generation,
    const InterferenceScanResult& diagnostic
)
{
    if (generation != session.activeGeneration()) {
        return;
    }
    preparingScan = false;
    clearPreview();
    if (progressLabel) {
        progressLabel->clear();
    }
    onScanFinished(generation, diagnostic);
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

bool TaskInterferenceCheck::testIsRunEnabled() const
{
    return runButton && runButton->isEnabled();
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
    hasAcceptedScanResult = false;
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
    if (clearanceSheetCombo) {
        clearanceSheetCombo->setEnabled(enabled);
    }
    if (includeHiddenCheck) {
        // Selected-pair mode keeps Include hidden disabled even while idle.
        includeHiddenCheck->setEnabled(enabled && !selectedComponentsMode);
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
        bool hasUnsuppressedViolation = isViolationKind(pair.detection.kind) && !pair.excluded;
        for (const auto& hit : pair.faceHits) {
            if (isViolationKind(hit.classification) && !hit.suppressedByExclusion) {
                hasUnsuppressedViolation = true;
            }
        }
        canExclude = !pair.excluded && hasUnsuppressedViolation;
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

int TaskInterferenceCheck::currentFaceHitIndex() const
{
    const int pairIndex = currentPairIndex();
    if (pairIndex < 0 || !resultsTable) {
        return -1;
    }
    auto* statusItem = resultsTable->item(resultsTable->currentRow(), 0);
    if (!statusItem) {
        return -1;
    }
    const int faceHitIndex = statusItem->data(Qt::UserRole + 1).toInt();
    const auto& hits = lastResult.pairs[static_cast<std::size_t>(pairIndex)].faceHits;
    return faceHitIndex >= 0 && faceHitIndex < static_cast<int>(hits.size())
        ? faceHitIndex
        : -1;
}

bool TaskInterferenceCheck::isResultAffectingProperty(const App::Property& prop) const
{
    if (host) {
        if (auto* sheet = Assembly::getInterferenceClearanceSheet(host)) {
            if (prop.getContainer() == sheet) {
                return true;
            }
        }
    }
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
        || std::strcmp(name, "InterferenceClearanceSheet") == 0
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
    // Always disconnect first. Clearing the connection vector without disconnect
    // would leave orphaned slots that use-after-free on later document changes.
    disconnectDocumentSignals();
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
                    const char* name = prop.getName();
                    if (name && std::strcmp(name, "InterferenceClearanceSheet") == 0) {
                        refreshClearanceSheetUi();
                    }
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
        if (includeHiddenCheck) {
            includeHiddenCheck->setEnabled(false);
            includeHiddenCheck->setToolTip(
                tr("Include hidden applies only to all-components scans. "
                   "Selected-pair checks always include both selected components, "
                   "even when hidden.")
            );
        }
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

    if (includeHiddenCheck) {
        includeHiddenCheck->setEnabled(true);
        includeHiddenCheck->setToolTip(
            tr("Include hidden components when scanning all occurrences. "
               "Disabled for selected-pair checks, which always scan both selected "
               "components even if they are hidden.")
        );
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
    hasAcceptedScanResult = false;
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

    const bool betweenSelected = selectedComponentsMode;
    std::vector<InterferenceLeaf> leavesA;
    std::vector<InterferenceLeaf> leavesB;
    InterferenceComponentScanSnapshot acrossSnapshot;

    GuiThreadScanFailureInjection capturedGuiFailureInjection;
    {
        const auto& pending = testGuiThreadScanFailureInjection;
        if (pending.stage != GuiThreadScanFailureStage::None
            && (pending.generation == 0 || pending.generation == generation)) {
            capturedGuiFailureInjection = pending;
        }
    }

    auto prepCancelled = [&]() {
        return cancel && cancel->load(std::memory_order_relaxed);
    };
    auto supersededGeneration = [&]() {
        return generation != session.activeGeneration();
    };

    try {
        if (betweenSelected) {
            // Explicitly selected occurrences always contribute geometry, including
            // when hidden. Traverse only each selected branch (includeHidden=true).
            if (testPreparationBarrierFn) {
                testPreparationBarrierFn();
            }
            if (supersededGeneration()) {
                return;
            }
            if (prepCancelled() || !host || !host->isAttachedToDocument()) {
                if (supersededGeneration()) {
                    return;
                }
                preparingScan = false;
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
            leavesA = collectInterferenceLeavesUnderPrefix(
                host,
                selectedA.occurrencePrefix,
                /*includeHidden=*/true
            );
            if (supersededGeneration()) {
                return;
            }
            if (prepCancelled() || !host || !host->isAttachedToDocument()) {
                if (supersededGeneration()) {
                    return;
                }
                preparingScan = false;
                (void)session.finishScan(generation);
                discardResults();
                cancelButton->setEnabled(false);
                setScanControlsEnabled(host != nullptr);
                if (statusLabel) {
                    statusLabel->setText(tr("Scan cancelled."));
                }
                return;
            }
            leavesB = collectInterferenceLeavesUnderPrefix(
                host,
                selectedB.occurrencePrefix,
                /*includeHidden=*/true
            );
            if (supersededGeneration()) {
                return;
            }
            if (prepCancelled() || !host || !host->isAttachedToDocument()) {
                if (supersededGeneration()) {
                    return;
                }
                preparingScan = false;
                (void)session.finishScan(generation);
                discardResults();
                cancelButton->setEnabled(false);
                setScanControlsEnabled(host != nullptr);
                if (statusLabel) {
                    statusLabel->setText(tr("Scan cancelled."));
                }
                return;
            }
            std::vector<InterferenceLeaf> preparedPairLeaves;
            preparedPairLeaves.reserve(leavesA.size() + leavesB.size());
            preparedPairLeaves.insert(
                preparedPairLeaves.end(),
                leavesA.begin(),
                leavesA.end()
            );
            preparedPairLeaves.insert(
                preparedPairLeaves.end(),
                leavesB.begin(),
                leavesB.end()
            );
            prepOptions.clearanceRules = Assembly::snapshotInterferenceClearanceRules(
                host,
                &preparedPairLeaves
            );
            throwInjectedGuiThreadScanFailure(
                GuiThreadScanFailureStage::SelectedPairPreparation,
                capturedGuiFailureInjection
            );
            if (supersededGeneration()) {
                return;
            }
            preparingScan = false;
            if (statusLabel) {
                statusLabel->setText(
                    tr("Scanning selected components '%1' and '%2'…")
                        .arg(QString::fromStdString(selectedA.displayPath))
                        .arg(QString::fromStdString(selectedB.displayPath))
                );
            }
        }
        else {
            // Snapshot DocumentObject-backed geometry on this thread before the worker.
            // Optional test barrier runs mid-preparation (after occurrence listing).
            auto snapshot = prepareInterferenceComponentScanSnapshot(
                host,
                includeHidden,
                prepOptions,
                testPreparationBarrierFn
            );
            if (supersededGeneration()) {
                return;
            }
            if (!host || !host->isAttachedToDocument() || snapshot.cancelled
                || (cancel && cancel->load(std::memory_order_relaxed))) {
                preparingScan = false;
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
            acrossSnapshot = std::move(snapshot);
            prepOptions.clearanceRules = Assembly::snapshotInterferenceClearanceRules(
                host,
                &acrossSnapshot.leaves
            );
            throwInjectedGuiThreadScanFailure(
                GuiThreadScanFailureStage::AllComponentsPreparation,
                capturedGuiFailureInjection
            );
            if (supersededGeneration()) {
                return;
            }
            preparingScan = false;
            if (statusLabel) {
                statusLabel->setText(tr("Scanning all applicable component occurrences…"));
            }
        }

        if (supersededGeneration()) {
            return;
        }

        // Worker phase: Cancel is meaningful for pair classification.
        cancelButton->setEnabled(true);
        if (progressLabel) {
            progressLabel->setText(tr("Progress: starting…"));
        }

        auto* watcher = new QFutureWatcher<InterferenceScanResult>(nullptr);
        ScopedPendingScanWatcher pendingWatcher(watcher, this, ownedScanWatcherCount_);
        std::uint64_t injectGeneration = testInjectWorkerFailureGeneration;
        if (injectGeneration == 0 && testWorkerInjectionControl) {
            injectGeneration = testWorkerInjectionControl->injectGeneration.load(
                std::memory_order_relaxed
            );
        }
        std::shared_ptr<InterferenceWorkerInjectionControl> workerInjection =
            testWorkerInjectionControl;
        connect(
            watcher,
            &QFutureWatcher<InterferenceScanResult>::finished,
            this,
            [self, watcher, generation, workerInjection, injectGeneration]() {
                if (self) {
                    --self->ownedScanWatcherCount_;
                }
                watcher->deleteLater();
                if (!self) {
                    return;
                }
                InterferenceScanResult result;
                if (watcher->future().isFinished()) {
                    result = readWatcherInterferenceResult(watcher);
                }
                self->onScanFinished(generation, result);
                if (workerInjection && injectGeneration != 0 && generation == injectGeneration) {
                    workerInjection->injectWatcherDelivered.store(true, std::memory_order_release);
                }
            }
        );

        throwInjectedGuiThreadScanFailure(
            GuiThreadScanFailureStage::WorkerLaunchSetup,
            capturedGuiFailureInjection
        );

        QFuture<InterferenceScanResult> future = QtConcurrent::run(
            [leavesA = std::move(leavesA),
             leavesB = std::move(leavesB),
             acrossSnapshot = std::move(acrossSnapshot),
             betweenSelected,
             excluded = std::move(excluded),
             clearance,
             clearanceRules = prepOptions.clearanceRules,
             cancel,
             self,
             generation,
             injectGeneration,
             workerInjection]() mutable {
                if (injectGeneration != 0 && injectGeneration == generation) {
                    if (workerInjection) {
                        workerInjection->workerStarted.store(true, std::memory_order_release);
                        // Test seam: explicit hold ignores cooperative cancel from newer generations.
                        while (workerInjection->holdInWorker.load(std::memory_order_acquire)) {
                            QThread::msleep(2);
                        }
                    }
                    throw Base::RuntimeError("Injected worker failure");
                }
                InterferenceScanOptions options;
                options.clearance = clearance;
                options.clearanceRules = std::move(clearanceRules);
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
                            if (!self->session.isBusy()) {
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
        watcher->setParent(this);
        pendingWatcher.release();
    }
    catch (const Base::Exception& e) {
        recoverFromSynchronousScanFailure(generation, incompleteResultFromBaseException(e));
    }
    catch (const std::exception& e) {
        recoverFromSynchronousScanFailure(generation, incompleteResultFromStdException(e));
    }
    catch (...) {
        recoverFromSynchronousScanFailure(generation, incompleteResultFromUnknownException());
    }
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

    hasAcceptedScanResult = true;

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
    const bool showExcluded = showExcludedCheck && showExcludedCheck->isChecked();
    const bool showClear = showClearFaceChecks && showClearFaceChecks->isChecked();

    auto ruleKindText = [this](Assembly::InterferenceClearanceRuleKind kind) -> QString {
        switch (kind) {
            case Assembly::InterferenceClearanceRuleKind::ExactPair:
                return tr("Exact pair");
            case Assembly::InterferenceClearanceRuleKind::MaxIndividual:
                return tr("Max face");
            case Assembly::InterferenceClearanceRuleKind::DefaultStar:
                return tr("Default *");
            case Assembly::InterferenceClearanceRuleKind::Unresolved:
                return tr("Unresolved");
            case Assembly::InterferenceClearanceRuleKind::AssemblyGlobal:
            default:
                return tr("Assembly");
        }
    };

    for (std::size_t i = 0; i < lastResult.pairs.size(); ++i) {
        const auto& pair = lastResult.pairs[i];

        const bool solidInvalidOrInconclusive =
            pair.detection.kind == Part::InterferenceKind::InvalidInput
            || pair.detection.kind == Part::InterferenceKind::Inconclusive;
        const bool solidPenetration = pair.detection.kind == Part::InterferenceKind::Penetration;

        std::vector<const Assembly::InterferenceFaceHit*> visibleHits;
        std::vector<std::size_t> visibleHitIndices;
        bool hasVisibleUnknown = solidInvalidOrInconclusive;
        bool hasVisibleViolation = false;
        for (std::size_t hitIndex = 0; hitIndex < pair.faceHits.size(); ++hitIndex) {
            const auto& hit = pair.faceHits[hitIndex];
            if (hit.classification == Part::InterferenceKind::Clear && !showClear) {
                continue;
            }
            if (hit.suppressedByExclusion && !showExcluded) {
                continue;
            }
            if (hit.classification == Part::InterferenceKind::InvalidInput
                || hit.classification == Part::InterferenceKind::Inconclusive) {
                hasVisibleUnknown = true;
            }
            if (isViolationKind(hit.classification) && !hit.suppressedByExclusion) {
                hasVisibleViolation = true;
            }
            if (hit.suppressedByExclusion && showExcluded) {
                hasVisibleViolation = true;
            }
            visibleHits.push_back(&hit);
            visibleHitIndices.push_back(hitIndex);
        }

        const bool solidReportable = solidInvalidOrInconclusive
            || (solidPenetration && (!pair.excluded || showExcluded))
            || (pair.faceHits.empty()
                && (pair.detection.kind == Part::InterferenceKind::ClearanceViolation
                    || pair.detection.kind == Part::InterferenceKind::Contact)
                && (!pair.excluded || showExcluded));

        // Excluded-only violation pairs stay hidden unless Show excluded.
        if (!solidReportable && visibleHits.empty()) {
            continue;
        }
        if (pair.excluded && !showExcluded && !hasVisibleUnknown && !hasVisibleViolation
            && !solidInvalidOrInconclusive) {
            continue;
        }

        const int row = resultsTable->rowCount();
        resultsTable->insertRow(row);

        const Assembly::InterferenceFaceHit* governingHit = nullptr;
        std::size_t governingHitIndex = static_cast<std::size_t>(-1);
        if (pair.governingFaceHitIndex < pair.faceHits.size()) {
            governingHitIndex = pair.governingFaceHitIndex;
            governingHit = &pair.faceHits[governingHitIndex];
        }
        const Assembly::InterferenceFaceHit* displayedHit = nullptr;
        std::size_t displayedHitIndex = static_cast<std::size_t>(-1);
        const auto governingVisible = std::find(
            visibleHitIndices.begin(),
            visibleHitIndices.end(),
            governingHitIndex
        );
        if (governingHit && governingVisible != visibleHitIndices.end()) {
            displayedHit = governingHit;
            displayedHitIndex = governingHitIndex;
        }
        else if (!visibleHits.empty() && !solidReportable) {
            // If the governing violation is filtered (normally an exclusion),
            // report the first remaining diagnostic hit and preview that same hit.
            displayedHit = visibleHits.front();
            displayedHitIndex = visibleHitIndices.front();
        }

        QString status;
        if (displayedHit) {
            status = kindText(
                displayedHit->classification,
                displayedHit->suppressedByExclusion
            );
        }
        else {
            status = kindText(
                pair.detection.kind,
                pair.excluded && showExcluded && !solidInvalidOrInconclusive
            );
        }

        auto* statusItem = new QTableWidgetItem(status);
        statusItem->setData(Qt::UserRole, static_cast<int>(i));
        statusItem->setData(
            Qt::UserRole + 1,
            displayedHit ? static_cast<int>(displayedHitIndex) : -1
        );
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
        QString distanceText = formatPairDistance(pair.detection);
        if (displayedHit) {
            distanceText = displayedHit->distance >= 0.0
                    && std::isfinite(displayedHit->distance)
                ? formatLength(displayedHit->distance)
                : (displayedHit->diagnostic.empty()
                       ? QStringLiteral("—")
                       : QString::fromStdString(displayedHit->diagnostic));
        }
        resultsTable->setItem(row, 3, new QTableWidgetItem(distanceText));
        resultsTable->setItem(
            row,
            4,
            new QTableWidgetItem(
                displayedHit ? QStringLiteral("—") : formatPairVolume(pair.detection)
            )
        );

        QStringList appliedParts;
        QStringList faceParts;
        QStringList ruleParts;
        if (!displayedHit && pair.detection.kind == Part::InterferenceKind::Penetration) {
            appliedParts << tr("n/a");
        }
        if (displayedHit) {
            appliedParts << formatLength(displayedHit->appliedClearance);
            QString hitStatus =
                kindText(displayedHit->classification, displayedHit->suppressedByExclusion);
            faceParts << QStringLiteral("[%1] %2 ↔ %3")
                             .arg(hitStatus)
                             .arg(QString::fromStdString(displayedHit->facePathA))
                             .arg(QString::fromStdString(displayedHit->facePathB));
            QString rule = ruleKindText(displayedHit->ruleKind);
            QStringList provenance;
            const std::size_t n = displayedHit->sourceRows.size();
            for (std::size_t ri = 0; ri < n; ++ri) {
                QString part = QStringLiteral("row %1").arg(displayedHit->sourceRows[ri]);
                const QString comment =
                    ri < displayedHit->sourceComments.size()
                    ? QString::fromStdString(displayedHit->sourceComments[ri])
                    : QString();
                if (!comment.isEmpty()) {
                    part += QStringLiteral(": %1").arg(comment);
                }
                provenance << part;
            }
            if (!provenance.isEmpty()) {
                rule += QStringLiteral(" (%1)").arg(provenance.join(QStringLiteral("; ")));
            }
            ruleParts << rule;
            if (!displayedHit->diagnostic.empty()) {
                ruleParts << QString::fromStdString(displayedHit->diagnostic);
            }
        }
        else if (!pair.detection.diagnostic.empty()) {
            ruleParts << QString::fromStdString(pair.detection.diagnostic);
        }
        if (!pair.faceEnumerationDiagnostic.empty()) {
            ruleParts << QString::fromStdString(pair.faceEnumerationDiagnostic);
        }

        resultsTable->setItem(
            row,
            5,
            new QTableWidgetItem(appliedParts.isEmpty() ? QStringLiteral("—") : appliedParts.join(QStringLiteral("\n")))
        );
        resultsTable->setItem(
            row,
            6,
            new QTableWidgetItem(faceParts.isEmpty() ? QString() : faceParts.join(QStringLiteral("\n")))
        );
        resultsTable->setItem(
            row,
            7,
            new QTableWidgetItem(ruleParts.isEmpty() ? QString() : ruleParts.join(QStringLiteral("\n")))
        );
    }

    for (const auto& issue : lastResult.componentIssues) {
        const int row = resultsTable->rowCount();
        resultsTable->insertRow(row);
        auto* statusItem = new QTableWidgetItem(issueKindText(issue.kind));
        statusItem->setData(Qt::UserRole, -1);
        resultsTable->setItem(row, 0, statusItem);
        resultsTable->setItem(
            row,
            1,
            new QTableWidgetItem(
                issue.leafIndex < lastResult.leaves.size()
                    ? QString::fromStdString(lastResult.leaves[issue.leafIndex].displayPath)
                    : QString()
            )
        );
        resultsTable->setItem(
            row,
            2,
            new QTableWidgetItem(
                issue.leafIndexB != static_cast<std::size_t>(-1)
                        && issue.leafIndexB < lastResult.leaves.size()
                    ? QString::fromStdString(lastResult.leaves[issue.leafIndexB].displayPath)
                    : QString()
            )
        );
        resultsTable->setItem(row, 3, new QTableWidgetItem(QString()));
        resultsTable->setItem(row, 4, new QTableWidgetItem(QString()));
        resultsTable->setItem(row, 5, new QTableWidgetItem(QString()));
        resultsTable->setItem(row, 6, new QTableWidgetItem(QString()));
        resultsTable->setItem(row, 7, new QTableWidgetItem(QString::fromStdString(issue.diagnostic)));
    }
    updateRowActionState();
}

void TaskInterferenceCheck::updateSummary()
{
    const auto& c = lastResult.counts;
    summaryLabel->setText(
        tr("Penetrations: %1 | Contacts: %2 | Clearance: %3 | Excluded: %4 | Invalid geom: %5 | "
           "Invalid rules: %6 | Inconclusive: %7 | Clear faces: %8 | Clear pairs: %9")
            .arg(c.penetrations)
            .arg(c.contacts)
            .arg(c.clearanceViolations)
            .arg(c.excludedViolations)
            .arg(c.invalidInputs)
            .arg(c.invalidRules)
            .arg(c.inconclusivePairs)
            .arg(c.clearFaceHits)
            .arg(c.clearPairs)
    );
}

void TaskInterferenceCheck::onShowExcludedToggled(bool)
{
    rebuildTable();
}

void TaskInterferenceCheck::onShowClearFaceChecksToggled(bool)
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
    const int faceHitIndex = currentFaceHitIndex();
    const Assembly::InterferenceFaceHit* faceHit =
        faceHitIndex >= 0 ? &pair.faceHits[static_cast<std::size_t>(faceHitIndex)] : nullptr;
    const auto color =
        colorForKind(faceHit ? faceHit->classification : pair.detection.kind);

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

    const TopoDS_Shape& commonShape =
        faceHit ? faceHit->commonShape : pair.detection.commonShape;
    if (!commonShape.IsNull()) {
        addPreviewShape(commonShape, 0.45F);
    }

    const bool closestPointsValid =
        faceHit ? faceHit->closestPointsValid : hasValidClosestPoints(pair.detection);
    if (closestPointsValid) {
        const Base::Vector3d& first =
            faceHit ? faceHit->pointOnFirst : pair.detection.pointOnFirst;
        const Base::Vector3d& second =
            faceHit ? faceHit->pointOnSecond : pair.detection.pointOnSecond;
        auto* coords = new SoCoordinate3;
        const SbVec3f p1(
            static_cast<float>(first.x),
            static_cast<float>(first.y),
            static_cast<float>(first.z)
        );
        const SbVec3f p2(
            static_cast<float>(second.x),
            static_cast<float>(second.y),
            static_cast<float>(second.z)
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

ExcludePairCommandResult AssemblyGui::tryExcludeInterferencePairInCommand(
    Gui::Document* guiDocument,
    App::DocumentObject* host,
    App::DocumentObject* sourceA,
    App::DocumentObject* sourceB
)
{
    ExcludePairCommandResult result;
    if (!host || !sourceA || !sourceB) {
        result.errorMessage = TaskInterferenceCheck::tr("Cannot exclude pair: missing host or source.");
        return result;
    }
    if (!guiDocument) {
        result.errorMessage =
            TaskInterferenceCheck::tr("Cannot exclude pair: no active GUI document.");
        return result;
    }

    bool commandOpen = false;
    try {
        guiDocument->openCommand("Exclude interference source pair");
        commandOpen = true;
        Assembly::addInterferenceExclusion(host, sourceA, sourceB);
        guiDocument->commitCommand();
        commandOpen = false;
        result.success = true;
        return result;
    }
    catch (const Base::Exception& exc) {
        if (commandOpen) {
            guiDocument->abortCommand();
        }
        result.errorMessage = QString::fromUtf8(exc.what());
        return result;
    }
    catch (const std::exception& exc) {
        if (commandOpen) {
            guiDocument->abortCommand();
        }
        result.errorMessage = QString::fromUtf8(exc.what());
        return result;
    }
    catch (...) {
        if (commandOpen) {
            guiDocument->abortCommand();
        }
        result.errorMessage =
            TaskInterferenceCheck::tr("Unknown error while excluding interference source pair.");
        return result;
    }
}

bool TaskInterferenceCheck::testExecuteExcludePairCommand(
    App::DocumentObject* sourceA,
    App::DocumentObject* sourceB,
    Gui::Document* guiDocument,
    QString* errorOut
)
{
    const auto result =
        tryExcludeInterferencePairInCommand(guiDocument, host, sourceA, sourceB);
    if (errorOut) {
        *errorOut = result.errorMessage;
    }
    return result.success;
}

bool TaskInterferenceCheck::testExecuteExcludePairForSelectedRow(
    Gui::Document* guiDocument,
    QString* errorOut
)
{
    const int pairIndex = currentPairIndex();
    if (pairIndex < 0 || !host) {
        if (errorOut) {
            *errorOut = tr("No interference pair selected.");
        }
        return false;
    }
    const auto& pair = lastResult.pairs[static_cast<std::size_t>(pairIndex)];
    auto* sourceA = resolveSourceId(lastResult.leaves[pair.leafIndexA].sourceId);
    auto* sourceB = resolveSourceId(lastResult.leaves[pair.leafIndexB].sourceId);
    if (!sourceA || !sourceB) {
        if (errorOut) {
            *errorOut = tr("Could not resolve selected interference source pair.");
        }
        return false;
    }
    const auto result =
        tryExcludeInterferencePairInCommand(guiDocument, host, sourceA, sourceB);
    if (errorOut) {
        *errorOut = result.errorMessage;
    }
    return result.success;
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

    Gui::Document* guiDocument = hostGuiDocument();
    const auto commandResult = tryExcludeInterferencePairInCommand(
        guiDocument,
        host,
        sourceA,
        sourceB
    );
    if (!commandResult.success) {
        if (!commandResult.errorMessage.isEmpty()) {
            QMessageBox::warning(this, tr("Exclude source pair"), commandResult.errorMessage);
        }
        return;
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
