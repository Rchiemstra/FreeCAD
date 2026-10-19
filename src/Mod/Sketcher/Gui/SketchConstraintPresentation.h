// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <map>
#include <utility>
#include "SketchExpressionDiagnostics.h"

namespace SketcherGui
{
// Populated only under observation admission. Paint and tooltip requests use
// identities, expression icons and legacy roles from the same capture, even
// while an owner transaction temporarily removes or replaces live constraints.
struct SketchConstraintPresentation
{
    std::pair<int, int> pair {-1, -1};
    bool boundExpression = false;

    template<typename ReadRole>
    void capture(ReadRole read)
    {
        for (int role : {Qt::DisplayRole, Qt::EditRole, Qt::DecorationRole,
                         Qt::ForegroundRole, Qt::ToolTipRole}) {
            roles[role] = read(role);
        }
    }

    QVariant data(const SketchExpressionDiagnostics& diagnostics, const std::vector<int>& indices,
                  int role, const QVariant& fallback) const
    {
        if (role == Qt::ForegroundRole || role == Qt::ToolTipRole) {
            const auto diagnostic = constraintExpressionData(diagnostics, indices, role);
            if (diagnostic.isValid()) {
                return diagnostic;
            }
        }
        const auto found = roles.find(role);
        return found != roles.end() ? found->second : fallback;
    }

private:
    std::map<int, QVariant> roles;
};
}  // namespace SketcherGui
