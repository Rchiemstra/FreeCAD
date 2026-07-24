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

class PartExport SweepGeometryOperation : public App::DetachedGeometryTask
{
public:
    SweepGeometryOperation();
    SweepGeometryOperation(const FrozenTopoShapeBundle& spine, const std::vector<FrozenTopoShapeBundle>& profiles, bool isSolid = false);
    ~SweepGeometryOperation() override;

    std::string operationType() const override { return "Part::Sweep"; }
    uint32_t codecVersion() const override { return 1; }
    App::GeometryOperationTraits traits() const override;

    App::DetachedGeometryResult run(App::GeometryWorkerContext& ctx) const override;
    void writeArchive(App::GeometryArchiveWriter& writer) const override;

private:
    FrozenTopoShapeBundle _spine;
    std::vector<FrozenTopoShapeBundle> _profiles;
    bool _isSolid {false};
};

} // namespace Part
