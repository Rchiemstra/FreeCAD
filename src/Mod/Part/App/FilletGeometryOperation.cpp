// SPDX-License-Identifier: LGPL-2.1-or-later

#include "FilletGeometryOperation.h"
#include <Mod/Part/App/TopoShapeOpCode.h>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS.hxx>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Standard_Failure.hxx>
#include <memory>

namespace Part
{

namespace
{

TopoShape materializeBase(const FrozenTopoShapeBundle& bundle)
{
    TopoShape shape = bundle.shape;
    const long tag = bundle.shapeTag != 0 ? bundle.shapeTag : (shape.Tag != 0 ? shape.Tag : 1);
    shape.Tag = tag;
    if (bundle.hasher) {
        shape.Hasher = bundle.hasher;
    }
    if (bundle.elementMap) {
        shape.resetElementMap(bundle.elementMap);
    }
    return shape;
}

} // namespace

FilletGeometryOperation::FilletGeometryOperation() = default;

FilletGeometryOperation::FilletGeometryOperation(const FrozenTopoShapeBundle& base, const std::vector<FilletEdgeSpec>& edges)
    : _base(base), _edges(edges)
{
}

FilletGeometryOperation::~FilletGeometryOperation() = default;

std::string FilletGeometryOperation::parameterDigest() const
{
    std::string digest = "fillet|" + TopoShapeArchive::fingerprintBundle(_base);
    for (const auto& edge : _edges) {
        digest.push_back('|');
        digest += std::to_string(edge.edgeIndex);
        digest.push_back(':');
        digest += std::to_string(edge.startRadius);
        digest.push_back(':');
        digest += std::to_string(edge.endRadius);
    }
    return digest;
}

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

    if (ctx.isCancelled()) {
        result.success = false;
        result.errorCode = "Cancelled";
        result.errorMessage = "Cancelled before fillet start";
        return result;
    }

    TopoShape base = materializeBase(_base);
    if (base.isNull()) {
        result.success = false;
        result.errorCode = "NullShape";
        result.errorMessage = "Base shape for fillet is null";
        return result;
    }

    if (!base.Hasher) {
        base.Hasher = App::StringHasherRef(new App::StringHasher);
    }

    try {
        BRepFilletAPI_MakeFillet mkFillet(base.getShape());
        std::vector<TopoDS_Edge> allEdges;
        for (TopExp_Explorer exp(base.getShape(), TopAbs_EDGE); exp.More(); exp.Next()) {
            allEdges.push_back(TopoDS::Edge(exp.Current()));
        }

        size_t added = 0;
        for (const auto& spec : _edges) {
            if (spec.edgeIndex < allEdges.size()) {
                mkFillet.Add(spec.startRadius, spec.endRadius, allEdges[spec.edgeIndex]);
                ++added;
            }
        }
        if (added == 0) {
            result.success = false;
            result.errorCode = "NoEdges";
            result.errorMessage = "No valid fillet edge indices were provided";
            return result;
        }

        ctx.reportProgress(0.5, "fillet.compute");
        mkFillet.Build();

        if (ctx.isCancelled()) {
            result.success = false;
            result.errorCode = "Cancelled";
            result.errorMessage = "Cancelled during fillet compute";
            return result;
        }

        if (!mkFillet.IsDone()) {
            result.success = false;
            result.errorCode = "FilletBuildError";
            result.errorMessage = "BRepFilletAPI_MakeFillet failed to build shape";
            return result;
        }

        TopoShape outShape(0, base.Hasher);
        outShape.makeElementShape(mkFillet, base, OpCodes::Fillet);
        if (outShape.isNull()) {
            result.success = false;
            result.errorCode = "NullResult";
            result.errorMessage = "Mapped fillet produced a null shape";
            return result;
        }

        ctx.reportProgress(0.8, "fillet.archive");
        FrozenTopoShapeBundle outBundle = TopoShapeArchive::createBundle(outShape);
        if (!outBundle.valid) {
            result.success = false;
            result.errorCode = outBundle.errorCode.empty() ? "BundleInvalid" : outBundle.errorCode;
            result.errorMessage = "Failed to freeze mapped fillet result";
            return result;
        }

        std::string resultPath = ctx.tempDir() + "/result.fcg";
        if (TopoShapeArchive::writeArchive(outBundle, resultPath)) {
            result.success = true;
            result.resultArchivePath = resultPath;
        }
        else {
            result.success = false;
            result.errorCode = "ArchiveError";
            result.errorMessage = "Failed to write fillet output archive";
        }
    }
    catch (const Base::Exception& e) {
        result.success = false;
        result.errorCode = "FilletError";
        result.errorMessage = e.what();
    }
    catch (const Standard_Failure& e) {
        result.success = false;
        result.errorCode = "OCCError";
        result.errorMessage = e.GetMessageString() ? e.GetMessageString()
                                                   : "OCC Exception during fillet operation";
    }
    catch (const std::exception& e) {
        result.success = false;
        result.errorCode = "Exception";
        result.errorMessage = e.what();
    }

    ctx.reportProgress(1.0, "fillet.complete");
    return result;
}

void FilletGeometryOperation::writeArchive(App::GeometryArchiveWriter& writer) const
{
    (void)writer;
}

} // namespace Part
