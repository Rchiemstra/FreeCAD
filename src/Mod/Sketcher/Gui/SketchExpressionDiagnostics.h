// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <map>
#include <memory>
#include <tuple>
#include <string>
#include <QTimer>
#include <fastsignals/connection.h>
#include <App/MutationClassification.h>
#include <vector>
#include <QVariant>
#include <QLabel>
#include <QString>

namespace Sketcher { class SketchObject; }

namespace SketcherGui
{
class ViewProviderSketch;

// A presentation snapshot: never evaluate expressions from a paint/data method.
class SketchExpressionDiagnostics
{
public:
    using Errors = std::map<int, QString>;

    ~SketchExpressionDiagnostics();
    void start(ViewProviderSketch* view);
    void stop();
    void requestUpdate();
    std::unique_ptr<App::LivePresentationRefreshScope> readAdmission();
    bool refresh(Sketcher::SketchObject* sketch);
    QString reason(int index) const;
    const Errors& errors() const { return *constraintErrors; }
    static QString editFailure(Sketcher::SketchObject* sketch, const QString& error);

private:
    void invalidate(bool constraintsOnly = false);
    using ConstraintInput = std::tuple<std::string, std::string, double, bool, bool>;
    std::vector<ConstraintInput> constraintInputs;
    std::shared_ptr<const Errors> constraintErrors = std::make_shared<const Errors>();
    ViewProviderSketch* view = nullptr;
    QTimer timer;
    std::vector<fastsignals::connection> connections;
    bool dirty = true;
    bool checkConstraints = false;
    bool checkSolverOutputs = false;
};

QVariant constraintExpressionData(const SketchExpressionDiagnostics& diagnostics,
                                  const std::vector<int>& indices, int role);

class SketchExpressionDiagnosticsLabel: public QLabel
{
public:
    SketchExpressionDiagnosticsLabel(ViewProviderSketch* view, QWidget* parent);
    void refresh();

private:
    ViewProviderSketch* view;
};
} // namespace SketcherGui
