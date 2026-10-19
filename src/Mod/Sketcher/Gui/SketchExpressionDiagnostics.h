// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <map>
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
    void refresh(Sketcher::SketchObject* sketch);
    QString reason(int index) const;
    const std::map<int, QString>& errors() const { return constraintErrors; }
    static QString editFailure(Sketcher::SketchObject* sketch, const QString& error);

private:
    std::map<int, QString> constraintErrors;
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
