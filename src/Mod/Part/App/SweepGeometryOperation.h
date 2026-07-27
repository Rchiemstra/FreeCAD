// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Part/PartGlobal.h>
#include <App/GeometryJob.h>
#include <Mod/Part/App/TopoShapeArchive.h>

#include <QJsonObject>

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
    std::string parameterDigest() const override;
    App::GeometryOperationTraits traits() const override;

    App::DetachedGeometryResult run(App::GeometryWorkerContext& ctx) const override;
    App::GeometryArchiveWriteResult writeArchive(App::GeometryArchiveWriter& writer) const override;
    App::DetachedGeometryResult decodeResultArchive(const std::string& absolutePath) const override;

    /// Decode a typed Sweep request from workspace-relative archives.
    static std::shared_ptr<SweepGeometryOperation>
    decodeFromRequest(const QJsonObject& request,
                      const QString& workspaceDir,
                      std::string& errorCode,
                      std::string& errorMessage);

    const FrozenTopoShapeBundle& spineBundle() const
    {
        return _spine;
    }
    const std::vector<FrozenTopoShapeBundle>& profileBundles() const
    {
        return _profiles;
    }
    bool isSolid() const
    {
        return _isSolid;
    }

private:
    FrozenTopoShapeBundle _spine;
    std::vector<FrozenTopoShapeBundle> _profiles;
    bool _isSolid {false};
};

} // namespace Part
