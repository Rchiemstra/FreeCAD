// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Part/PartGlobal.h>
#include <App/GeometryJob.h>
#include <Mod/Part/App/TopoShapeArchive.h>

#include <QJsonObject>

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
    std::string parameterDigest() const override;
    App::GeometryOperationTraits traits() const override;

    App::DetachedGeometryResult run(App::GeometryWorkerContext& ctx) const override;
    App::GeometryArchiveWriteResult writeArchive(App::GeometryArchiveWriter& writer) const override;
    App::DetachedGeometryResult decodeResultArchive(const std::string& absolutePath) const override;

    /// Decode a typed Boolean request from workspace-relative archives.
    static std::shared_ptr<BooleanGeometryOperation>
    decodeFromRequest(const QJsonObject& request,
                      const QString& workspaceDir,
                      std::string& errorCode,
                      std::string& errorMessage);

    BooleanType booleanType() const
    {
        return _type;
    }
    const FrozenTopoShapeBundle& baseBundle() const
    {
        return _base;
    }
    const FrozenTopoShapeBundle& toolBundle() const
    {
        return _tool;
    }

private:
    BooleanType _type {BooleanType::Fuse};
    FrozenTopoShapeBundle _base;
    FrozenTopoShapeBundle _tool;
};

} // namespace Part
