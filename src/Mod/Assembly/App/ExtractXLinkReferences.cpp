// SPDX-License-Identifier: LGPL-2.1-or-later
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/PropertyLinks.h>
#include "ExtractReferences.h"
#include "ExtractXLinkReferences.h"

namespace Assembly
{
namespace
{
// Like PropertyXLink::copyTo(), construct detached metadata, never endpoints.
// Endpoint setters would register backlinks and require a live property owner.
class EntryCopy : public App::PropertyXLinkSub
{
public:
    EntryCopy(const App::PropertyXLinkSub& source)
    {
        filePath = source.getFilePath();
        objectName = source.getObjectName() ? source.getObjectName() : "";
        _SubList = source.getSubValues();
        _ShadowSubList = source.getShadowSubs();
        for (int flag = LinkAllowExternal; flag <= LinkSilentRestore; ++flag) {
            setFlag(flag, source.testFlag(flag));
        }
    }
    void transfer(App::PropertyXLinkSub& target,
                  App::DocumentObject* object,
                  std::vector<std::string>* subs)
    {
        copyTo(target, object, subs);
    }
};
class ListCopy : public App::PropertyXLinkSubList
{
public:
    void append(const App::PropertyXLinkSub& source,
                App::DocumentObject* object,
                std::vector<std::string>* subs)
    {
        _Links.emplace_back();
        EntryCopy entry(source);
        entry.transfer(_Links.back(), object, subs);
    }
};
} // namespace
std::unique_ptr<App::Property>
prepareXLinkReferences(App::PropertyXLinkSubList& property,
                       const std::vector<ReferenceReplacement>& replacements,
                       std::vector<App::DocumentObject*>& referenced)
{
    auto copy = std::make_unique<ListCopy>();
    auto owner = dynamic_cast<App::DocumentObject*>(property.getContainer());
    bool changed = false;
    for (const auto& entry : property.getSubListValues()) {
        auto target = entry.getValue();
        auto subs = entry.getSubValues(false);
        bool replaced = false;
        for (const auto& item : replacements) {
            auto result = App::PropertyLinkBase::tryReplaceLinkSubs(
                owner, target, owner, item.oldObject, item.link, subs);
            if (!result.first) {
                for (auto parent : item.oldObject->getInList()) {
                    result = App::PropertyLinkBase::tryReplaceLinkSubs(
                        owner, target, parent, item.oldObject, item.link, subs);
                    if (result.first) {
                        break;
                    }
                }
            }
            if (result.first) {
                target = result.first;
                subs = std::move(result.second);
                referenced.push_back(item.oldObject);
                changed = replaced = true;
            }
        }
        if (replaced) {
            if (subs.empty()) {
                subs.emplace_back();
            }
        }
        copy->append(entry, target, replaced ? &subs : nullptr);
    }
    return changed ? std::unique_ptr<App::Property>(std::move(copy)) : nullptr;
}
} // namespace Assembly
