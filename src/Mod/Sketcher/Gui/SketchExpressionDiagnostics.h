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

    // Constraint rows share this object. refresh() replaces errors with a new
    // immutable map; a row that outlives the view provider keeps the last map.
    struct ErrorPublication
    {
        std::shared_ptr<const Errors> errors = std::make_shared<const Errors>();
    };

    ~SketchExpressionDiagnostics();
    void start(ViewProviderSketch* view);
    void stop();
    void requestUpdate();
    std::unique_ptr<App::LivePresentationRefreshScope> readAdmission();
    bool refresh(Sketcher::SketchObject* sketch);
    QString reason(int index) const;
    std::shared_ptr<ErrorPublication> shareErrors() const
    {
        return publication;
    }
    std::shared_ptr<const Errors> errorsSnapshot() const
    {
        return publication->errors;
    }
    static QString editFailure(Sketcher::SketchObject* sketch, const QString& error);

private:
    void invalidate(bool constraintsOnly = false);
    using ConstraintInput = std::tuple<std::string, std::string, double, bool, bool>;
    std::vector<ConstraintInput> constraintInputs;
    std::shared_ptr<ErrorPublication> publication = std::make_shared<ErrorPublication>();
    ViewProviderSketch* view = nullptr;
    QTimer timer;
    std::vector<fastsignals::connection> connections;
    bool dirty = true;
    bool checkConstraints = false;
    bool checkSolverOutputs = false;
};

QVariant constraintExpressionData(std::shared_ptr<const SketchExpressionDiagnostics::Errors> errors,
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
