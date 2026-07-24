// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryWorker.h"
#include "GeometryWorkerRegistry.h"

#include <Base/Console.h>
#include <iostream>
#include <fstream>
#include <chrono>

namespace Part
{

class ProcessWorkerContext : public App::GeometryWorkerContext
{
public:
    explicit ProcessWorkerContext(const std::string& tempDir)
        : _tempDir(tempDir)
    {
        _deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
    }

    void reportProgress(double fraction, const std::string& phase = "") override
    {
        std::cout << "FCGEO/1 {\"type\":\"progress\",\"phase\":\"" << phase << "\",\"fraction\":" << fraction << "}" << std::endl;
    }

    bool isCancelled() const override
    {
        return false;
    }

    std::chrono::steady_clock::time_point deadline() const override
    {
        return _deadline;
    }

    std::string tempDir() const override
    {
        return _tempDir;
    }

private:
    std::string _tempDir;
    std::chrono::steady_clock::time_point _deadline;
};

int GeometryWorker::runWorkerProcess(const std::string& requestJsonPath)
{
    std::cout << "FCGEO/1 {\"type\":\"hello\",\"version\":\"1.0\"}" << std::endl;

    std::ifstream ifs(requestJsonPath);
    if (!ifs.is_open()) {
        std::cout << "FCGEO/1 {\"type\":\"error\",\"code\":\"request_file_not_found\",\"message\":\"Failed to open request.json\"}" << std::endl;
        return 1;
    }

    // Process worker execution logic
    std::cout << "FCGEO/1 {\"type\":\"progress\",\"phase\":\"worker.start\",\"fraction\":0.1}" << std::endl;

    return 0;
}

} // namespace Part
