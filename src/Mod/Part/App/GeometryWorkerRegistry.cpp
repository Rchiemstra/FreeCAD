// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryWorkerRegistry.h"
#include <algorithm>

namespace Part
{

GeometryWorkerRegistry& GeometryWorkerRegistry::instance()
{
    static GeometryWorkerRegistry reg;
    return reg;
}

void GeometryWorkerRegistry::registerOperation(const std::string& name, TaskFactory factory)
{
    _factories[name] = factory;
}

std::shared_ptr<App::DetachedGeometryTask> GeometryWorkerRegistry::createTask(const std::string& name) const
{
    auto it = _factories.find(name);
    if (it != _factories.end()) {
        return it->second();
    }
    return nullptr;
}

bool GeometryWorkerRegistry::isOperationAllowed(const std::string& name) const
{
    return _factories.find(name) != _factories.end();
}

void GeometryWorkerRegistry::setInProcessAllowlist(const std::vector<std::string>& allowlist)
{
    _inProcessAllowlist = allowlist;
}

bool GeometryWorkerRegistry::isInProcessAllowed(const std::string& name) const
{
    return std::find(_inProcessAllowlist.begin(), _inProcessAllowlist.end(), name) != _inProcessAllowlist.end();
}

} // namespace Part
