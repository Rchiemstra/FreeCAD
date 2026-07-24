// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Part/PartGlobal.h>
#include <App/GeometryJob.h>

#include <memory>
#include <string>

namespace Part
{

class PartExport GeometryWorker
{
public:
    static int runWorkerProcess(const std::string& requestJsonPath);
};

} // namespace Part
