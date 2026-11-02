// SPDX-License-Identifier: LGPL-2.1-or-later

#include <algorithm>
#include <memory>
#include <set>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <Standard_Failure.hxx>
#include <nlohmann/json.hpp>
#include <App/DocumentObserver.h>
#include <Base/Console.h>
#include <Gui/Application.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/QuantitySpinBox.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Selection/SelectionObject.h>
#include <Mod/Sketcher/App/LinkedOffset.h>
#include <Mod/Sketcher/App/SketchObject.h>
#include "CommandSketcherLinkedOffset.h"
#include "ViewProviderSketch.h"

namespace SketcherGui
{
namespace
{
using Operation = Sketcher::LinkedOffsetOperation;

Sketcher::SketchObject* target()
{
    auto selection = Gui::Selection().getSelectionEx(
        0, Sketcher::SketchObject::getClassTypeId());
    if (selection.size() > 1) {
        return nullptr;
    }
    if (selection.size() == 1) {
        return static_cast<Sketcher::SketchObject*>(selection.front().getObject());
    }
    auto* document = Gui::Application::Instance->activeDocument();
    auto* edit = document ? dynamic_cast<ViewProviderSketch*>(document->getInEdit()) : nullptr;
    return edit ? static_cast<Sketcher::SketchObject*>(edit->getObject()) : nullptr;
}

std::vector<std::string> selectedRelations(const Sketcher::SketchObject& sketch, bool all)
{
    const auto relations = Sketcher::listLinkedOffsets(sketch);
    std::set<int> indices;
    const auto selection = Gui::Selection().getSelectionEx(
        0, Sketcher::SketchObject::getClassTypeId());
    if (!all && selection.empty()) {
        throw std::invalid_argument("Select a linked offset source, output, or its sketch.");
    }
    bool wholeSketch = false;
    if (!all) {
        for (const auto& item : selection) {
            if (item.getObject() != &sketch) {
                continue;
            }
            wholeSketch |= item.getSubNames().empty();
            for (const auto& name : item.getSubNames()) {
                if (name.compare(0, 4, "Edge") == 0) {
                    size_t end = 0;
                    const int index = std::stoi(name.substr(4), &end) - 1;
                    if (end == name.size() - 4 && index >= 0
                        && index < sketch.Geometry.getSize()) {
                        indices.insert(index);
                    }
                }
            }
        }
    }
    std::vector<std::string> ids;
    for (const auto& relation : relations) {
        bool matches = all || wholeSketch;
        for (const auto index : indices) {
            const auto* curve = sketch.getGeometry(index);
            if (curve->hasExtension(Sketcher::LinkedOffsetExtension::getClassTypeId())) {
                const auto ext = std::dynamic_pointer_cast<const Sketcher::LinkedOffsetExtension>(
                    curve->getExtension(Sketcher::LinkedOffsetExtension::getClassTypeId()).lock());
                matches |= ext && ext->membership == relation.relation.id;
            }
            matches |= std::find(relation.sources.begin(), relation.sources.end(), index)
                != relation.sources.end();
        }
        if (matches) {
            ids.push_back(relation.relation.id);
        }
    }
    if (ids.empty()) {
        throw std::invalid_argument("Select a linked offset output, source, or its sketch.");
    }
    return ids;
}

QString inspectText(const Sketcher::SketchObject& sketch, const std::vector<std::string>& ids)
{
    QString text = QObject::tr("Explicit refresh only. Source edits keep the last valid offset "
                               "until you press Refresh.\n");
    for (const auto& relation : nlohmann::json::parse(Sketcher::linkedOffsetsJson(sketch))) {
        const auto id = relation["id"].get<std::string>();
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            continue;
        }
        const auto reason = relation.value("refreshReason", relation.value("reason", ""));
        text += QString::fromStdString(id + ": " + (reason.empty() ? "Ready" : reason)) + '\n';
    }
    return text;
}

void showOperation(Sketcher::SketchObject& sketch, const std::vector<std::string>& ids,
                   bool edit, const QString& failure = {})
{
    auto* dialog = new QDialog(Gui::getMainWindow());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("LinkedOffsetEdit"));
    dialog->setWindowTitle(QObject::tr("Linked offset — explicit refresh"));
    auto* layout = new QVBoxLayout(dialog);
    auto* status = new QLabel(failure.isEmpty() ? inspectText(sketch, ids) : failure, dialog);
    status->setObjectName(QStringLiteral("LinkedOffsetStatus"));
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    auto* form = new QFormLayout();
    layout->addLayout(form);
    const auto relations = Sketcher::listLinkedOffsets(sketch);
    const auto found = std::find_if(relations.begin(), relations.end(), [&](const auto& item) {
        return item.relation.id == ids.front();
    });
    const auto parameters = found->relation.parameters;
    const auto quantity = [&](const char* name, const QString& label, double value) {
        auto* box = new Gui::QuantitySpinBox(dialog);
        box->setObjectName(QString::fromLatin1(name));
        box->setDecimals(7);
        box->setRange(-1e9, 1e9);
        box->setUnit(Base::Unit::Length);
        box->setValue(value);
        form->addRow(label, box);
        box->setVisible(edit);
        return box;
    };
    auto* distance = quantity("LinkedOffsetDistance", QObject::tr("Signed distance"),
                              parameters.distance);
    auto* tolerance = quantity("LinkedOffsetTolerance", QObject::tr("Tolerance"),
                               parameters.tolerance);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    auto object = std::make_shared<App::DocumentObjectWeakPtrT>(&sketch);
    const auto run = [=](Operation operation) {
        try {
            auto* live = static_cast<Sketcher::SketchObject*>(**object);
            if (!live) {
                throw std::runtime_error("The sketch has been deleted.");
            }
            if (operation == Operation::Edit
                && (!distance->hasValidInput() || !tolerance->hasValidInput())) {
                throw std::invalid_argument("Enter valid length quantities.");
            }
            Sketcher::operateLinkedOffsets(*live, ids, operation, distance->rawValue(),
                                           tolerance->rawValue());
            dialog->accept();
        }
        catch (const Standard_Failure& error) {
            status->setText(QString::fromUtf8(error.GetMessageString()));
        }
        catch (const std::exception& error) {
            status->setText(QString::fromUtf8(error.what()));
        }
    };
    auto* refresh = buttons->addButton(QObject::tr("Refresh explicitly"),
                                       QDialogButtonBox::ActionRole);
    refresh->setObjectName(QStringLiteral("LinkedOffsetRefresh"));
    QObject::connect(refresh, &QPushButton::clicked, dialog, [=]() {
        run(edit ? Operation::Edit : Operation::Refresh);
    });
    auto* detach = buttons->addButton(QObject::tr("Detach"), QDialogButtonBox::ActionRole);
    detach->setObjectName(QStringLiteral("LinkedOffsetDetach"));
    QObject::connect(detach, &QPushButton::clicked, dialog, [=]() { run(Operation::Detach); });
    dialog->show();
}

