// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <vector>

#include <QMap>
#include <QStringList>
#include <QVariant>

#include <App/DocumentObject.h>
#include <App/ObjectIdentifier.h>
#include <App/Property.h>
#include <Gui/MainWindow.h>

namespace Gui
{
// The Python observer publishes plain presentation data after changes/recompute.
// This path only looks up cached strings: no Python calls or expression evaluation.
inline QString expressionDiagnosticReason(
    const App::ObjectIdentifier& path,
    const std::vector<App::Property*>& properties)
{
    const auto* owner = path.getOwner();
    const auto* window = getMainWindow();
    if (!window) {
        return {};
    }
    const auto reasons = window->property("expressionDiagnosticsReasons").toMap();
    if (reasons.isEmpty()) {
        return {};
    }
    if (owner && properties.size() <= 1) {
        const QString key = QString::fromStdString(
            owner->getFullName() + "." + path.getPropertyName() + path.getSubPathStr());
        return reasons.value(key).toString();
    }
    // Compound roots and merged multi-selection rows may have no binding.
    QStringList messages;
    for (const auto* property : properties) {
        const auto* object = dynamic_cast<const App::DocumentObject*>(property->getContainer());
        if (object && property->getName()) {
            const auto key = QString::fromStdString(object->getFullName() + "." + property->getName());
            const auto reason = reasons.value(key).toString();
            if (!reason.isEmpty() && !messages.contains(reason)) {
                messages.append(reason);
            }
        }
    }
    return messages.join(QStringLiteral("\n\n"));
}

inline QVariant expressionDiagnosticColor()
{
    return getMainWindow()->property("expressionDiagnosticsErrorColor");
}
}  // namespace Gui
