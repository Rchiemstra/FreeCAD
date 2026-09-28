// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <App/Document.h>

namespace App
{
class Document;
}

namespace Gui::Test
{

/// Submits a recompute through the document execution handle and pumps the GUI
/// event loop until it completes, avoiding synchronous recompute() on the GUI thread.
void recomputeWithoutBlockingGui(App::Document& document,
                                 const char* coalescingKey = "gui-test-setup-recompute");

/// Submits save-as through the document execution handle and pumps the GUI event
/// loop until it completes, avoiding synchronous saveAs() on the GUI thread.
void saveAsWithoutBlockingGui(App::Document& document, const char* path);

App::DocumentSaveOutcome saveWithOutcomeWithoutBlockingGui(App::Document& document);

App::DocumentSaveOutcome saveAsWithOutcomeWithoutBlockingGui(App::Document& document,
                                                               const char* path,
                                                               bool overwrite = false);

}  // namespace Gui::Test
