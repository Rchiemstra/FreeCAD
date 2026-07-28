// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QPointer>
#include <QString>

#include <Base/Quantity.h>
#include <Base/Unit.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>
#include <Mod/Assembly/AssemblyGlobal.h>
#include <Mod/Assembly/App/InterferenceScan.h>
#include <Mod/Assembly/App/InterferenceScanSession.h>
#include <fastsignals/connection.h>

class QCheckBox;
class QComboBox;
class QDialog;
class QLabel;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class SoGroup;
class SoSeparator;

namespace App
{
class Document;
class DocumentObject;
class Property;
}

namespace Gui
{
class Document;
class QuantitySpinBox;
class View3DInventor;
class View3DInventorViewer;
}

namespace AssemblyGui
{

struct AssemblyGuiExport ExcludePairCommandResult
{
    bool success {false};
    QString errorMessage;
};

/**
 * Open a GUI command, append an interference exclusion pair, and commit only on success.
 * Aborts the command on insertion/commit failures and returns a user-facing error message.
 */
AssemblyGuiExport ExcludePairCommandResult tryExcludeInterferencePairInCommand(
    Gui::Document* guiDocument,
    App::DocumentObject* host,
    App::DocumentObject* sourceA,
    App::DocumentObject* sourceB
);

/**
 * Test-only seam: shared atomics captured by value into the QtConcurrent worker.
 * Workers must not read or write TaskInterferenceCheck state directly.
 */
struct AssemblyGuiExport InterferenceWorkerInjectionControl
{
    std::atomic<std::uint64_t> injectGeneration {0};
    std::atomic<bool> workerStarted {false};
    std::atomic<bool> holdInWorker {false};
    std::atomic<bool> injectWatcherDelivered {false};
};

class AssemblyGuiExport TaskInterferenceCheck: public QWidget
{
    Q_OBJECT

public:
    explicit TaskInterferenceCheck(
        App::DocumentObject* host,
        QWidget* parent = nullptr
    );
    TaskInterferenceCheck(
        App::DocumentObject* host,
        const Assembly::InterferenceComponentOccurrence& componentA,
        const Assembly::InterferenceComponentOccurrence& componentB,
        QWidget* parent = nullptr
    );
    ~TaskInterferenceCheck() override;

    bool accept();
    bool reject();

    // Test hooks (also used by lifecycle unit tests via friend access patterns).
    Assembly::InterferenceScanSession& scanSession()
    {
        return session;
    }
    bool isScanning() const
    {
        return session.isBusy();
    }
    bool hasResults() const
    {
        return hasAcceptedScanResult;
    }
    bool isSelectPairEnabled() const;
    bool isExcludePairEnabled() const;

    /**
     * Test / lifecycle hooks. Deliver a finished scan as the QtConcurrent watcher would.
     * Used to prove B-then-A generations do not mutate UI after a newer scan completed.
     */
    void testDeliverScanFinished(
        std::uint64_t generation,
        const Assembly::InterferenceScanResult& result
    );
    QString testStatusText() const;
    QString testSummaryText() const;
    QString testProgressText() const;
    int testTableRowCount() const;
    QString testTableCellText(int row, int column) const;
    std::size_t testResultPairCount() const;
    Base::Unit testClearanceUnit() const;
    double testClearanceRawMm() const;
    void testSetClearanceQuantity(const Base::Quantity& quantity);
    QString testClearanceSheetLabel() const;
    void testRefreshClearanceSheetUi();
    void testSetShowClearFaceChecks(bool enabled);
    bool testShowClearFaceChecks() const;
    void testSetShowExcluded(bool enabled);
    bool testShowExcluded() const;
    void testSelectClearanceSheetByName(const QString& objectName);
    void testRebuildTable();
    bool testIsRestorePairEnabled() const;

