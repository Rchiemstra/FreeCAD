// SPDX-License-Identifier: LGPL-2.1-or-later

#include "SweepGeometryOperation.h"
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS.hxx>
#include <Base/Console.h>

namespace Part
{

SweepGeometryOperation::SweepGeometryOperation() = default;

SweepGeometryOperation::SweepGeometryOperation(const FrozenTopoShapeBundle& spine, const std::vector<FrozenTopoShapeBundle>& profiles, bool isSolid)
    : _spine(spine), _profiles(profiles), _isSolid(isSolid)
{
}

SweepGeometryOperation::~SweepGeometryOperation() = default;

App::GeometryOperationTraits SweepGeometryOperation::traits() const
{
    App::GeometryOperationTraits t;
    t.supportsInProcess = false; // Process-first for sweep
    t.supportsCooperativeCancel = true;
    return t;
}

App::DetachedGeometryResult SweepGeometryOperation::run(App::GeometryWorkerContext& ctx) const
{
    App::DetachedGeometryResult result;
    ctx.reportProgress(0.1, "sweep.prepare");

    TopoDS_Shape spineShape = _spine.shape.getShape();
    if (spineShape.IsNull() || spineShape.ShapeType() != TopAbs_WIRE) {
        result.success = false;
        result.errorCode = "InvalidSpine";
        result.errorMessage = "Spine shape is null or not a Wire";
        return result;
    }

    try {
        BRepOffsetAPI_MakePipeShell mkPipe(TopoDS::Wire(spineShape));
        for (const auto& prof : _profiles) {
            TopoDS_Shape profShape = prof.shape.getShape();
            if (!profShape.IsNull()) {
                mkPipe.Add(profShape);
            }
        }

        ctx.reportProgress(0.5, "sweep.compute");
        mkPipe.Build();

        if (mkPipe.IsDone()) {
            if (_isSolid) {
                mkPipe.MakeSolid();
            }

            TopoShape outShape(mkPipe.Shape());
            FrozenTopoShapeBundle outBundle = TopoShapeArchive::createBundle(outShape);
            std::string resultPath = ctx.tempDir() + "/result.fcg";

            if (TopoShapeArchive::writeArchive(outBundle, resultPath)) {
                result.success = true;
                result.resultArchivePath = resultPath;
            } else {
                result.success = false;
                result.errorCode = "ArchiveError";
                result.errorMessage = "Failed to write sweep output archive";
            }
        } else {
            result.success = false;
            result.errorCode = "SweepBuildError";
            result.errorMessage = "BRepOffsetAPI_MakePipeShell failed to build sweep";
        }
    } catch (const Standard_Failure& e) {
        result.success = false;
        result.errorCode = "OCCError";
        result.errorMessage = e.GetMessageString() ? e.GetMessageString() : "OCC Exception during sweep";
    }

    ctx.reportProgress(1.0, "sweep.complete");
    return result;
}

void SweepGeometryOperation::writeArchive(App::GeometryArchiveWriter& writer) const
{
}

} // namespace Part
