// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 FreeCAD Project Association                         *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <algorithm>

#include <App/Datums.h>
#include <Base/Tools.h>
#include <Gui/Application.h>
#include <Gui/CommandT.h>
#include <Gui/MainWindow.h>
#include <Gui/Notifications.h>
#include <Gui/PrefWidgets.h>
#include <Mod/Sketcher/App/SketchObject.h>

#include "EditDeltaPositionDialog.h"
#include "Utils.h"
#include "ViewProviderSketch.h"

using namespace SketcherGui;

namespace
{

Gui::PrefQuantitySpinBox* createDeltaSpinBox(Sketcher::SketchObject* sketch,
                                             int constraintNbr,
                                             const QByteArray& entryName,
                                             QWidget* parent)
{
    auto* spinBox = new Gui::PrefQuantitySpinBox(parent);

    Base::Quantity initValue;
    initValue.setUnit(Base::Unit::Length);
    initValue.setValue(sketch->Constraints[constraintNbr]->getValue());

    spinBox->setEntryName(entryName);
    spinBox->setParamGrpPath(QByteArray("User parameter:BaseApp/History/SketcherLength"));
    spinBox->setUnit(Base::Unit::Length);
    spinBox->setValue(initValue);
    spinBox->pushToHistory();
    spinBox->bind(sketch->Constraints.createPath(constraintNbr));

    return spinBox;
}

bool applyDeltaDatum(Sketcher::SketchObject* sketch,
                     int constraintNbr,
                     Gui::PrefQuantitySpinBox* spinBox)
{
    if (!spinBox->hasValidInput()) {
        Gui::TranslatedUserWarning(
            sketch,
            QObject::tr("Invalid datum"),
            QObject::tr("The Delta Position value is not a valid length."));
        return false;
    }

    Base::Quantity newQuant = spinBox->value();
    if (!newQuant.isDimensionlessOrUnit(Base::Unit::Length)) {
        Gui::TranslatedUserWarning(
            sketch,
            QObject::tr("Invalid datum"),
            QObject::tr("The Delta Position value must be a length."));
        return false;
    }

    spinBox->pushToHistory();

    if (spinBox->hasExpression()) {
        spinBox->apply();
        return true;
    }

    auto unitString = newQuant.getUnit().getString();
    unitString = Base::Tools::escapeQuotesFromString(unitString);

    Gui::cmdAppObjectArgs(sketch,
                          "setDatum(%i,App.Units.Quantity('%f %s'))",
                          constraintNbr,
                          newQuant.getValue(),
                          unitString);
    return true;
}

}  // namespace

EditDeltaPositionDialog::EditDeltaPositionDialog(int tid, ViewProviderSketch* vp, int ConstrNbr)
    : sketch(vp->getSketchObject())
    , transactionID(tid)
{
    auto pair = sketch->getDeltaPositionConstraintPair(ConstrNbr);
    xConstraintNbr = pair.first;
    yConstraintNbr = pair.second;
}

EditDeltaPositionDialog::EditDeltaPositionDialog(int tid,
                                                 Sketcher::SketchObject* pcSketch,
                                                 int ConstrNbr)
    : sketch(pcSketch)
    , transactionID(tid)
{
    auto pair = sketch->getDeltaPositionConstraintPair(ConstrNbr);
    xConstraintNbr = pair.first;
    yConstraintNbr = pair.second;
}

int EditDeltaPositionDialog::exec(bool atCursor)
{
    if (xConstraintNbr < 0 || yConstraintNbr < 0) {
        return QDialog::Rejected;
    }

    if (sketch->hasConflicts()) {
        Gui::TranslatedUserWarning(
            sketch,
            QObject::tr("Delta Position constraint"),
            QObject::tr("Not allowed to edit the datum because the sketch contains conflicting constraints"));
        return QDialog::Rejected;
    }

    QDialog dlg(Gui::getMainWindow());
    dlg.setWindowTitle(QObject::tr("Insert Delta Position"));

    auto* layout = new QVBoxLayout(&dlg);
    auto* formLayout = new QFormLayout();
    layout->addLayout(formLayout);

    auto* deltaXEdit = createDeltaSpinBox(sketch, xConstraintNbr, QByteArray("DeltaXValue"), &dlg);
    auto* deltaYEdit = createDeltaSpinBox(sketch, yConstraintNbr, QByteArray("DeltaYValue"), &dlg);
    formLayout->addRow(new QLabel(QObject::tr("Delta X:"), &dlg), deltaXEdit);
    formLayout->addRow(new QLabel(QObject::tr("Delta Y:"), &dlg), deltaYEdit);

    auto* buttonBox =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttonBox);

    auto formulaDialogOpened = [buttonBox](bool state) {
        buttonBox->setHidden(state);
    };
    QObject::connect(deltaXEdit, &Gui::QuantitySpinBox::showFormulaDialog, formulaDialogOpened);
    QObject::connect(deltaYEdit, &Gui::QuantitySpinBox::showFormulaDialog, formulaDialogOpened);

    QObject::connect(buttonBox, &QDialogButtonBox::accepted, [&]() {
        try {
            if (!applyDeltaDatum(sketch, xConstraintNbr, deltaXEdit)
                || !applyDeltaDatum(sketch, yConstraintNbr, deltaYEdit)) {
                return;
            }

            Gui::Command::commitCommand(transactionID);

            sketch->ExpressionEngine.execute();
            sketch->solve();
            tryAutoRecompute(sketch);
            success = true;
            dlg.accept();
        }
        catch (const Base::Exception& e) {
            Gui::NotifyUserError(sketch, QT_TRANSLATE_NOOP("Notifications", "Value Error"), e.what());

            Gui::Command::abortCommand(transactionID);

            if (sketch->noRecomputes) {
                sketch->solve();
            }
        }
    });

    QObject::connect(buttonBox, &QDialogButtonBox::rejected, [&]() {
        Gui::Command::abortCommand(transactionID);
        sketch->recomputeFeature();
        dlg.reject();
    });

    if (atCursor) {
        dlg.show();
        QRect pg = dlg.parentWidget()->geometry();
        int Xmin = pg.x() + 10;
        int Ymin = pg.y() + 10;
        int Xmax = pg.x() + pg.width() - dlg.geometry().width() - 10;
        int Ymax = pg.y() + pg.height() - dlg.geometry().height() - 10;
        int x = Xmax < Xmin ? (Xmin + Xmax) / 2 : std::min(std::max(QCursor::pos().x(), Xmin), Xmax);
        int y = Ymax < Ymin ? (Ymin + Ymax) / 2 : std::min(std::max(QCursor::pos().y(), Ymin), Ymax);
        dlg.setGeometry(x, y, dlg.geometry().width(), dlg.geometry().height());
    }

    return dlg.exec();
}

bool EditDeltaPositionDialog::isSuccess() const
{
    return success;
}
