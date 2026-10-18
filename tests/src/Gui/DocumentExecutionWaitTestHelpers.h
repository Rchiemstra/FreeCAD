// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <chrono>
#include <string>

namespace App
{
class Document;
struct RecoverySnapshotSaveOptions;
}

namespace Gui::Test
{

/** Test-only nested Qt wait for lane Save completion in close/save scenarios. */
bool submitDocumentSaveAwaitingCompletion(
    App::Document& document,
    std::string* failureDiagnostic = nullptr);

/**
 * Test-only nested Qt wait for recovery completion. On timeout, throws while
 * the production scheduler's detached worker finishes with shared state and
 * its retained lane. A lifecycle pin rejects document close until the queued
 * GUI completion releases it. The legacy no-lane path completes synchronously.
 */
bool writeRecoverySnapshotAwaitingOwnerThread(
    App::Document& document,
    const App::RecoverySnapshotSaveOptions& options,
    std::chrono::milliseconds timeout = std::chrono::seconds(120));

}  // namespace Gui::Test
