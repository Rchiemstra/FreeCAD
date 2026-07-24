// SPDX-License-Identifier: LGPL-2.1-or-later

#include "BooleanGeometryOperation.h"
#include <Mod/Part/App/FCBRepAlgoAPI_Fuse.h>
#include <Mod/Part/App/FCBRepAlgoAPI_Cut.h>
#include <Mod/Part/App/FCBRepAlgoAPI_Common.h>
#include <Mod/Part/App/FCBRepAlgoAPI_Section.h>
#include <Base/Console.h>

namespace Part
{

BooleanGeometryOperation::BooleanGeometryOperation() = default;

BooleanGeometryOperation::BooleanGeometryOperation(BooleanType type, const FrozenTopoShapeBundle& base, const FrozenTopoShapeBundle& tool)
    : _type(type), _base(base), _tool(tool)
{
}

BooleanGeometryOperation::~BooleanGeometryOperation() = default;

App::GeometryOperationTraits BooleanGeometryOperation::traits() const
{
    App::GeometryOperationTraits t;
    t.supportsInProcess = true;
    t.supportsCooperativeCancel = true;
    return t;
}

App::DetachedGeometryResult BooleanGeometryOperation::run(App::GeometryWorkerContext& ctx) const
{
    App::DetachedGeometryResult result;
    ctx.reportProgress(0.1, "boolean.prepare");

    TopoDS_Shape shape1 = _base.shape.getShape();
    TopoDS_Shape shape2 = _tool.shape.getShape();

    if (shape1.IsNull() || shape2.IsNull()) {
        result.success = false;
        result.errorCode = "NullShape";
        result.errorMessage = "One of the boolean operand shapes is null";
        return result;
    }

    ctx.reportProgress(0.4, "boolean.compute");
    TopoShape outShape;

    try {
        if (_type == BooleanType::Fuse) {
            FCBRepAlgoAPI_Fuse fuse(shape1, shape2);
            fuse.Build();
            if (fuse.IsDone()) {
                outShape.setShape(fuse.Shape());
            }
        } else if (_type == BooleanType::Cut) {
            FCBRepAlgoAPI_Cut cut(shape1, shape2);
            cut.Build();
            if (cut.IsDone()) {
                outShape.setShape(cut.Shape());
            }
        } else if (_type == BooleanType::Common) {
            FCBRepAlgoAPI_Common common(shape1, shape2);
            common.Build();
            if (common.IsDone()) {
                outShape.setShape(common.Shape());
            }
        }

        ctx.reportProgress(0.8, "boolean.archive");
        FrozenTopoShapeBundle outBundle = TopoShapeArchive::createBundle(outShape);
        std::string resultPath = ctx.tempDir() + "/result.fcg";

        if (TopoShapeArchive::writeArchive(outBundle, resultPath)) {
            result.success = true;
            result.resultArchivePath = resultPath;
        } else {
            result.success = false;
            result.errorCode = "ArchiveWriteError";
            result.errorMessage = "Failed to write output result archive";
        }
    } catch (const Standard_Failure& e) {
        result.success = false;
        result.errorCode = "OCCError";
        result.errorMessage = e.GetMessageString() ? e.GetMessageString() : "OCC Exception during boolean operation";
    }

    ctx.reportProgress(1.0, "boolean.complete");
    return result;
}

void BooleanGeometryOperation::writeArchive(App::GeometryArchiveWriter& writer) const
{
}

} // namespace Part