    /**
     * Run the document-command exclusion path (no confirmation dialog). Does not rescan.
     */
    bool testExecuteExcludePairCommand(
        App::DocumentObject* sourceA,
        App::DocumentObject* sourceB,
        Gui::Document* guiDocument,
        QString* errorOut = nullptr
    );

    /**
     * Same resolution path as onExcludePair() (selected row + resolveSourceId), without the
     * confirmation dialog. Does not rescan.
     */
    bool testExecuteExcludePairForSelectedRow(
        Gui::Document* guiDocument,
        QString* errorOut = nullptr
    );

    /** Create a detached preview root without MainWindow/viewer (Inventor unit tests). */
    void testEnsureDetachedPreviewRoot();
    bool testHasPreviewRoot() const;
    void testSelectResultRow(int row);
    int testPreviewShapeCount() const;
    bool testPreviewShapeTranslation(
        int shapeIndex,
        double& x,
        double& y,
        double& z
    ) const;
    void testOpenManageExclusions();
    bool testManageExclusionsOpen() const;
    bool testHasHost() const;
    QString testScopeText() const;
    bool testIsSelectedPairMode() const;
    bool testIsIncludeHiddenEnabled() const;
    int testPenetrationCount() const;
    void testRefreshScanScope();
    /**
     * Headless-safe stand-in for Gui::Selection: feeds the same handle list
     * refreshScanScope would build from Selection.getSelectionEx(). Valid
     * addSelection() requires Gui::Application/MainWindow and crashes without it.
     */
    void testSetSelectionHandles(
        std::vector<Assembly::InterferenceSelectionHandle> handles
    );
    void testClearSelectionHandles();
    void testNotifySelectionChanged();
    void testSetPreparationBarrier(std::function<void()> barrier);
    void testClearPreparationBarrier();
    /**
     * GUI-thread generation tag captured into the next worker lambda before launch.
     * When that generation runs, the worker throws Base::RuntimeError.
     */
    void testSetInjectWorkerFailureForGeneration(std::uint64_t generation);
    void testClearInjectWorkerFailure();
    void testSetWorkerInjectionControl(
        std::shared_ptr<InterferenceWorkerInjectionControl> control
    );
    void testClearWorkerInjectionControl();
    void testSetIncludeHidden(bool enabled);
    void testRunScan();
    bool testIsPreparing() const;
    bool testIsCancelEnabled() const;
    bool testIsRunEnabled() const;
    /** Attach previewRoot to an arbitrary Coin scene (no MainWindow required). */
    void testAttachPreviewToScene(SoGroup* scene);
    void testDetachPreview();
    int testPreviewIndexInScene(SoGroup* scene) const;

private Q_SLOTS:
    void onRun();
    void onCancelScan();
    void onSelectPair();
    void onExcludePair();
    void onRestorePair();
    void onManageExclusions();
    void onShowExcludedToggled(bool checked);
    void onShowClearFaceChecksToggled(bool checked);
    void onIncludeHiddenToggled(bool checked);
    void onRowChanged();
    void onScanFinished(std::uint64_t generation, const Assembly::InterferenceScanResult& result);
    void onScanProgress(int current, int total);
    void onSelectionChanged();
    void onClearanceSheetChanged(int index);

private:
    void setupUi();
    void refreshClearanceSheetUi();
    void refreshScanScope();
    void attachPreviewToViewer();
    /** Create previewRoot and add it to scene; optional viewer/view for live teardown hooks. */
    void attachPreviewToScene(
        SoGroup* scene,
        Gui::View3DInventorViewer* view = nullptr,
        Gui::View3DInventor* viewWin = nullptr
    );
    void detachPreviewFromViewer();
    void clearPreview();
    void updatePreviewForCurrentRow();
    void rebuildTable();
    void updateSummary();
    void discardResults();
    void updateRowActionState();
    void setScanControlsEnabled(bool enabled);
    void markStale(const char* reason);
    void connectDocumentSignals();
    void disconnectDocumentSignals();
    void closeManageExclusionsDialog();
    bool isResultAffectingProperty(const App::Property& prop) const;
    Gui::Document* hostGuiDocument() const;
    Gui::View3DInventorViewer* viewer() const;
    std::vector<std::pair<std::string, std::string>> currentExclusionSourceIds() const;
    App::DocumentObject* resolveSourceId(const std::string& sourceId) const;
    int currentPairIndex() const;
    QString formatLength(double mm) const;
    QString formatVolume(double cubicMm) const;
    QString formatPairDistance(const Part::InterferenceResult& detection) const;
    QString formatPairVolume(const Part::InterferenceResult& detection) const;

