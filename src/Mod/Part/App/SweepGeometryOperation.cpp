// SPDX-License-Identifier: LGPL-2.1-or-later

#include "SweepGeometryOperation.h"
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

} // namespace

SweepGeometryOperation::SweepGeometryOperation() = default;

SweepGeometryOperation::SweepGeometryOperation(const FrozenTopoShapeBundle& spine,
                                               const std::vector<FrozenTopoShapeBundle>& profiles,
                                               bool isSolid)
    : _spine(spine)
    , _profiles(profiles)
    , _isSolid(isSolid)
{
}

SweepGeometryOperation::~SweepGeometryOperation() = default;

std::string SweepGeometryOperation::parameterDigest() const
{
    std::string digest = std::string("sweep:solid=")
        + (_isSolid ? '1' : '0') + '|'
        + TopoShapeArchive::fingerprintBundle(_spine);
    for (const auto& profile : _profiles) {
        digest.push_back('|');
        digest += TopoShapeArchive::fingerprintBundle(profile);
    }
    return digest;
}

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

    if (ctx.isCancelled()) {
        result.success = false;
        result.errorCode = "Cancelled";
        result.errorMessage = "Cancelled before sweep start";
        return result;
    }

    if (_profiles.empty()) {
        result.success = false;
        result.errorCode = "NoProfiles";
        result.errorMessage = "Sweep requires at least one profile section";
        return result;
    }

    TopoShape spine = materializeOperand(_spine, /*fallbackTag=*/1);
    if (spine.isNull()) {
        result.success = false;
        result.errorCode = "InvalidSpine";
        result.errorMessage = "Spine shape is null";
        return result;
    }

    // makeElementPipeShell accepts a spine that can form a single wire (edge/wire/face outline).
    App::StringHasherRef hasher = spine.Hasher;
    std::vector<TopoShape> shapes;
    shapes.push_back(spine);

    long nextTag = spine.Tag == 0 ? 2 : spine.Tag + 1;
    for (const auto& profBundle : _profiles) {
        TopoShape profile = materializeOperand(profBundle, nextTag);
        if (profile.Tag == spine.Tag || profile.Tag == 0) {
            profile.Tag = nextTag;
        }
        ++nextTag;
        if (profile.isNull()) {
            result.success = false;
            result.errorCode = "InvalidProfile";
            result.errorMessage = "A sweep profile shape is null";
            return result;
        }
        if (!hasher && profile.Hasher) {
            hasher = profile.Hasher;
        }
        shapes.push_back(std::move(profile));
    }

    if (!hasher) {
        hasher = App::StringHasherRef(new App::StringHasher);
    }
    for (auto& shape : shapes) {
        if (!shape.Hasher) {
            shape.Hasher = hasher;
        }
    }

    ctx.reportProgress(0.5, "sweep.compute");

    try {
        if (ctx.isCancelled()) {
            result.success = false;
            result.errorCode = "Cancelled";
            result.errorMessage = "Cancelled during sweep compute";
            return result;
        }

        TopoShape outShape(0, hasher);
        outShape.makeElementPipeShell(
            shapes,
            _isSolid ? MakeSolid::makeSolid : MakeSolid::noSolid,
            /*isFrenet=*/Standard_False,
            TransitionMode::Transformed,
            OpCodes::Sweep);

        if (outShape.isNull()) {
            result.success = false;
            result.errorCode = "NullResult";
            result.errorMessage = "Mapped sweep produced a null shape";
            return result;
        }

        ctx.reportProgress(0.8, "sweep.archive");
        FrozenTopoShapeBundle outBundle = TopoShapeArchive::createBundle(outShape);
        if (!outBundle.valid) {
            result.success = false;
            result.errorCode = outBundle.errorCode.empty() ? "BundleInvalid" : outBundle.errorCode;
            result.errorMessage = "Failed to freeze mapped sweep result";
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
            result.errorMessage = "Failed to write sweep output archive";
        }
    }
    catch (const Base::Exception& e) {
        result.success = false;
        result.errorCode = "SweepError";
        result.errorMessage = e.what();
    }
    catch (const Standard_Failure& e) {
        result.success = false;
        result.errorCode = "OCCError";
        result.errorMessage = e.GetMessageString() ? e.GetMessageString()
                                                   : "OCC Exception during sweep";
    }
    catch (const std::exception& e) {
        result.success = false;
        result.errorCode = "Exception";
        result.errorMessage = e.what();
    }

    ctx.reportProgress(1.0, "sweep.complete");
    return result;
}

App::GeometryArchiveWriteResult
SweepGeometryOperation::writeArchive(App::GeometryArchiveWriter& writer) const
{
    (void)writer;
    App::GeometryArchiveWriteResult out;
    out.success = true; // Typed Sweep codec not yet implemented; no operands to stage.
    return out;
}

} // namespace Part
