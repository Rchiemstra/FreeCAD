// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <deque>
#include <map>

#include <QString>
#include <QWidget>
#include <fastsignals/signal.h>

#include <FCGlobal.h>

class QDockWidget;
class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;

namespace App
{
class Document;
struct DocumentSaveOutcome;
}

namespace Gui
{
class Document;
class StatusBarLabel;

/** Session-local observer for authoritative persistence and mutation health. */
class GuiExport DocumentChangesWidget final: public QWidget
{
public:
    explicit DocumentChangesWidget(StatusBarLabel* statusLabel, QWidget* parent = nullptr);
    ~DocumentChangesWidget() override;

    void setDockContainer(QDockWidget* dock);
    void showPanel();

private:
    struct ObservedDocumentConnections
    {
        fastsignals::connection fileState;
        fastsignals::connection saveOutcome;
        fastsignals::connection camera;
    };

    void observeDocument(Gui::Document& document);
    void forgetDocument(const Gui::Document& document);
    void bindDocument(Gui::Document* document);
    void refresh();
    void refreshHistory();
    void appendHistory(const App::Document& document, const QString& entry);
    void onFileStateChanged(const App::Document& document);
    void onSaveOutcome(const App::Document& document, const App::DocumentSaveOutcome& outcome);
    void onCameraActivity(const Gui::Document& document);

    StatusBarLabel* statusLabel {nullptr};
    QDockWidget* dockContainer {nullptr};
    Gui::Document* activeGuiDocument {nullptr};
    App::Document* activeDocument {nullptr};
    QLabel* pathValue {nullptr};
    QLabel* stateValue {nullptr};
    QLabel* categoriesValue {nullptr};
    QLabel* cameraNote {nullptr};
    QLabel* readinessValue {nullptr};
    QLabel* operationValue {nullptr};
    QCheckBox* pauseAgentWrites {nullptr};
    QPushButton* clearHistoryButton {nullptr};
    QListWidget* historyList {nullptr};
    QTimer* refreshTimer {nullptr};

    std::map<const App::Document*, std::deque<QString>> histories;
    std::map<const App::Document*, bool> cameraActivity;
    std::map<const App::Document*, ObservedDocumentConnections> observedDocuments;
    fastsignals::connection newDocumentConnection;
    fastsignals::connection activeDocumentConnection;
    fastsignals::connection deletedDocumentConnection;
};

}  // namespace Gui
