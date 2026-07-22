// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DlgMutationTakeover.h"

#include <App/Document.h>
#include <App/DocumentMutationAuthority.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/PyObjectBase.h>
#include <Gui/MainWindow.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Gui;
using namespace Gui::Dialog;

namespace
{

bool syncMcpLeaseTakeover(App::Document* document)
{
    if (!document) {
        return false;
    }
    try {
        Base::PyGILStateLocker lock;
        PyObject* module = nullptr;
        // Prefer in-addon bridge module (Mod path / addon import).
        module = PyImport_ImportModule("document_lease.core_authority");
        if (!module) {
            PyErr_Clear();
            module = PyImport_ImportModule("addon.FreeCADMCP.document_lease.core_authority");
        }
        if (!module) {
            PyErr_Clear();
            // Soft-compat: no MCP addon loaded; core takeover alone is fine.
            return true;
        }
        PyObject* func = PyObject_GetAttrString(module, "sync_gui_lease_takeover");
        Py_DECREF(module);
        if (!func || !PyCallable_Check(func)) {
            Py_XDECREF(func);
            PyErr_Clear();
            return true;
        }
        PyObject* docPy = document->getPyObject();
        PyObject* result = PyObject_CallFunctionObjArgs(func, docPy, nullptr);
        Py_DECREF(func);
        Py_XDECREF(docPy);
        if (!result) {
            Base::PyException exc;
            Base::Console().warning("MCP lease takeover sync failed: %s\n",
                                    exc.what());
            return false;
        }
        const bool ok = PyObject_IsTrue(result) == 1;
        Py_DECREF(result);
        return ok;
    }
    catch (const Base::Exception& e) {
        Base::Console().warning("MCP lease takeover sync failed: %s\n", e.what());
        return false;
    }
    catch (...) {
        Base::Console().warning("MCP lease takeover sync failed with unknown error\n");
        return false;
    }
}

}  // namespace

DlgMutationTakeover::DlgMutationTakeover(App::Document* document,
                                         const std::string& operation,
                                         QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Document controlled by MCP"));
    setModal(true);

    auto& authority = App::DocumentMutationAuthority::instance();
    const std::string docName = document ? document->Label.getValue() : std::string("<unknown>");
    const std::string owner = document ? authority.providerId(*document) : std::string("MCP Agent");
    const auto generation = document ? authority.fencingGeneration(*document) : 0;

    auto* layout = new QVBoxLayout(this);
    auto* text = new QLabel(this);
    text->setWordWrap(true);
    text->setText(tr(
        "This document is currently controlled by an MCP agent.\n\n"
        "Document: %1\n"
        "Operation: %2\n"
        "Owner: %3\n"
        "Fencing generation: %4\n\n"
        "Take over to revoke the agent and continue editing locally.")
                      .arg(QString::fromStdString(docName),
                           QString::fromStdString(operation),
                           QString::fromStdString(owner.empty() ? "MCP Agent" : owner),
                           QString::number(static_cast<qulonglong>(generation))));
    layout->addWidget(text);

    auto* buttons = new QHBoxLayout();
    auto* takeOverBtn = new QPushButton(tr("Take Over"), this);
    auto* cancelBtn = new QPushButton(tr("Cancel"), this);
    takeOverBtn->setDefault(true);
    buttons->addStretch();
    buttons->addWidget(takeOverBtn);
    buttons->addWidget(cancelBtn);
    layout->addLayout(buttons);

    connect(cancelBtn, &QPushButton::clicked, this, [this]() {
        _result = Result::Cancel;
        reject();
    });
    connect(takeOverBtn, &QPushButton::clicked, this, [this]() {
        _result = Result::TakeOver;
        accept();
    });
}

DlgMutationTakeover::Result DlgMutationTakeover::ask(App::Document* document,
                                                     const std::string& operation,
                                                     QWidget* parent)
{
    DlgMutationTakeover dialog(document, operation, parent ? parent : getMainWindow());
    dialog.exec();
    if (dialog.resultChoice() != Result::TakeOver || !document) {
        return dialog.resultChoice();
    }
    // Lease token rotation / sidecar first so core and lease cannot split.
    if (!syncMcpLeaseTakeover(document)) {
        Base::Console().warning(
            "Take over aborted: could not synchronize MCP lease authority\n");
        return Result::Cancel;
    }
    App::DocumentMutationAuthority::instance().takeover(*document);
    return Result::TakeOver;
}