class LinkedOffsetCommand: public Gui::Command
{
public:
    LinkedOffsetCommand(const char* name, const char* label, Operation operation, bool all = false)
        : Gui::Command(name), operation(operation), all(all)
    {
        sAppModule = "Sketcher";
        sGroup = "Sketcher";
        sMenuText = label;
        sToolTipText = QT_TR_NOOP("Manage persistent offsets with explicit refresh. "
                                 "Select source/output edges or a sketch. No automatic refresh.");
        sStatusTip = sToolTipText;
        sPixmap = "Sketcher_Offset";
        eType = ForEdit | NoTransaction;
    }

    const char* className() const override
    {
        return "LinkedOffsetCommand";
    }

protected:
    bool isActive() override
    {
        const auto* sketch = target();
        return sketch && !Sketcher::listLinkedOffsets(*sketch).empty();
    }

    void activated(int) override
    {
        auto* sketch = target();
        if (!sketch) {
            return;
        }
        std::vector<std::string> ids;
        try {
            ids = selectedRelations(*sketch, all);
            if (operation == Operation::Edit) {
                showOperation(*sketch, ids, true);
            }
            else {
                Sketcher::operateLinkedOffsets(*sketch, ids, operation);
            }
        }
        catch (const Standard_Failure& error) {
            if (!ids.empty()) {
                showOperation(*sketch, ids, false, QString::fromUtf8(error.GetMessageString()));
            }
        }
        catch (const std::exception& error) {
            if (ids.empty()) {
                // A selected unrelated edge must never turn into Refresh all.
                Base::Console().warning("Linked offset: %s\n", error.what());
            }
            else {
                showOperation(*sketch, ids, false, QString::fromUtf8(error.what()));
            }
        }
    }

private:
    Operation operation;
    bool all;
};
}  // namespace

void createLinkedOffsetCommands()
{
    auto& manager = Gui::Application::Instance->commandManager();
    manager.addCommand(new LinkedOffsetCommand("Sketcher_LinkedOffsetRefresh",
        QT_TR_NOOP("Refresh selected linked offsets explicitly"), Operation::Refresh));
    manager.addCommand(new LinkedOffsetCommand("Sketcher_LinkedOffsetRefreshAll",
        QT_TR_NOOP("Refresh all linked offsets explicitly"), Operation::Refresh, true));
    manager.addCommand(new LinkedOffsetCommand("Sketcher_LinkedOffsetEdit",
        QT_TR_NOOP("Edit linked offset distance and tolerance"), Operation::Edit));
    manager.addCommand(new LinkedOffsetCommand("Sketcher_LinkedOffsetDetach",
        QT_TR_NOOP("Detach linked offsets"), Operation::Detach));
}
}  // namespace SketcherGui
