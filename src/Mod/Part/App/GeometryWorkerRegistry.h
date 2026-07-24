// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Part/PartGlobal.h>
#include <App/GeometryJob.h>

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace Part
{

using TaskFactory = std::function<std::shared_ptr<App::DetachedGeometryTask>()>;

class PartExport GeometryWorkerRegistry
{
public:
    static GeometryWorkerRegistry& instance();

    void registerOperation(const std::string& name, TaskFactory factory);
    std::shared_ptr<App::DetachedGeometryTask> createTask(const std::string& name) const;
    bool isOperationAllowed(const std::string& name) const;

    void setInProcessAllowlist(const std::vector<std::string>& allowlist);
    bool isInProcessAllowed(const std::string& name) const;

private:
    std::map<std::string, TaskFactory> _factories;
    std::vector<std::string> _inProcessAllowlist;
};

} // namespace Part
