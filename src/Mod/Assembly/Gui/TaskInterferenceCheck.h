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

#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <Base/Quantity.h>
#include <Base/Unit.h>
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

struct AssemblyGuiExport ExcludePairCommandEntry
{
    App::DocumentObject* sourceA = nullptr;
    App::DocumentObject* sourceB = nullptr;
    Assembly::ReviewNote* reason = nullptr;
};

/**
 * Add multiple exact source-pair exclusions in one undoable document command.
 * The operation is atomic: a failure aborts the complete command.
 */
AssemblyGuiExport ExcludePairCommandResult tryExcludeInterferencePairsInCommand(
    Gui::Document* guiDocument,
    App::DocumentObject* host,
    const std::vector<ExcludePairCommandEntry>& entries
);

/**
 * Open a GUI command, append an interference exclusion pair, and commit only on success.
 * Aborts the command on insertion/commit failures and returns a user-facing error message.
 */
AssemblyGuiExport ExcludePairCommandResult tryExcludeInterferencePairInCommand(
    Gui::Document* guiDocument,
    App::DocumentObject* host,
    App::DocumentObject* sourceA,
    App::DocumentObject* sourceB,
    Assembly::ReviewNote* reason = nullptr
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

/** GUI-thread-only scan failure injection stage (unit tests). */
enum class GuiThreadScanFailureStage
{
    None,
    AllComponentsPreparation,
    SelectedPairPreparation,
    WorkerLaunchSetup,
};

enum class GuiThreadScanFailureKind
{
    BaseException,
    StdException,
    Unknown,
};

struct AssemblyGuiExport GuiThreadScanFailureInjection
{
    std::uint64_t generation = 0;
    GuiThreadScanFailureStage stage = GuiThreadScanFailureStage::None;
    GuiThreadScanFailureKind kind = GuiThreadScanFailureKind::BaseException;
    std::string message;
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
    bool isCreateReviewNoteEnabled() const;
    std::size_t testAffectedViolationPairCount() const;

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
    bool testCreateClearanceSheet(QString* errorOut = nullptr);
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
    bool testCreateReviewNoteForSelectedRow(QString* errorOut = nullptr);
    void testSelectResultRows(const std::vector<int>& rows);
    std::size_t testSelectedExclusionSourcePairCount() const;
    bool testExecuteExcludePairsForSelectedRows(
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
    bool testPreviewMarkerPoints(
        Base::Vector3d& pointOnFirst,
        Base::Vector3d& pointOnSecond
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
    void testSetGuiThreadScanFailureInjection(const GuiThreadScanFailureInjection& injection);
    void testClearGuiThreadScanFailureInjection();
    int testOwnedScanWatcherCount() const;
    void testSetIncludeHidden(bool enabled);
    void testRunScan();
    bool testIsPreparing() const;
    bool testIsCancelEnabled() const;
    bool testIsRunEnabled() const;
    /** Simulate destruction of the attached 3D view without requiring a MainWindow. */
    void testNotifyAttachedViewDestroyed();
    /** Attach previewRoot to an arbitrary Coin scene (no MainWindow required). */
    void testAttachPreviewToScene(SoGroup* scene);
    void testDetachPreview();
    int testPreviewIndexInScene(SoGroup* scene) const;

    /** True when a dock-panel request addresses this widget's current host and scope. */
    bool matchesContext(
        App::DocumentObject* requestedHost,
        const Assembly::InterferenceComponentOccurrence& componentA = {},
        const Assembly::InterferenceComponentOccurrence& componentB = {}
    ) const;
    /** Rebind result preview to the current 3D view after GUI navigation. */
    void activateInCurrentView();
    /** Explain why a different dock-panel request did not replace an active scan. */
    void notifyBusyContextRetained();

private Q_SLOTS:
    void onRun();
    void onCancelScan();
    void onSelectPair();
    void onCreateReviewNote();
    void onExcludePair();
    void onRestorePair();
    void onManageExclusions();
    void onShowExcludedToggled(bool checked);
    void onShowClearFaceChecksToggled(bool checked);
    void onIncludeHiddenToggled(bool checked);
    void onRowChanged();
    void onScanFinished(std::uint64_t generation, const Assembly::InterferenceScanResult& result);
    void onScanProgress(int current, int total);
    void recoverFromSynchronousScanFailure(
        std::uint64_t generation,
        const Assembly::InterferenceScanResult& diagnostic
    );
    void throwInjectedGuiThreadScanFailure(
        GuiThreadScanFailureStage stage,
        const GuiThreadScanFailureInjection& captured
    ) const;
    void onSelectionChanged();
    void onClearanceSheetChanged(int index);
    void onCreateClearanceSheet();

private:
    void setupUi();
    void refreshClearanceSheetUi();
    bool createClearanceSheet(QString& errorMessage);
    void refreshScanScope();
    void attachPreviewToViewer();
    /** Create previewRoot and add it to scene; optional viewer/view for live teardown hooks. */
    void attachPreviewToScene(
        SoGroup* scene,
        Gui::View3DInventorViewer* view = nullptr,
        Gui::View3DInventor* viewWin = nullptr
    );
    void onAttachedViewDestroyed();
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
    Assembly::ReviewNote* matchingInterferenceReasonNote(
        const std::string& sourceIdA,
        const std::string& sourceIdB
    ) const;
    std::vector<std::size_t> selectedPairIndices() const;
    std::vector<ExcludePairCommandEntry> selectedExclusionEntries(
        std::size_t& affectedOccurrencePairs,
        QString& errorMessage
    ) const;
    bool createReviewNoteForCurrentRow(QString& errorMessage);
    int currentPairIndex() const;
    int currentFaceHitIndex() const;
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
    GuiThreadScanFailureInjection testGuiThreadScanFailureInjection;
    int ownedScanWatcherCount_ = 0;
    bool hasTestSelectionOverride = false;
    std::vector<Assembly::InterferenceSelectionHandle> testSelectionHandles;
    /** True while DocumentObject-backed snapshot preparation is running (Cancel inactive). */
    bool preparingScan = false;
    Assembly::InterferenceScanSession session;
    Gui::QuantitySpinBox* clearanceSpin = nullptr;
    QComboBox* clearanceSheetCombo = nullptr;
    QPushButton* createClearanceSheetButton = nullptr;
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
    QPushButton* createReviewNoteButton = nullptr;
    QPushButton* excludeButton = nullptr;
    QPushButton* restoreButton = nullptr;
    QPushButton* manageExclusionsButton = nullptr;

    Assembly::InterferenceScanResult lastResult;
    /** True after the active scan generation finishes via finishScan() and is not cancelled. */
    bool hasAcceptedScanResult = false;
    /**
     * ReviewNote creation changes only annotation/group metadata. Its document
     * signals are synchronous, so suppress result invalidation only for the
     * duration of that known operation.
     */
    bool suppressResultInvalidation = false;
    SoSeparator* previewRoot = nullptr;
    SoGroup* attachedScene = nullptr;
    Gui::View3DInventorViewer* attachedViewer = nullptr;
    QPointer<Gui::View3DInventor> attachedView;
    QMetaObject::Connection attachedViewDestroyedConnection;
    QPointer<QDialog> manageExclusionsDialog;
    std::set<const App::Document*> watchedDocuments;

    std::vector<fastsignals::connection> connections;
};

/**
 * Persistent content for the Assembly interference dock.
 *
 * Hiding or moving the surrounding QDockWidget does not destroy this object, so
 * an active worker continues until it completes, the user presses Cancel scan,
 * or its source document changes.
 */
class AssemblyGuiExport InterferenceCheckPanel: public QWidget
{
    Q_OBJECT

public:
    explicit InterferenceCheckPanel(QWidget* parent = nullptr);
    ~InterferenceCheckPanel() override;

    TaskInterferenceCheck* openCheck(App::DocumentObject* host);
    TaskInterferenceCheck* openCheck(
        App::DocumentObject* host,
        const Assembly::InterferenceComponentOccurrence& componentA,
        const Assembly::InterferenceComponentOccurrence& componentB
    );
    TaskInterferenceCheck* currentCheck() const;

private:
    TaskInterferenceCheck* openCheckImpl(
        App::DocumentObject* host,
        const Assembly::InterferenceComponentOccurrence& componentA,
        const Assembly::InterferenceComponentOccurrence& componentB
    );

    QPointer<TaskInterferenceCheck> widget;
};

/**
 * Show or reactivate the persistent interference dock. If a different request
 * arrives while a scan is active, the running scan and its original scope win.
 */
AssemblyGuiExport TaskInterferenceCheck*
showInterferenceCheckPanel(App::DocumentObject* host);
AssemblyGuiExport TaskInterferenceCheck* showInterferenceCheckPanel(
    App::DocumentObject* host,
    const Assembly::InterferenceComponentOccurrence& componentA,
    const Assembly::InterferenceComponentOccurrence& componentB
);

}  // namespace AssemblyGui
