// SPDX-License-Identifier: LGPL-2.1-or-later

#include "BooleanGeometryOperation.h"
#include <Mod/Part/App/TopoShapeOpCode.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Standard_Failure.hxx>
#include <memory>

namespace Part
{

namespace
{

TopoShape materializeOperand(const FrozenTopoShapeBundle& bundle, long fallbackTag)
{
    TopoShape shape = bundle.shape;
    const long tag = bundle.shapeTag != 0 ? bundle.shapeTag : (shape.Tag != 0 ? shape.Tag : fallbackTag);
    shape.Tag = tag;
    if (bundle.hasher) {
        shape.Hasher = bundle.hasher;
    }
    if (bundle.elementMap) {
        shape.resetElementMap(bundle.elementMap);
    }
    return shape;
}

const char* makerForType(BooleanType type)
{
    switch (type) {
        case BooleanType::Cut:
            return OpCodes::Cut;
        case BooleanType::Common:
            return OpCodes::Common;
        case BooleanType::Section:
            return OpCodes::Section;
        case BooleanType::Fuse:
        default:
            return OpCodes::Fuse;
    }
}

} // namespace

BooleanGeometryOperation::BooleanGeometryOperation() = default;

BooleanGeometryOperation::BooleanGeometryOperation(BooleanType type, const FrozenTopoShapeBundle& base, const FrozenTopoShapeBundle& tool)
    : _type(type), _base(base), _tool(tool)
{
}

BooleanGeometryOperation::~BooleanGeometryOperation() = default;

std::string BooleanGeometryOperation::parameterDigest() const
{
    return std::string("bool:")
        + std::to_string(static_cast<int>(_type)) + '|'
        + TopoShapeArchive::fingerprintBundle(_base) + '|'
        + TopoShapeArchive::fingerprintBundle(_tool);
}

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

    if (ctx.isCancelled()) {
        result.success = false;
        result.errorCode = "Cancelled";
        result.errorMessage = "Cancelled before boolean start";
        return result;
    }

    TopoShape base = materializeOperand(_base, /*fallbackTag=*/1);
    TopoShape tool = materializeOperand(_tool, /*fallbackTag=*/2);
    if (base.Tag == tool.Tag) {
        // Distinct source tags are required for unambiguous OCC history encoding.
        tool.Tag = base.Tag == 0 ? 2 : base.Tag + 1;
    }

    if (base.isNull() || tool.isNull()) {
        result.success = false;
        result.errorCode = "NullShape";
        result.errorMessage = "One of the boolean operand shapes is null";
        return result;
    }

    App::StringHasherRef hasher = base.Hasher;
    if (!hasher) {
        hasher = tool.Hasher;
    }
    if (!hasher) {
        hasher = App::StringHasherRef(new App::StringHasher);
        base.Hasher = hasher;
        tool.Hasher = hasher;
    }

    ctx.reportProgress(0.4, "boolean.compute");

    try {
        TopoShape outShape(0, hasher);
        outShape.makeElementBoolean(makerForType(_type), {base, tool});

        if (ctx.isCancelled()) {
            result.success = false;
            result.errorCode = "Cancelled";
            result.errorMessage = "Cancelled during boolean compute";
            return result;
        }

        if (outShape.isNull()) {
            result.success = false;
            result.errorCode = "NullResult";
            result.errorMessage = "Boolean operation produced a null shape";
            return result;
        }

        ctx.reportProgress(0.8, "boolean.archive");
        FrozenTopoShapeBundle outBundle = TopoShapeArchive::createBundle(outShape);
        if (!outBundle.valid) {
            result.success = false;
            result.errorCode = outBundle.errorCode.empty() ? "BundleInvalid" : outBundle.errorCode;
            result.errorMessage = "Failed to freeze mapped boolean result";
            return result;
        }

        std::string resultPath = ctx.tempDir() + "/result.fcg";
        if (TopoShapeArchive::writeArchive(outBundle, resultPath)) {
            result.success = true;
            result.resultArchivePath = resultPath;
        }
        else {
            result.success = false;
            result.errorCode = "ArchiveWriteError";
            result.errorMessage = "Failed to write output result archive";
        }
    }
    catch (const Base::Exception& e) {
        result.success = false;
        result.errorCode = "BooleanError";
        result.errorMessage = e.what();
    }
    catch (const Standard_Failure& e) {
        result.success = false;
        result.errorCode = "OCCError";
        result.errorMessage = e.GetMessageString() ? e.GetMessageString()
                                                   : "OCC Exception during boolean operation";
    }
    catch (const std::exception& e) {
        result.success = false;
        result.errorCode = "Exception";
        result.errorMessage = e.what();
    }

    ctx.reportProgress(1.0, "boolean.complete");
    return result;
}

void BooleanGeometryOperation::writeArchive(App::GeometryArchiveWriter& writer) const
{
    (void)writer;
}

} // namespace Part
