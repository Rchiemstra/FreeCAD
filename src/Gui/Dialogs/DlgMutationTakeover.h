// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QDialog>
#include <FCGlobal.h>
#include <string>

class QLabel;

namespace App
{
class Document;
}

namespace Gui
{
namespace Dialog
{

/**
 * Prompt shown when a GUI command attempts to mutate an MCP-owned document.
 * Returns Accepted only when the user chooses Take Over.
 */
class GuiExport DlgMutationTakeover: public QDialog
{
    Q_OBJECT
public:
    enum class Result
    {
        Cancel = 0,
        TakeOver = 1,
        Inspect = 2,
        RequestPause = 3,
    };

    explicit DlgMutationTakeover(App::Document* document,
                                 const std::string& operation,
                                 QWidget* parent = nullptr);

    Result resultChoice() const
    {
        return _result;
    }

    static Result ask(App::Document* document, const std::string& operation, QWidget* parent = nullptr);

private:
    Result _result {Result::Cancel};
};

}  // namespace Dialog
}  // namespace Gui
