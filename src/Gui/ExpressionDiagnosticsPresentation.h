// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <optional>

#include <QMap>
#include <QStringList>
#include <QVariant>

#include <App/DocumentObject.h>
#include <App/MutationClassification.h>
#include <Gui/MainWindow.h>
#include <Gui/propertyeditor/PropertyItem.h>

namespace Gui
{
inline const App::ObjectIdentifier& PropertyEditor::PropertyItemAttorney::diagnosticPath(
    const PropertyItem& item)
{
    return item.getPath();
}

// Explicit model-population request, issued only by the admitted observer.
// Normal paint/tooltip roles never resolve paths, properties or document objects.
inline constexpr int expressionDiagnosticsPopulateRole = Qt::UserRole + 26496;

namespace ExpressionDiagnosticsPresentation
{
using PropertyEditor::PropertyItem;

inline QString reason(const PropertyItem& item)
{
    const auto* window = getMainWindow();
    if (!window) {
        return {};
    }
    const auto reasons = window->property("expressionDiagnosticsReasons").toMap();
    QStringList messages;
    for (const auto& key : item.property("expressionDiagnosticsKeys").toStringList()) {
        const auto message = reasons.value(key).toString();
        if (!message.isEmpty() && !messages.contains(message)) {
            messages.append(message);
        }
    }
    return messages.join(QStringLiteral("\n\n"));
}

// Capture legacy roles too: even a healthy expression's foreground and a normal
// property tooltip may resolve a document path. Preserve their existing text and
// colour while keeping all these reads inside model population under admission.
inline void captureRow(PropertyItem& item, QStringList owners = {}, QString path = {})
{
    const auto& properties = item.getPropertyData();
    if (!properties.empty()) {
        owners.clear();
        for (const auto* property : properties) {
            const auto* object = dynamic_cast<const App::DocumentObject*>(property->getContainer());
            if (object && property->getName()) {
                owners.append(QString::fromStdString(object->getFullName()));
                path = QString::fromUtf8(property->getName());
            }
        }
    }
    else if (item.isBound()) {
        // Constraint rows have canonical names/indexes different from their Qt
        // object names. This is population, so resolving that identity is safe.
        const auto& binding = PropertyEditor::PropertyItemAttorney::diagnosticPath(item);
        path = QString::fromStdString(binding.getPropertyName() + binding.getSubPathStr());
    }
    QStringList keys;
    for (const auto& owner : owners) {
        keys.append(owner + QLatin1Char('.') + path);
    }
    item.setProperty("expressionDiagnosticsKeys", keys);

    item.setProperty("expressionDiagnosticsCapturing", true);
    QVariantList roles;
    try {
        for (int column = 0; column < PropertyItem::ColumnCount; ++column) {
            roles.append(item.data(column, Qt::ForegroundRole));
            roles.append(item.data(column, Qt::ToolTipRole));
        }
    }
    catch (...) {
        item.setProperty("expressionDiagnosticsCapturing", false);
        throw;
    }
    item.setProperty("expressionDiagnosticsCapturing", false);
    item.setProperty("expressionDiagnosticsRoles", roles);

    for (int row = 0; row < item.childCount(); ++row) {
        auto& child = *item.child(row);
        QString component = child.objectName();
        // These editor names intentionally differ from the expression subpath.
        if (qobject_cast<PropertyEditor::PropertyPlacementItem*>(&item)) {
            if (component == QLatin1String("Position")) {
                component = QStringLiteral("Base");
            }
            else {
                component.prepend(QStringLiteral("Rotation."));
            }
        }
        captureRow(child, owners, path.isEmpty() ? component : path + QLatin1Char('.') + component);
    }
}

inline bool populate(PropertyItem& item)
{
    App::LivePresentationRefreshScope admission;
    if (!admission.acquired()) {
        return false;
    }
    captureRow(item);
    return true;
}

inline std::optional<QVariant> data(const PropertyItem& item, int column, int role)
{
    if (item.property("expressionDiagnosticsCapturing").toBool()) {
        return std::nullopt;
    }
    if (role == expressionDiagnosticsPopulateRole) {
        return populate(const_cast<PropertyItem&>(item));
    }
    if (role != Qt::ForegroundRole && role != Qt::ToolTipRole) {
        return std::nullopt;
    }
    const auto roles = item.property("expressionDiagnosticsRoles").toList();
    const int index = column * 2 + (role == Qt::ToolTipRole ? 1 : 0);
    QVariant original = index >= 0 && index < roles.size() ? roles[index] : QVariant();
    const auto message = reason(item);
    if (!message.isEmpty()) {
        if (role == Qt::ForegroundRole && column == PropertyItem::ValueColumn) {
            return getMainWindow()->property("expressionDiagnosticsErrorColor");
        }
        if (role == Qt::ToolTipRole) {
            return original.toString() + PropertyItem::tr("\n\nExpression error: ") + message;
        }
    }
    return original;
}
}  // namespace ExpressionDiagnosticsPresentation
}  // namespace Gui
