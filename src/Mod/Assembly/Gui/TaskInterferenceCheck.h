// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QPointer>

#include <Base/Quantity.h>
#include <Base/Unit.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>
#include <Mod/Assembly/App/InterferenceScan.h>
#include <Mod/Assembly/App/InterferenceScanSession.h>
#include <fastsignals/connection.h>

class QCheckBox;
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

namespace Assembly
{
class AssemblyObject;
}

namespace AssemblyGui
{

class TaskInterferenceCheck: public QWidget
{
    Q_OBJECT

public:
    explicit TaskInterferenceCheck(Assembly::AssemblyObject* assembly, QWidget* parent = nullptr);
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
        return !lastResult.pairs.empty() || !lastResult.componentIssues.empty();
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
    QString testProgressText() const;
    int testTableRowCount() const;
    QString testTableCellText(int row, int column) const;
    std::size_t testResultPairCount() const;
    Base::Unit testClearanceUnit() const;
    double testClearanceRawMm() const;
    void testSetClearanceQuantity(const Base::Quantity& quantity);

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
    bool testHasAssembly() const;
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
    void onIncludeHiddenToggled(bool checked);
    void onRowChanged();
    void onScanFinished(std::uint64_t generation, const Assembly::InterferenceScanResult& result);
    void onScanProgress(int current, int total);

private:
    void setupUi();
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
    Gui::Document* assemblyGuiDocument() const;
    Gui::View3DInventorViewer* viewer() const;
    std::vector<std::pair<std::string, std::string>> currentExclusionSourceIds() const;
    App::DocumentObject* resolveSourceId(const std::string& sourceId) const;
    int currentPairIndex() const;
    QString formatLength(double mm) const;
    QString formatVolume(double cubicMm) const;
    QString formatPairDistance(const Part::InterferenceResult& detection) const;
    QString formatPairVolume(const Part::InterferenceResult& detection) const;

    Assembly::AssemblyObject* assembly = nullptr;
    Assembly::InterferenceScanSession session;
    Gui::QuantitySpinBox* clearanceSpin = nullptr;
    QCheckBox* includeHiddenCheck = nullptr;
    QCheckBox* showExcludedCheck = nullptr;
    QLabel* summaryLabel = nullptr;
    QLabel* statusLabel = nullptr;
    QLabel* progressLabel = nullptr;
    QTableWidget* resultsTable = nullptr;
    QPushButton* runButton = nullptr;
    QPushButton* cancelButton = nullptr;
    QPushButton* selectPairButton = nullptr;
    QPushButton* excludeButton = nullptr;
    QPushButton* restoreButton = nullptr;
    QPushButton* manageExclusionsButton = nullptr;

    Assembly::InterferenceScanResult lastResult;
    SoSeparator* previewRoot = nullptr;
    SoGroup* attachedScene = nullptr;
    Gui::View3DInventorViewer* attachedViewer = nullptr;
    QPointer<Gui::View3DInventor> attachedView;
    QPointer<QDialog> manageExclusionsDialog;
    std::set<const App::Document*> watchedDocuments;

    std::vector<fastsignals::connection> connections;
};

class TaskInterferenceCheckDialog: public Gui::TaskView::TaskDialog
{
    Q_OBJECT

public:
    explicit TaskInterferenceCheckDialog(Assembly::AssemblyObject* assembly);
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
