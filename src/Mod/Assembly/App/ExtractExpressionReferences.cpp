// SPDX-License-Identifier: LGPL-2.1-or-later
#include <map>
#include <App/DocumentObject.h>
#include <App/Expression.h>
#include <App/PropertyExpressionEngine.h>
#include "ExtractReferences.h"
#include "ExtractExpressionReferences.h"

namespace Assembly
{
namespace
{
class RenameVisitor : public App::ExpressionVisitor
{
public:
    explicit RenameVisitor(const std::map<App::ObjectIdentifier, App::ObjectIdentifier>& paths)
        : paths(paths)
    {
    }
    void visit(App::Expression& expression) override
    {
        renameObjectIdentifier(expression, paths, dummy);
    }

private:
    const std::map<App::ObjectIdentifier, App::ObjectIdentifier>& paths;
    App::ObjectIdentifier dummy;
};
} // namespace

std::unique_ptr<App::Property>
prepareExpressionReferences(App::DocumentObject& owner,
                            App::PropertyExpressionContainer& property,
                            const std::vector<ReferenceReplacement>& replacements,
                            std::vector<App::DocumentObject*>& shapes)
{
    if (property.getExpressions().empty()) {
        return {};
    }
    std::unique_ptr<App::Property> copy(property.Copy());
    auto expressions = static_cast<App::PropertyExpressionContainer*>(copy.get());
    bool changed = false;
    for (const auto& [binding, expression] : expressions->getExpressions()) {
        if (!expression) {
            continue;
        }
        std::map<App::ObjectIdentifier, App::ObjectIdentifier> paths;
        for (const auto& [identifier, hidden] : expression->getIdentifiers()) {
            for (const auto& replacement : replacements) {
                const auto name = identifier.getPropertyName();
                const bool instanceProperty = replacement.component &&
                                              identifier.getSubObjectName().empty() &&
                                              (name == "Placement" || name == "Visibility" ||
                                               name == "Label" || name == "Label2");
                auto target = instanceProperty || !replacement.definition ? replacement.link
                                                                          : replacement.definition;
                App::ObjectIdentifier rewritten;
                bool replaced =
                    identifier.replaceObject(rewritten, &owner, replacement.oldObject, target);
                if (!replaced) {
                    for (auto parent : replacement.oldObject->getInList()) {
                        if (identifier.replaceObject(
                                rewritten, parent, replacement.oldObject, target)) {
                            replaced = true;
                            break;
                        }
                    }
                }
                if (replaced) {
                    if (replacement.component && name == "Shape") {
                        shapes.push_back(replacement.oldObject);
                        if (replacement.shape) {
                            identifier.replaceObject(
                                rewritten, &owner, replacement.oldObject, replacement.shape);
                        }
                    }
                    paths.emplace(identifier.canonicalPath(), rewritten);
                    break;
                }
            }
        }
        if (!paths.empty()) {
            RenameVisitor visitor(paths);
            const_cast<App::Expression*>(expression)->visit(visitor);
            changed = true;
        }
    }
    return changed ? std::move(copy) : nullptr;
}
} // namespace Assembly
