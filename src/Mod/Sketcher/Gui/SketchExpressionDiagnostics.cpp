// SPDX-License-Identifier: LGPL-2.1-or-later
#include <string>
#include <QBrush>
#include <QCoreApplication>
#include <QStringList>
#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentWouldBlock.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/DocumentExecutionIngress.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Gui/Selection/Selection.h>
#include <Mod/Sketcher/App/Constraint.h>
#include <Mod/Sketcher/App/SketchObject.h>

#include "SketchExpressionDiagnostics.h"
#include "EditModeCoinManagerParameters.h"
#include "ViewProviderSketch.h"

using namespace SketcherGui;

SketchExpressionDiagnostics::~SketchExpressionDiagnostics()
{
    stop();
}

void SketchExpressionDiagnostics::start(ViewProviderSketch* provider)
{
    stop();
    view = provider;
    dirty = true;
    timer.setSingleShot(true);
    timer.setInterval(25);
    QObject::connect(&timer, &QTimer::timeout, &timer, [this]() {
        // The provider owns this timer. Reserve reads before inspecting the edit
        // object or entering any solver/Qt/Coin presentation code.
        auto admission = readAdmission();
        if (admission && view && view->isInEditMode()) {
            view->slotSolverUpdate();
        }
    });
    auto& app = App::GetApplication();
    connections.push_back(app.signalChangedObject.connect(
        [this, sketch = provider->getObject()](const App::DocumentObject& object,
                                              const App::Property& property) {
            // Notification arguments are borrowed only for this callback. No
            // document traversal or evaluation occurs under the sender's lock.
            const char* name = property.getName();
            if (name && std::string_view(name) == "Visibility") {
                return;
            }
            if (&object == sketch && name
                && (std::string_view(name) == "Geometry"
                    || std::string_view(name) == "FullyConstrained")) {
                checkSolverOutputs = true;
                requestUpdate();
                return;
            }
            invalidate(&object == sketch && name && std::string_view(name) == "Constraints");
        }));
    connections.push_back(app.signalNewObject.connect(
        [this](const App::DocumentObject&) { invalidate(); }));
    connections.push_back(app.signalDeletedObject.connect(
        [this](const App::DocumentObject&) { invalidate(); }));
    // A failed expression can skip SketchObject::execute() and all solver
    // notifications. Completion still invalidates the captured report.
    connections.push_back(app.signalRecomputed.connect(
        [this](const App::Document&) { invalidate(); }));
    requestUpdate();
}

void SketchExpressionDiagnostics::stop()
{
    timer.stop();
    QObject::disconnect(&timer, nullptr, &timer, nullptr);
    for (auto& connection : connections) {
        connection.disconnect();
    }
    connections.clear();
    view = nullptr;
}

void SketchExpressionDiagnostics::invalidate(bool constraintsOnly)
{
    if (constraintsOnly) {
        checkConstraints = true;
    }
    else {
        dirty = true;
    }
    requestUpdate();
}

void SketchExpressionDiagnostics::requestUpdate()
{
    // Raw SketchObject solver signals can originate on the document owner.
    // Queue against the timer's QObject lifetime, never retain live model args.
    QMetaObject::invokeMethod(&timer, [this]() {
        if (view && !timer.isActive()) {
            timer.start();
        }
    }, Qt::QueuedConnection);
}

std::unique_ptr<App::LivePresentationRefreshScope> SketchExpressionDiagnostics::readAdmission()
{
    if (!App::DocumentWouldBlock::isGuiThread()) {
        requestUpdate();
        return {};
    }
    auto admission = std::make_unique<App::LivePresentationRefreshScope>();
    if (!admission->acquired()) {
        requestUpdate();
        return {};
    }
    // diagnose() follows references into open external documents too. Match
    // Gui::Document's idle read checks for every possible source while holding
    // the process reservation against new mutation admission.
    for (auto* document : App::GetApplication().getDocuments()) {
        auto* guiDocument = Gui::Application::Instance->getDocument(document);
        if (Gui::documentExecutionLaneBusy(*document)
            || (guiDocument && guiDocument->deferLivePresentationUpdates())) {
            requestUpdate();
            return {};
        }
    }
    return admission;
}

bool SketchExpressionDiagnostics::refresh(Sketcher::SketchObject* sketch)
{
    auto admission = readAdmission();
    if (!admission) {
        return false;
    }
    std::vector<ConstraintInput> inputs;
    if (dirty || checkConstraints) {
        for (const auto* constraint : sketch->Constraints.getValues()) {
            inputs.emplace_back(constraint->Name, constraint->toString(), constraint->getValue(),
                                constraint->isDriving, constraint->isActive);
        }
        dirty = dirty || inputs != constraintInputs;
        checkConstraints = false;
    }
    if (!dirty && !checkSolverOutputs) {
        return true;
    }
    Base::PyGILStateLocker lock;
    try {
        Py::Module module(PyImport_ImportModule("freecad.sketcher_expression_diagnostics"), true);
        Py::Tuple args(1);
        args.setItem(0, Py::Object(sketch->getPyObject(), true));
        if (checkSolverOutputs) {
            if (!dirty) {
                dirty = Py::Object(Py::Callable(module.getAttr("references_solver_outputs"))
                                       .apply(args)).isTrue();
            }
            checkSolverOutputs = false;
        }
        if (!dirty) {
            return true;
        }
        Py::Dict result(Py::Callable(module.getAttr("constraint_errors")).apply(args));
        Errors captured;
        PyObject* key = nullptr;
        PyObject* value = nullptr;
        Py_ssize_t position = 0;
        while (PyDict_Next(result.ptr(), &position, &key, &value)) {
            const long index = PyLong_AsLong(key);
            const char* text = PyUnicode_AsUTF8(value);
            if (PyErr_Occurred()) {
                throw Py::Exception();
            }
            captured.emplace(static_cast<int>(index), QString::fromUtf8(text));
        }
        // Publish only complete, pointer-free data. Busy/failed captures keep
        // the previous immutable snapshot and remain invalidated for retry.
        constraintErrors = std::make_shared<const Errors>(std::move(captured));
        constraintInputs = std::move(inputs);
        dirty = false;
        return true;
    }
    catch (Py::Exception&) {
        Base::PyException error;
        error.reportException();
        requestUpdate();
        return false;
    }
}

QString SketchExpressionDiagnostics::reason(int index) const
{
    auto found = constraintErrors->find(index);
    return found == constraintErrors->end() ? QString() : found->second;
}

QString SketchExpressionDiagnostics::editFailure(Sketcher::SketchObject* sketch, const QString& error)
{
    App::LivePresentationRefreshScope admission;
    if (!admission.acquired() || !Gui::Application::Instance) {
        return error;
    }
    auto* guiDocument = Gui::Application::Instance->getDocument(sketch->getDocument());
    auto* provider = guiDocument
        ? freecad_cast<ViewProviderSketch*>(guiDocument->getViewProvider(sketch)) : nullptr;
    if (!provider) {
        return error;
    }
    const auto& diagnostics = provider->getExpressionDiagnostics();
    if (diagnostics.errors().empty()) {
        return error;
    }
    QString message = QCoreApplication::translate(
        "SketcherExpressionDiagnostics", "Constraint edit failed. The sketch has broken expressions:");
    for (const auto& [index, reason] : diagnostics.errors()) {
        QString name = QCoreApplication::translate("SketcherExpressionDiagnostics", "Constraint %1")
                           .arg(index + 1);
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
