// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>
#include <QObject>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>

namespace App
{
class Document;
struct RecoverySnapshotSaveOptions;
}

namespace Gui
{
/** GUI-thread owner for recovery work dispatched to existing document lanes.
 * One outstanding write per document, at most maxConcurrent dispatched writes.
 * shutdown stops admission and drops callbacks, waiting at most the supplied
 * budget. Already running IO retains its document until it finishes.
 */
class GuiExport RecoveryWriteExecutor final: public QObject
{
public:
    using Completion = std::function<void(bool, std::exception_ptr)>;
    explicit RecoveryWriteExecutor(QObject* parent = nullptr, unsigned maxConcurrent = 2);
    ~RecoveryWriteExecutor() override;
    bool schedule(App::Document&, const App::RecoverySnapshotSaveOptions&, Completion);
    /** Cancel queued IO and all completion delivery for this document.
     * Already running IO keeps its own lifecycle admission until it retires.
     */
    void cancelDocument(App::Document&);
    bool isAccepting() const;
    /** Returns false when dispatched work outlives the total wait budget. */
    bool shutdown(std::chrono::milliseconds budget = std::chrono::milliseconds(100));
private:
    struct Private;
    std::unique_ptr<Private> d;
};
}
