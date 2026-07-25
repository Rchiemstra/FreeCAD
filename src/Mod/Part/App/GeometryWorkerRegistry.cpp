// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryWorkerRegistry.h"
#include "BooleanGeometryOperation.h"
#include "FilletGeometryOperation.h"
#include "SweepGeometryOperation.h"
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
    _factories[name] = std::move(factory);
}

void GeometryWorkerRegistry::registerBuiltins()
{
    // Factories return empty-parameter stubs; the worker decodes request payloads
    // into typed ops in a later protocol slice. Registration establishes the
    // allowlist of trusted operation names for FreeCADCmd.
    registerOperation("Part::Boolean", []() {
        return std::shared_ptr<App::DetachedGeometryTask>(new BooleanGeometryOperation());
    });
    registerOperation("Part::Fillet", []() {
        return std::shared_ptr<App::DetachedGeometryTask>(new FilletGeometryOperation());
    });
    registerOperation("Part::Sweep", []() {
        return std::shared_ptr<App::DetachedGeometryTask>(new SweepGeometryOperation());
    });
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
