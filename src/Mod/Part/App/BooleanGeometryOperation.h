// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Part/PartGlobal.h>
#include <App/GeometryJob.h>
#include <Mod/Part/App/TopoShapeArchive.h>

#include <memory>
#include <string>

namespace Part
{

enum class BooleanType
{
    Fuse,
    Cut,
    Common,
    Section
};

class PartExport BooleanGeometryOperation : public App::DetachedGeometryTask
{
public:
    BooleanGeometryOperation();
    BooleanGeometryOperation(BooleanType type, const FrozenTopoShapeBundle& base, const FrozenTopoShapeBundle& tool);
    ~BooleanGeometryOperation() override;

    std::string operationType() const override { return "Part::Boolean"; }
    uint32_t codecVersion() const override { return 1; }
    App::GeometryOperationTraits traits() const override;

    App::DetachedGeometryResult run(App::GeometryWorkerContext& ctx) const override;
    void writeArchive(App::GeometryArchiveWriter& writer) const override;

private:
    BooleanType _type {BooleanType::Fuse};
    FrozenTopoShapeBundle _base;
    FrozenTopoShapeBundle _tool;
};

} // namespace Part