    App::DocumentObject* host = nullptr;
    Assembly::InterferenceComponentOccurrence selectedA;
    Assembly::InterferenceComponentOccurrence selectedB;
    bool selectedComponentsMode = false;
    /** When true, scope stays on the constructor-provided component pair. */
    bool scopeLockedToSelection = false;
    /** Selection changed while a scan was busy; refresh scope after finish. */
    bool selectionDirtyWhileBusy = false;
    std::function<void()> testPreparationBarrierFn;
    std::uint64_t testInjectWorkerFailureGeneration = 0;
    std::shared_ptr<InterferenceWorkerInjectionControl> testWorkerInjectionControl;
    bool hasTestSelectionOverride = false;
    std::vector<Assembly::InterferenceSelectionHandle> testSelectionHandles;
    /** True while DocumentObject-backed snapshot preparation is running (Cancel inactive). */
    bool preparingScan = false;
    Assembly::InterferenceScanSession session;
    Gui::QuantitySpinBox* clearanceSpin = nullptr;
    QComboBox* clearanceSheetCombo = nullptr;
    QLabel* clearanceSheetLabel = nullptr;
    QCheckBox* includeHiddenCheck = nullptr;
    QCheckBox* showExcludedCheck = nullptr;
    QCheckBox* showClearFaceChecks = nullptr;
    QLabel* summaryLabel = nullptr;
    QLabel* scopeLabel = nullptr;
    QLabel* statusLabel = nullptr;
    QLabel* progressLabel = nullptr;
    bool updatingClearanceSheetUi = false;
    QTableWidget* resultsTable = nullptr;
    QPushButton* runButton = nullptr;
    QPushButton* cancelButton = nullptr;
    QPushButton* selectPairButton = nullptr;
    QPushButton* excludeButton = nullptr;
    QPushButton* restoreButton = nullptr;
    QPushButton* manageExclusionsButton = nullptr;

    Assembly::InterferenceScanResult lastResult;
    /** True after the active scan generation finishes via finishScan() and is not cancelled. */
    bool hasAcceptedScanResult = false;
    SoSeparator* previewRoot = nullptr;
    SoGroup* attachedScene = nullptr;
    Gui::View3DInventorViewer* attachedViewer = nullptr;
    QPointer<Gui::View3DInventor> attachedView;
    QPointer<QDialog> manageExclusionsDialog;
    std::set<const App::Document*> watchedDocuments;

    std::vector<fastsignals::connection> connections;
};

class AssemblyGuiExport TaskInterferenceCheckDialog: public Gui::TaskView::TaskDialog
{
    Q_OBJECT

public:
    explicit TaskInterferenceCheckDialog(App::DocumentObject* host);
    TaskInterferenceCheckDialog(
        App::DocumentObject* host,
        const Assembly::InterferenceComponentOccurrence& componentA,
        const Assembly::InterferenceComponentOccurrence& componentB
    );
    ~TaskInterferenceCheckDialog() override;

    QDialogButtonBox::StandardButtons getStandardButtons() const override
    {
        return QDialogButtonBox::Close;
    }
    bool accept() override;
    bool reject() override;
    bool isAllowedAlterDocument() const override
    {
        return true;
    }

private:
    TaskInterferenceCheck* widget = nullptr;
    Gui::TaskView::TaskBox* taskbox = nullptr;
};

}  // namespace AssemblyGui
