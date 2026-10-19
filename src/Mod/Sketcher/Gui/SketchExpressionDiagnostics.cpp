// SPDX-License-Identifier: LGPL-2.1-or-later
#include <string>
#include <QBrush>
#include <QCoreApplication>
#include <QStringList>
#include <App/Document.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Gui/Selection/Selection.h>
#include <Mod/Sketcher/App/Constraint.h>
#include <Mod/Sketcher/App/SketchObject.h>

#include "SketchExpressionDiagnostics.h"
#include "EditModeCoinManagerParameters.h"
#include "ViewProviderSketch.h"

using namespace SketcherGui;

void SketchExpressionDiagnostics::refresh(Sketcher::SketchObject* sketch)
{
    constraintErrors.clear();
    Base::PyGILStateLocker lock;
    try {
        Py::Module module(PyImport_ImportModule("freecad.sketcher_expression_diagnostics"), true);
        Py::Tuple args(1);
        args.setItem(0, Py::Object(sketch->getPyObject(), true));
        Py::Dict result(Py::Callable(module.getAttr("constraint_errors")).apply(args));
        PyObject* key = nullptr;
        PyObject* value = nullptr;
        Py_ssize_t position = 0;
        while (PyDict_Next(result.ptr(), &position, &key, &value)) {
            const long index = PyLong_AsLong(key);
            const char* text = PyUnicode_AsUTF8(value);
            if (PyErr_Occurred()) {
                throw Py::Exception();
            }
            constraintErrors.emplace(static_cast<int>(index), QString::fromUtf8(text));
        }
    }
    catch (Py::Exception&) {
        constraintErrors.clear();
        Base::PyException error;
        error.reportException();
    }
}

QString SketchExpressionDiagnostics::reason(int index) const
{
    auto found = constraintErrors.find(index);
    return found == constraintErrors.end() ? QString() : found->second;
}

QString SketchExpressionDiagnostics::editFailure(Sketcher::SketchObject* sketch, const QString& error)
{
    SketchExpressionDiagnostics diagnostics;
    diagnostics.refresh(sketch);
    if (diagnostics.errors().empty()) {
        return error;
    }
    QString message = QCoreApplication::translate(
        "SketcherExpressionDiagnostics", "Constraint edit failed. The sketch has broken expressions:");
    for (const auto& [index, reason] : diagnostics.errors()) {
        const auto* constraint = sketch->Constraints[index];
        QString name = QCoreApplication::translate("SketcherExpressionDiagnostics", "Constraint %1")
                           .arg(index + 1);
        if (!constraint->Name.empty()) {
            name += QStringLiteral(" (%1)").arg(QString::fromStdString(constraint->Name));
        }
        message += QStringLiteral("\n%1: %2").arg(name, reason);
    }
    return message + QStringLiteral("\n\n") + error;
}

QVariant SketcherGui::constraintExpressionData(const SketchExpressionDiagnostics& diagnostics,
                                               const std::vector<int>& indices, int role)
{
    QStringList reasons;
    for (int index : indices) {
        const auto reason = diagnostics.reason(index);
        if (!reason.isEmpty()) {
            reasons << reason;
        }
    }
    if (reasons.isEmpty()) {
        return {};
    }
    if (role == Qt::ForegroundRole) {
        const auto& color = DrawingParameters::BrokenExpressionConstrDimColor;
        return QBrush(QColor::fromRgbF(color[0], color[1], color[2]));
    }
    return QCoreApplication::translate("SketcherExpressionDiagnostics", "Broken expression")
        + QLatin1Char('\n') + reasons.join(QLatin1Char('\n'));
}

SketchExpressionDiagnosticsLabel::SketchExpressionDiagnosticsLabel(
    ViewProviderSketch* view, QWidget* parent)
    : QLabel(parent), view(view)
{
    setObjectName(QStringLiteral("brokenExpressionConstraints"));
    setWordWrap(true);
    setOpenExternalLinks(false);
    connect(this, &QLabel::linkActivated, this, [this](const QString&) {
        Gui::Selection().clearSelection();
        auto* sketch = this->view->getSketchObject();
        for (const auto& [index, reason] : this->view->getExpressionDiagnostics().errors()) {
            const std::string sub = "Constraint" + std::to_string(index + 1);
            Gui::Selection().addSelection(sketch->getDocument()->getName(),
                                         sketch->getNameInDocument(), sub.c_str());
        }
    });
    refresh();
}

void SketchExpressionDiagnosticsLabel::refresh()
{
    const auto& errors = view->getExpressionDiagnostics().errors();
    setVisible(!errors.empty());
    setText(QStringLiteral("<a href=\"#broken-expressions\">%1</a>").arg(
        (errors.size() == 1
             ? QCoreApplication::translate("SketcherExpressionDiagnostics",
                                           "1 constraint with a broken expression")
             : QCoreApplication::translate("SketcherExpressionDiagnostics",
                                           "%n constraints with broken expressions", nullptr,
                                           static_cast<int>(errors.size()))).toHtmlEscaped()));
    QStringList reasons;
    for (const auto& [index, reason] : errors) {
        reasons << QStringLiteral("%1: %2").arg(index + 1).arg(reason);
    }
    setToolTip(reasons.join(QLatin1Char('\n')));
}
