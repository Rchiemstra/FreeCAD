// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"
#ifndef _PreComp_
# include <BRepCheck_Analyzer.hxx>
# include <BRepExtrema_DistShapeShape.hxx>
# include <BRepGProp.hxx>
# include <BRep_Builder.hxx>
# include <GProp_GProps.hxx>
# include <Message_ProgressIndicator.hxx>
# include <Message_ProgressRange.hxx>
# include <Standard_ConstructionError.hxx>
# include <Standard_Failure.hxx>
# include <TopExp_Explorer.hxx>
# include <TopTools_ListOfShape.hxx>
# include <cmath>
# include <gp_Pnt.hxx>
#endif

#include "InterferenceDetection.h"
#include "FCBRepAlgoAPI_Common.h"

namespace Part
{
namespace
{

class CancelProgressIndicator: public Message_ProgressIndicator
{
public:
    explicit CancelProgressIndicator(const std::atomic<bool>* flag)
        : cancelFlag(flag)
    {}

    void Show(const Message_ProgressScope& /*theScope*/, const Standard_Boolean /*isForce*/) override
    {}

    Standard_Boolean UserBreak() override
    {
        return cancelFlag && cancelFlag->load(std::memory_order_relaxed) ? Standard_True
                                                                         : Standard_False;
    }

private:
    const std::atomic<bool>* cancelFlag = nullptr;
};

bool isCancelled(const InterferenceOptions& options)
{
    return options.cancelFlag && options.cancelFlag->load(std::memory_order_relaxed);
}

InterferenceResult makeResult(InterferenceKind kind, const char* diagnostic = "")
{
    InterferenceResult result;
    result.kind = kind;
    if (diagnostic && diagnostic[0] != '\0') {
        result.diagnostic = diagnostic;
    }
    return result;
}

Base::Vector3d toVector(const gp_Pnt& point)
{
    return Base::Vector3d(point.X(), point.Y(), point.Z());
}

InterferenceResult classifyCommon(
    const TopoDS_Shape& first,
    const TopoDS_Shape& second,
    const InterferenceOptions& options,
    double minimumDistance,
    const Base::Vector3d& pointOnFirst,
    const Base::Vector3d& pointOnSecond
)
{
    if (isCancelled(options)) {
        return makeResult(InterferenceKind::Cancelled, "Cancelled before Common");
    }

    try {
        FCBRepAlgoAPI_Common mkCommon;
        mkCommon.SetRunParallel(Standard_True);
        mkCommon.SetNonDestructive(Standard_True);

        TopTools_ListOfShape arguments;
        TopTools_ListOfShape tools;
        arguments.Append(first);
        tools.Append(second);
        mkCommon.SetArguments(arguments);
        mkCommon.SetTools(tools);
        // Intentionally do not call setAutoFuzzy(): classification must rely on
        // stored shape tolerances so a small gap is not turned into penetration.

        Handle(CancelProgressIndicator) progress = new CancelProgressIndicator(options.cancelFlag);
        try {
#if OCC_VERSION_HEX >= 0x070600
            mkCommon.Build(progress->Start());
#else
            mkCommon.SetProgressIndicator(progress);
            mkCommon.Build();
#endif
        }
        catch (const Standard_ConstructionError& exc) {
            if (isCancelled(options) || progress->UserBreak()
                || (exc.GetMessageString()
                    && std::string(exc.GetMessageString()).find("aborted") != std::string::npos)) {
                return makeResult(InterferenceKind::Cancelled, "Cancelled during Common");
            }
            throw;
        }

        if (isCancelled(options) || progress->UserBreak()) {
            return makeResult(InterferenceKind::Cancelled, "Cancelled during Common");
        }

        if (!mkCommon.IsDone()) {
            auto result = makeResult(InterferenceKind::Inconclusive, "Common operation failed");
            result.minimumDistance = minimumDistance;
            result.pointOnFirst = pointOnFirst;
            result.pointOnSecond = pointOnSecond;
            return result;
        }

        const TopoDS_Shape common = mkCommon.Shape();
        BRep_Builder builder;
        TopoDS_Compound solidCompound;
        builder.MakeCompound(solidCompound);

        double overlapVolume = 0.0;
        bool hasSolid = false;
        for (TopExp_Explorer explorer(common, TopAbs_SOLID); explorer.More(); explorer.Next()) {
            hasSolid = true;
            const TopoDS_Shape& solid = explorer.Current();
            builder.Add(solidCompound, solid);
            GProp_GProps props;
            BRepGProp::VolumeProperties(solid, props);
            overlapVolume += props.Mass();
        }

        InterferenceResult result;
        result.minimumDistance = minimumDistance;
        result.pointOnFirst = pointOnFirst;
        result.pointOnSecond = pointOnSecond;
        if (hasSolid) {
            result.kind = InterferenceKind::Penetration;
            result.commonShape = solidCompound;
            result.overlapVolume = overlapVolume;
            result.diagnostic = "Overlapping solid volume detected";
        }
        else {
            result.kind = InterferenceKind::Contact;
            result.commonShape = common;
            result.overlapVolume = 0.0;
            result.diagnostic = "Zero-distance contact without solid overlap";
        }
        return result;
    }
    catch (const Standard_Failure& exc) {
        auto result = makeResult(
            InterferenceKind::Inconclusive,
            exc.GetMessageString() ? exc.GetMessageString() : "OCCT failure during Common"
        );
        result.minimumDistance = minimumDistance;
        result.pointOnFirst = pointOnFirst;
        result.pointOnSecond = pointOnSecond;
        return result;
    }
    catch (...) {
        auto result = makeResult(InterferenceKind::Inconclusive, "Unknown failure during Common");
        result.minimumDistance = minimumDistance;
        result.pointOnFirst = pointOnFirst;
        result.pointOnSecond = pointOnSecond;
        return result;
    }
}

}  // namespace

InterferenceResult classifyInterference(
    const TopoDS_Shape& first,
    const TopoDS_Shape& second,
    const InterferenceOptions& options
)
{
    if (isCancelled(options)) {
        return makeResult(InterferenceKind::Cancelled, "Cancelled before classification");
    }

    if (first.IsNull() || second.IsNull()) {
        return makeResult(InterferenceKind::InvalidInput, "One or both input shapes are null");
    }

    if (options.clearance < 0.0 || !std::isfinite(options.clearance)
        || options.linearTolerance < 0.0 || !std::isfinite(options.linearTolerance)) {
        return makeResult(InterferenceKind::InvalidInput, "Clearance/tolerance must be finite and nonnegative");
    }

    if (!options.skipGeometryValidation) {
        try {
            const BRepCheck_Analyzer firstCheck(first);
            const BRepCheck_Analyzer secondCheck(second);
            if (!firstCheck.IsValid() || !secondCheck.IsValid()) {
                return makeResult(
                    InterferenceKind::InvalidInput,
                    "BRepCheck_Analyzer reported invalid input geometry"
                );
            }
        }
        catch (const Standard_Failure& exc) {
            return makeResult(
                InterferenceKind::Inconclusive,
                exc.GetMessageString() ? exc.GetMessageString()
                                       : "BRepCheck_Analyzer failed during validation"
            );
        }
        catch (...) {
            return makeResult(
                InterferenceKind::Inconclusive,
                "Unknown failure during BRepCheck_Analyzer validation"
            );
        }
    }

    if (isCancelled(options)) {
        return makeResult(InterferenceKind::Cancelled, "Cancelled after geometry validation");
    }

    const double tolerance =
        options.linearTolerance > 0.0 ? options.linearTolerance : Precision::Confusion();

    try {
        BRepExtrema_DistShapeShape distance;
        distance.SetDeflection(tolerance);
#if OCC_VERSION_HEX >= 0x070600
        distance.SetMultiThread(true);
#endif
        distance.LoadS1(first);
        distance.LoadS2(second);

        Handle(CancelProgressIndicator) progress = new CancelProgressIndicator(options.cancelFlag);
#if OCC_VERSION_HEX >= 0x070600
        // DistShapeShape does not always honor progress; still check the flag around Perform.
        (void)progress;
#endif
        distance.Perform();

        if (isCancelled(options)) {
            return makeResult(InterferenceKind::Cancelled, "Cancelled during distance check");
        }

        if (!distance.IsDone() || distance.NbSolution() <= 0) {
            return makeResult(
                InterferenceKind::Inconclusive,
                "BRepExtrema_DistShapeShape failed or returned no solution"
            );
        }

        const double minimumDistance = distance.Value();
        const Base::Vector3d pointOnFirst = toVector(distance.PointOnShape1(1));
        const Base::Vector3d pointOnSecond = toVector(distance.PointOnShape2(1));

        if (minimumDistance > options.clearance + tolerance) {
            InterferenceResult result = makeResult(InterferenceKind::Clear);
            result.minimumDistance = minimumDistance;
            result.pointOnFirst = pointOnFirst;
            result.pointOnSecond = pointOnSecond;
            return result;
        }

        if (minimumDistance > tolerance) {
            InterferenceResult result =
                makeResult(InterferenceKind::ClearanceViolation, "Positive gap within clearance");
            result.minimumDistance = minimumDistance;
            result.pointOnFirst = pointOnFirst;
            result.pointOnSecond = pointOnSecond;
            return result;
        }

        return classifyCommon(
            first,
            second,
            options,
            minimumDistance,
            pointOnFirst,
            pointOnSecond
        );
    }
    catch (const Standard_Failure& exc) {
        return makeResult(
            InterferenceKind::Inconclusive,
            exc.GetMessageString() ? exc.GetMessageString() : "OCCT failure during distance check"
        );
    }
    catch (...) {
        return makeResult(InterferenceKind::Inconclusive, "Unknown failure during distance check");
    }
}

}  // namespace Part
