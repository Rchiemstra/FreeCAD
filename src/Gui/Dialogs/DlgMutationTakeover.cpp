// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DlgMutationTakeover.h"

#include <App/Document.h>
#include <App/DocumentMutationAuthority.h>
#include <Gui/MainWindow.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Gui;
using namespace Gui::Dialog;

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
    auto* inspectBtn = new QPushButton(tr("Inspect"), this);
    auto* pauseBtn = new QPushButton(tr("Request Pause"), this);
    auto* takeOverBtn = new QPushButton(tr("Take Over"), this);
    auto* cancelBtn = new QPushButton(tr("Cancel"), this);
    takeOverBtn->setDefault(true);
    buttons->addWidget(inspectBtn);
    buttons->addWidget(pauseBtn);
    buttons->addStretch();
    buttons->addWidget(takeOverBtn);
    buttons->addWidget(cancelBtn);
    layout->addLayout(buttons);

    connect(cancelBtn, &QPushButton::clicked, this, [this]() {
        _result = Result::Cancel;
        reject();
    });
    connect(inspectBtn, &QPushButton::clicked, this, [this]() {
        _result = Result::Inspect;
        reject();
    });
    connect(pauseBtn, &QPushButton::clicked, this, [this]() {
        _result = Result::RequestPause;
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
    if (dialog.resultChoice() == Result::TakeOver && document) {
        App::DocumentMutationAuthority::instance().takeover(*document);
    }
    return dialog.resultChoice();
}
