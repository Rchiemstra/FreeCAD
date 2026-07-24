// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Part/PartGlobal.h>
#include <App/GeometryJob.h>
#include <Mod/Part/App/TopoShapeArchive.h>

#include <memory>
#include <string>
#include <vector>

namespace Part
{

struct PartExport FilletEdgeSpec
{
    uint32_t edgeIndex {0};
    double startRadius {1.0};
    double endRadius {1.0};
};

class PartExport FilletGeometryOperation : public App::DetachedGeometryTask
{
public:
    FilletGeometryOperation();
    FilletGeometryOperation(const FrozenTopoShapeBundle& base, const std::vector<FilletEdgeSpec>& edges);
    ~FilletGeometryOperation() override;

    std::string operationType() const override { return "Part::Fillet"; }
    uint32_t codecVersion() const override { return 1; }
    App::GeometryOperationTraits traits() const override;

    App::DetachedGeometryResult run(App::GeometryWorkerContext& ctx) const override;
    void writeArchive(App::GeometryArchiveWriter& writer) const override;

private:
    FrozenTopoShapeBundle _base;
    std::vector<FilletEdgeSpec> _edges;
};

} // namespace Part
