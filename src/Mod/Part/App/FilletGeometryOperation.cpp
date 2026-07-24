// SPDX-License-Identifier: LGPL-2.1-or-later

#include "FilletGeometryOperation.h"
#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS.hxx>
#include <Base/Console.h>

namespace Part
{

FilletGeometryOperation::FilletGeometryOperation() = default;

FilletGeometryOperation::FilletGeometryOperation(const FrozenTopoShapeBundle& base, const std::vector<FilletEdgeSpec>& edges)
    : _base(base), _edges(edges)
{
}

FilletGeometryOperation::~FilletGeometryOperation() = default;

App::GeometryOperationTraits FilletGeometryOperation::traits() const
{
    App::GeometryOperationTraits t;
    t.supportsInProcess = false; // Process-first for fillet
    t.supportsCooperativeCancel = true;
    return t;
}

App::DetachedGeometryResult FilletGeometryOperation::run(App::GeometryWorkerContext& ctx) const
{
    App::DetachedGeometryResult result;
    ctx.reportProgress(0.1, "fillet.prepare");

    TopoDS_Shape baseShape = _base.shape.getShape();
    if (baseShape.IsNull()) {
        result.success = false;
        result.errorCode = "NullShape";
        result.errorMessage = "Base shape for fillet is null";
        return result;
    }

    try {
        BRepFilletAPI_MakeFillet mkFillet(baseShape);
        std::vector<TopoDS_Edge> allEdges;
        for (TopExp_Explorer exp(baseShape, TopAbs_EDGE); exp.More(); exp.Next()) {
            allEdges.push_back(TopoDS::Edge(exp.Current()));
        }

        for (const auto& spec : _edges) {
            if (spec.edgeIndex < allEdges.size()) {
                mkFillet.Add(spec.startRadius, allEdges[spec.edgeIndex]);
            }
        }

        ctx.reportProgress(0.5, "fillet.compute");
        mkFillet.Build();

        if (mkFillet.IsDone()) {
            TopoShape outShape(mkFillet.Shape());
            FrozenTopoShapeBundle outBundle = TopoShapeArchive::createBundle(outShape);
            std::string resultPath = ctx.tempDir() + "/result.fcg";

            if (TopoShapeArchive::writeArchive(outBundle, resultPath)) {
                result.success = true;
                result.resultArchivePath = resultPath;
            } else {
                result.success = false;
                result.errorCode = "ArchiveError";
                result.errorMessage = "Failed to write fillet output archive";
            }
        } else {
            result.success = false;
            result.errorCode = "FilletBuildError";
            result.errorMessage = "BRepFilletAPI_MakeFillet failed to build shape";
        }
    } catch (const Standard_Failure& e) {
        result.success = false;
        result.errorCode = "OCCError";
        result.errorMessage = e.GetMessageString() ? e.GetMessageString() : "OCC Exception during fillet";
    }

    ctx.reportProgress(1.0, "fillet.complete");
    return result;
}

void FilletGeometryOperation::writeArchive(App::GeometryArchiveWriter& writer) const
{
}

} // namespace Part
