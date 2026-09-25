// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <App/DocumentCommand.h>
#include <App/DocumentCommandHandle.h>

#include <FCGlobal.h>

#include <vector>

namespace App
{
class Document;
class DocumentObject;
}  // namespace App

namespace Gui
{

/** Pointer-free recompute admission request for the document execution lane. */
struct DocumentRecomputeSubmitRequest
{
    std::vector<App::DocumentObject*> objects;
    bool force = false;
    int options = 0;
};

/** Build one recompute command from stable document/object identities. */
AppExport App::DocumentCommand makeDocumentRecomputeCommand(
    App::Document& document,
    const DocumentRecomputeSubmitRequest& request = {});

/** Surface a visible Busy or admission failure reason without waiting. */
AppExport void reportDocumentCommandSubmitBlocked(
    App::Document& document,
    const App::DocumentCommandSubmitOutcome& outcome,
    bool quiet = false);

/**
 * Admit one recompute through DocumentHandle::trySubmit().
 *
 * Returns true when the command is Accepted. Passive status refresh may
 * continue on a timer; status() is observation-only and never pumps work.
 */
AppExport bool trySubmitDocumentRecompute(
    App::Document& document,
    const DocumentRecomputeSubmitRequest& request = {});

/** Convenience wrapper that reports Busy and other rejections immediately. */
AppExport bool requestDocumentRecompute(
    App::Document& document,
    const std::vector<App::DocumentObject*>& objects = {},
    bool force = false,
    int options = 0,
    bool quiet = false);

}  // namespace Gui
