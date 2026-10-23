// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <Geom_BSplineCurve.hxx>
#include <Geom_OffsetCurve.hxx>
#include <Geom_Circle.hxx>
#include <GeomConvert.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Mod/Sketcher/App/BSplineOffset.h>

namespace
{
using V = Base::Vector3d;
using Sketcher::BSplineOffsetOptions;
using Sketcher::BSplineOffsetStatus;
using Sketcher::makeBSplineOffset;

Part::GeomBSplineCurve cubic()
{
    return Part::GeomBSplineCurve(
        {V(0, 0, 0), V(3, 4, 0), V(7, 4, 0), V(10, 0, 0)},
        {1, 1, 1, 1},
        {0, 1},
        {4, 4},
        3
    );
}

Part::GeomBSplineCurve rational()
{
    // Exact quarter circle, radius 10; curvature is constant and known.
    return Part::GeomBSplineCurve(
        {V(10, 0, 0), V(10, 10, 0), V(0, 10, 0)},
        {1, std::sqrt(0.5), 1},
        {0, 1},
        {3, 3},
        2
    );
}

Handle(Geom_BSplineCurve) handle(const Part::GeomBSplineCurve& curve)
{
    return Handle(Geom_BSplineCurve)::DownCast(curve.handle());
}

void checkOffset(const Part::GeomBSplineCurve& source, const BSplineOffsetOptions& options)
{
    auto result = makeBSplineOffset(source, options);
    ASSERT_EQ(result.status, BSplineOffsetStatus::Success) << result.message;
    ASSERT_NE(result.curve, nullptr);
    ASSERT_LE(result.achievedMaxError, options.tolerance);
    auto input = handle(source);
    auto output = handle(*result.curve);
    Geom_OffsetCurve exact(
        input,
        options.distance,
        gp_Dir(options.sketchNormal.x, options.sketchNormal.y, options.sketchNormal.z)
    );
    double measured = 0.0;
    for (int i = 0; i <= 1000; ++i) {
        double u = input->FirstParameter()
            + (input->LastParameter() - input->FirstParameter()) * i / 1000.0;
        auto point = output->Value(u);
        const double error = point.Distance(exact.Value(u));
        measured = std::max(measured, error);
        EXPECT_LE(error, options.tolerance);
        // Separately check physical distance to source by projection, including
        // endpoints (projection can legitimately have no interior solution).
        GeomAPI_ProjectPointOnCurve projection(point, input);
        double distance
            = std::min(point.Distance(input->StartPoint()), point.Distance(input->EndPoint()));
        if (projection.NbPoints() > 0) {
            distance = std::min(distance, projection.LowerDistance());
        }
        EXPECT_NEAR(distance, std::abs(options.distance), options.tolerance);
    }
    EXPECT_LE(measured, result.achievedMaxError);
    EXPECT_EQ(result.complexity.inputPoles, input->NbPoles());
    EXPECT_EQ(result.complexity.outputPoles, output->NbPoles());
    EXPECT_EQ(result.complexity.segments, output->NbKnots() - 1);
    EXPECT_EQ(result.complexity.degree, output->Degree());
}

void rejected(
    const Part::GeomBSplineCurve& source,
    const BSplineOffsetOptions& options,
    BSplineOffsetStatus status
)
{
    auto result = makeBSplineOffset(source, options);
    EXPECT_EQ(result.status, status) << result.message;
    EXPECT_EQ(result.curve, nullptr);
    EXPECT_FALSE(result.message.empty());
}

TEST(BSplineOffset, RegularCubic)
{
    checkOffset(cubic(), {});
}

TEST(BSplineOffset, BothSigns)
{
    BSplineOffsetOptions options;
    options.distance = -1.0;
    checkOffset(cubic(), options);
}

TEST(BSplineOffset, RationalPositiveWeights)
{
    checkOffset(rational(), {});
    BSplineOffsetOptions options;
    options.distance = -1;
    checkOffset(rational(), options);
}

TEST(BSplineOffset, RationalCubic)
{
    Part::GeomBSplineCurve source(
        {V(0, 0, 0), V(3, 4, 0), V(7, 4, 0), V(10, 0, 0)},
        {1, 1.5, 0.8, 1},
        {0, 1},
        {4, 4},
        3
    );
    checkOffset(source, {});
}

TEST(BSplineOffset, SmoothMultipleKnotSpans)
{
    Part::GeomBSplineCurve source(
        {V(0, 0, 0), V(2, 1, 0), V(4, 2, 0), V(6, 2, 0), V(8, 1, 0), V(10, 0, 0)},
        {1, 1, 1, 1, 1, 1},
        {0, 0.25, 0.75, 1},
        {4, 1, 1, 4},
        3
    );
    checkOffset(source, {});
    BSplineOffsetOptions options;
    options.maxSegments = 2;
    rejected(source, options, BSplineOffsetStatus::ComplexityExceeded);
}

TEST(BSplineOffset, StraightSpline)
{
    Part::GeomBSplineCurve
        source({V(0, 0, 0), V(3, 0, 0), V(7, 0, 0), V(10, 0, 0)}, {1, 1, 1, 1}, {0, 1}, {4, 4}, 3);
    checkOffset(source, {});
    auto result = makeBSplineOffset(source);
    ASSERT_NE(result.curve, nullptr);
    EXPECT_NEAR(handle(*result.curve)->StartPoint().Y(), -1.0, 1e-10);
}

TEST(BSplineOffset, NonDyadicParameterSpans)
{
    Part::GeomBSplineCurve source(
        {V(0, 0, 0), V(2, 1, 0), V(4, 2, 0), V(6, 2, 0), V(8, 1, 0), V(10, 0, 0)},
        {1, 1, 1, 1, 1, 1},
        {0.13, 0.37, 1.07, 2.61},
        {4, 1, 1, 4},
        3
    );
    checkOffset(source, {});
}

TEST(BSplineOffset, OriginalPoleIntervalsCoverRationalKnotConversion)
{
    // Non-dyadic knots, unequal rational weights, and translated poles exercise
    // interval knot insertion and its propagation into the reported error bound.
    Part::GeomBSplineCurve source(
        {V(10000, -20000, 0),
         V(10002, -19999, 0),
         V(10004, -19998, 0),
         V(10006, -19998, 0),
         V(10008, -19999, 0),
         V(10010, -20000, 0)},
        {1.0, 1.3, 0.9, 1.1, 0.8, 1.0},
        {0.13, 0.37, 1.07, 2.61},
        {4, 1, 1, 4},
        3
    );
    checkOffset(source, {});
}

TEST(BSplineOffset, CheapChecksRejectBeforeFitting)
{
    BSplineOffsetOptions options;
    options.maxInputPoles = 3;
    EXPECT_EQ(
        Sketcher::checkBSplineOffsetInput(cubic(), options),
        BSplineOffsetStatus::ComplexityExceeded
    );
    options.distance = 0;
    EXPECT_EQ(Sketcher::checkBSplineOffsetInput(cubic(), options), BSplineOffsetStatus::InvalidOptions);
}

TEST(BSplineOffset, ReversedSourceReversesSide)
{
    auto source = cubic();
    auto reversed = Part::GeomBSplineCurve(Handle(Geom_BSplineCurve
    )::DownCast(handle(source)->Reversed()));
    checkOffset(reversed, {});
    BSplineOffsetOptions options;
    options.distance = -1;
    auto left = makeBSplineOffset(source, options);
    auto right = makeBSplineOffset(reversed);
    ASSERT_NE(left.curve, nullptr) << left.message;
    ASSERT_NE(right.curve, nullptr) << right.message;
    for (int i = 0; i <= 100; ++i) {
        EXPECT_LE(
            handle(*left.curve)->Value(i / 100.0).Distance(handle(*right.curve)->Value(1 - i / 100.0)),
            2 * options.tolerance
        );
    }
}

TEST(BSplineOffset, ReversedNormalReversesSide)
{
    BSplineOffsetOptions options;
    options.sketchNormal = V(0, 0, -1);
    checkOffset(cubic(), options);
}

TEST(BSplineOffset, TightToleranceAccepted)
{
    BSplineOffsetOptions options;
    options.tolerance = 0.0001;
    checkOffset(cubic(), options);
}

TEST(BSplineOffset, TightToleranceFailsLimitedFit)
{
    BSplineOffsetOptions options;
    options.tolerance = 1e-7;
    options.maxDegree = 3;
    options.maxSegments = 1;
    auto result = makeBSplineOffset(cubic(), options);
    // OCCT versions can either refuse the fit themselves or underestimate its
    // error. In the latter case the independent validator must refuse it.
    EXPECT_TRUE(
        result.status == BSplineOffsetStatus::ApproximationFailed
        || result.status == BSplineOffsetStatus::VerificationFailed
    ) << result.message;
    EXPECT_EQ(result.curve, nullptr);
    EXPECT_FALSE(result.message.empty());
}

TEST(BSplineOffset, InvalidOptions)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (double value : {0.0, -0.01, 1e-9, nan, inf, 1.0}) {
        BSplineOffsetOptions options;
        options.tolerance = value;
        rejected(cubic(), options, BSplineOffsetStatus::InvalidOptions);
    }
    for (double value : {0.0, nan, inf, -inf}) {
        BSplineOffsetOptions options;
        options.distance = value;
        rejected(cubic(), options, BSplineOffsetStatus::InvalidOptions);
    }
    for (auto normal : {Base::Vector3d(0, 0, 0), Base::Vector3d(nan, 0, 1)}) {
        BSplineOffsetOptions options;
        options.sketchNormal = normal;
        rejected(cubic(), options, BSplineOffsetStatus::InvalidOptions);
    }
    std::vector<BSplineOffsetOptions> limits(8);
    limits[0].maxInputPoles = -1;
    limits[1].maxOutputPoles = 0;
    limits[2].maxSegments = 0;
    limits[3].maxDegree = 2;
    limits[4].maxDegree = 15;
    limits[5].maxValidationSpans = 0;
    limits[6].maxRefinementDepth = 0;
    limits[7].maxRefinementDepth = 25;
    for (const auto& options : limits) {
        rejected(cubic(), options, BSplineOffsetStatus::InvalidOptions);
    }
}

TEST(BSplineOffset, SourceUnchangedOnSuccessAndFailure)
{
    auto source = rational();
    const auto poles = source.getPoles();
    const auto weights = source.getWeights();
    const auto knots = source.getKnots();
    const auto mults = source.getMultiplicities();
    auto first = makeBSplineOffset(source);
    ASSERT_NE(first.curve, nullptr) << first.message;
    BSplineOffsetOptions options;
    options.distance = -11;
    auto failure = makeBSplineOffset(source, options);
    ASSERT_EQ(failure.curve, nullptr);
    EXPECT_EQ(source.getPoles(), poles);
    EXPECT_EQ(source.getWeights(), weights);
    EXPECT_EQ(source.getKnots(), knots);
    EXPECT_EQ(source.getMultiplicities(), mults);
    EXPECT_FALSE(handle(source)->IsPeriodic());
    EXPECT_EQ(handle(source)->Degree(), 2);
    handle(*first.curve)->SetPole(1, gp_Pnt(100, 100, 0));
    EXPECT_EQ(source.getPoles(), poles);
}

TEST(BSplineOffset, InputComplexityRejected)
{
    BSplineOffsetOptions options;
    options.maxInputPoles = 3;
    rejected(cubic(), options, BSplineOffsetStatus::ComplexityExceeded);
}

TEST(BSplineOffset, OutputComplexityRejected)
{
    BSplineOffsetOptions options;
    options.maxOutputPoles = 2;
    rejected(cubic(), options, BSplineOffsetStatus::ComplexityExceeded);
}

TEST(BSplineOffset, ValidationBudgetRejected)
{
    BSplineOffsetOptions options;
    options.maxValidationSpans = 16;
    options.tolerance = 1e-5;
    rejected(cubic(), options, BSplineOffsetStatus::ComplexityExceeded);
}

TEST(BSplineOffset, ZeroEndpointTangentRejected)
{
    Part::GeomBSplineCurve
        source({V(0, 0, 0), V(0, 0, 0), V(7, 4, 0), V(10, 0, 0)}, {1, 1, 1, 1}, {0, 1}, {4, 4}, 3);
    rejected(source, {}, BSplineOffsetStatus::ZeroTangent);
}

TEST(BSplineOffset, InteriorZeroTangentRejected)
{
    // x = (t - 0.5)^3, with a stationary point between the end samples.
    Part::GeomBSplineCurve source(
        {V(-0.125, 0, 0), V(0.125, 0, 0), V(-0.125, 0, 0), V(0.125, 0, 0)},
        {1, 1, 1, 1},
        {0, 1},
        {4, 4},
        3
    );
    rejected(source, {}, BSplineOffsetStatus::ZeroTangent);
}

TEST(BSplineOffset, ConcaveDistanceBeyondRadiusRejected)
{
    BSplineOffsetOptions options;
    options.distance = -11;
    rejected(rational(), options, BSplineOffsetStatus::SingularOffset);
    options.distance = 11;
    checkOffset(rational(), options);
}

TEST(BSplineOffset, CurvatureExtremumBetweenEndpointsRejected)
{
    // Quadratic parabola: curvature maximum at t=0.37, not a knot or midpoint.
    // C(t) = (10 t, 40 (t-0.37)^2). Its minimum radius is 1.25.
    Part::GeomBSplineCurve
        source({V(0, 5.476, 0), V(5, -9.324, 0), V(10, 15.876, 0)}, {1, 1, 1}, {0, 1}, {3, 3}, 2);
    BSplineOffsetOptions options;
    options.distance = -1.26;
    rejected(source, options, BSplineOffsetStatus::SingularOffset);
}

TEST(BSplineOffset, SelfCrossingRawOffsetRejected)
{
    // Open cubic with a transverse loop. Small offset retains the crossing
    // while remaining locally regular (curvature-radius check alone misses it).
    Part::GeomBSplineCurve
        source({V(0, 0, 0), V(10, 10, 0), V(-5, 10, 0), V(5, 0, 0)}, {1, 1, 1, 1}, {0, 1}, {4, 4}, 3);
    BSplineOffsetOptions options;
    options.distance = 0.1;
    options.tolerance = 0.001;
    rejected(source, options, BSplineOffsetStatus::SelfIntersection);
}

TEST(BSplineOffset, ClosedSeamCornerRejected)
{
    Part::GeomBSplineCurve
        source({V(0, 0, 0), V(3, 4, 0), V(7, 4, 0), V(0, 0, 0)}, {1, 1, 1, 1}, {0, 1}, {4, 4}, 3);
    rejected(source, {}, BSplineOffsetStatus::SeamCorner);
    EXPECT_NE(makeBSplineOffset(source).message.find("G0"), std::string::npos);
}

Part::GeomBSplineCurve circle(bool periodic = true)
{
    // Exact rational circle with C2 interior and a G1 seam, radius 10.
    auto c = GeomConvert::CurveToBSplineCurve(
        new Geom_Circle(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 10),
        Convert_QuasiAngular
    );
    if (!periodic) {
        c->Segment(c->FirstParameter(), c->LastParameter());
    }
    return Part::GeomBSplineCurve(c);
}

void checkCircle(bool periodic, double distance, bool reverse = false)
{
    auto source = circle(periodic);
    if (reverse) {
        handle(source)->Reverse();
    }
    const auto poles = source.getPoles();
    BSplineOffsetOptions options;
    options.distance = distance;
    auto result = makeBSplineOffset(source, options);
    ASSERT_EQ(result.status, BSplineOffsetStatus::Success) << result.message;
    ASSERT_NE(result.curve, nullptr);
    auto output = handle(*result.curve);
    EXPECT_TRUE(output->IsClosed());
    EXPECT_EQ(output->IsPeriodic(), periodic);
    EXPECT_EQ(source.getPoles(), poles);
    EXPECT_LE(result.achievedMaxError, options.tolerance);
    EXPECT_EQ(result.orientation, reverse ? -1 : 1);
    gp_Pnt p, q;
    gp_Vec a, b;
    output->D1(output->FirstParameter(), p, a);
    output->D1(output->LastParameter(), q, b);
    EXPECT_LT(p.Distance(q), 1e-10);
    EXPECT_LT(a.Angle(b), 1e-7);
    double radialError = 0;
    const double radius = 10 + (reverse ? -distance : distance);
    for (int i = 0; i <= 1000; ++i) {
        const double u = output->FirstParameter()
            + (output->LastParameter() - output->FirstParameter()) * i / 1000.0;
        const auto point = output->Value(u);
        radialError = std::max(radialError, std::abs(std::hypot(point.X(), point.Y()) - radius));
    }
    EXPECT_LE(radialError, result.achievedMaxError);
    std::cout << "exact radius=" << radius << " radial error=" << radialError
              << " certified error=" << result.achievedMaxError << std::endl;
}

TEST(BSplineOffset, PeriodicCircleOutward)
{
    checkCircle(true, 1);
}

TEST(BSplineOffset, PeriodicCircleInward)
{
    checkCircle(true, -1);
}

TEST(BSplineOffset, ClampedClosedCircle)
{
    checkCircle(false, 1);
    checkCircle(false, -1);
}

TEST(BSplineOffset, ClosedG1UnequalSeamSpeeds)
{
    // The seam is geometric G1, not parametric C1: endpoint speeds are 12/24.
    Part::GeomBSplineCurve source(
        {V(0, 0, 0), V(3, 0, 0), V(6, 10, 0), V(-6, 0, 0), V(0, 0, 0)},
        std::vector<double>(5, 1),
        {0, 1},
        {5, 5},
        4
    );
    BSplineOffsetOptions options;
    options.distance = 0.1;
    checkOffset(source, options);
    auto result = makeBSplineOffset(source, options);
    ASSERT_NE(result.curve, nullptr) << result.message;
    EXPECT_TRUE(handle(*result.curve)->IsClosed());
    EXPECT_FALSE(handle(*result.curve)->IsPeriodic());
}

TEST(BSplineOffset, ClosedReversedOrientation)
{
    checkCircle(true, 1, true);
    checkCircle(false, -1, true);
}

TEST(BSplineOffset, ClosedInwardInversionRejected)
{
    BSplineOffsetOptions options;
    options.distance = -11;
    rejected(circle(), options, BSplineOffsetStatus::SingularOffset);
    rejected(circle(false), options, BSplineOffsetStatus::SingularOffset);
}

TEST(BSplineOffset, SelfCrossingSourceWithSimpleOutwardOffsetRejected)
{
    std::vector<V> poles;
    std::vector<double> knots;
    for (int i = 0; i < 16; ++i) {
        const double t = i * std::acos(-1.0) / 8;
        poles.emplace_back(10 * std::cos(t), 2.5 * std::sin(t) + 3.1 * std::sin(3 * t), 0);
        knots.push_back(i);
    }
    knots.push_back(16);
    Part::GeomBSplineCurve source(
        poles, std::vector<double>(16, 1), knots, std::vector<int>(17, 1), 3, true
    );
    BSplineOffsetOptions options;
    options.distance = 0.1;
    auto result = makeBSplineOffset(source, options);
    EXPECT_EQ(result.status, BSplineOffsetStatus::SourceSelfIntersection) << result.message;
    EXPECT_EQ(result.curve, nullptr);
    EXPECT_EQ(result.orientation, 0);
    EXPECT_NE(result.message.find("Source"), std::string::npos) << result.message;
}

Part::GeomBSplineCurve ellipse(bool periodic, bool shifted, bool nonuniform)
{
    auto c = handle(circle());
    for (int i = 1; i <= c->NbPoles(); ++i) {
        auto p = c->Pole(i);
        // Rotate the circle before scaling to move the ellipse seam without
        // introducing the old G1 seam as a low-continuity interior knot.
        c->SetPole(i, shifted ? gp_Pnt(-p.Y(), p.X() / 10, 0)
                              : gp_Pnt(p.X(), p.Y() / 10, 0));
    }
    if (nonuniform) {
        // Preserve the exact rational ellipse with unequal knot intervals.
        const double first = c->FirstParameter(), length = c->LastParameter() - first;
        c->InsertKnot(first + 0.013 * length, 1, 1e-12);
        c->InsertKnot(first + 0.371 * length, 1, 1e-12);
    }
    if (!periodic) {
        c->Segment(c->FirstParameter(), c->LastParameter());
    }
    return Part::GeomBSplineCurve(c);
}

void checkEllipse(bool periodic, bool shifted, bool nonuniform)
{
    auto source = ellipse(periodic, shifted, nonuniform);
    auto input = handle(source);
    ASSERT_TRUE(input->IsRational());
    ASSERT_EQ(input->IsPeriodic(), periodic);
    if (!shifted) {
        EXPECT_LT(input->StartPoint().Distance(gp_Pnt(10, 0, 0)), 1e-10);
        gp_Pnt p;
        gp_Vec v, a;
        input->D2(input->FirstParameter(), p, v, a);
        EXPECT_NEAR(std::pow(v.Magnitude(), 3) / v.Crossed(a).Magnitude(), 0.1, 1e-10);
    }
    else {
        EXPECT_GT(input->StartPoint().Distance(gp_Pnt(10, 0, 0)), 1);
    }
    BSplineOffsetOptions options;
    options.distance = -0.11;
    rejected(source, options, BSplineOffsetStatus::SingularOffset);
    options.distance = -0.09;
    auto result = makeBSplineOffset(source, options);
    ASSERT_EQ(result.status, BSplineOffsetStatus::Success) << result.message;
    ASSERT_NE(result.curve, nullptr);
    EXPECT_TRUE(handle(*result.curve)->IsClosed());
    EXPECT_EQ(handle(*result.curve)->IsPeriodic(), periodic);
    Geom_OffsetCurve exact(input, options.distance, gp_Dir(0, 0, 1));
    double measured = 0;
    for (int i = 0; i <= 4000; ++i) {
        const double u = input->FirstParameter()
            + (input->LastParameter() - input->FirstParameter()) * i / 4000.0;
        measured = std::max(measured, handle(*result.curve)->Value(u).Distance(exact.Value(u)));
    }
    EXPECT_LE(measured, result.achievedMaxError);
    EXPECT_LE(result.achievedMaxError, options.tolerance);
    std::cout << "ellipse periodic=" << periodic << " shifted=" << shifted
              << " nonuniform=" << nonuniform << " measured error=" << measured
              << " certified error=" << result.achievedMaxError << std::endl;
}

TEST(BSplineOffset, PeriodicEllipseSeamCurvature)
{
    checkEllipse(true, false, false);
}

TEST(BSplineOffset, ClampedClosedEllipseSeamCurvature)
{
    checkEllipse(false, false, false);
}

TEST(BSplineOffset, PeriodicEllipseShiftedSeamCurvature)
{
    checkEllipse(true, true, false);
}

TEST(BSplineOffset, ClampedClosedEllipseShiftedSeamCurvature)
{
    checkEllipse(false, true, false);
}

TEST(BSplineOffset, PeriodicNonuniformRationalEllipseSeamCurvature)
{
    checkEllipse(true, false, true);
}

TEST(BSplineOffset, ClampedClosedNonuniformRationalEllipseSeamCurvature)
{
    checkEllipse(false, false, true);
}

TEST(BSplineOffset, ClosedBudgetRejected)
{
    BSplineOffsetOptions options;
    options.maxInputPoles = 3;
    rejected(circle(), options, BSplineOffsetStatus::ComplexityExceeded);
    options = {};
    options.maxOutputPoles = 2;
    rejected(circle(false), options, BSplineOffsetStatus::ComplexityExceeded);
}

TEST(BSplineOffset, PeriodicSeamCrossingRejected)
{
    // Exact degree-six loop with a transverse crossing at u=0.1 and 0.9:
    // x=100 u(1-u)(1-2u)((u-.5)^2-.16),
    // y=100 u^2(1-u)^2((u-.5)^2-.09).
    // The small loop straddles the seam at 0/1; the two crossing arms are
    // on opposite ends of the period. A blanket seam-adjacency exemption
    // must not hide this second contact away from the shared seam point.
    Part::GeomBSplineCurve source(
        {V(0, 0, 0),
         V(1.5, 0, 0),
         V(-82.0 / 15, 16.0 / 15, 0),
         V(0, -3.4, 0),
         V(82.0 / 15, 16.0 / 15, 0),
         V(-1.5, 0, 0),
         V(0, 0, 0)},
        std::vector<double>(7, 1),
        {0, 1},
        {7, 7},
        6
    );
    handle(source)->SetPeriodic();
    ASSERT_LT(handle(source)->Value(0.1).Distance(handle(source)->Value(0.9)), 1e-12);
    BSplineOffsetOptions options;
    options.distance = 0.0001;
    options.tolerance = 0.00001;
    rejected(source, options, BSplineOffsetStatus::SourceSelfIntersection);
}

TEST(BSplineOffset, LowContinuityRejected)
{
    Part::GeomBSplineCurve source(
        {V(0, 0, 0), V(1, 2, 0), V(2, 3, 0), V(3, 3, 0), V(4, 2, 0), V(5, 0, 0)},
        {1, 1, 1, 1, 1, 1},
        {0, 0.5, 1},
        {4, 2, 4},
        3
    );
    rejected(source, {}, BSplineOffsetStatus::UnsupportedCurve);
}

TEST(BSplineOffset, NonplanarRejected)
{
    Part::GeomBSplineCurve
        source({V(0, 0, 0), V(3, 4, 1), V(7, 4, 0), V(10, 0, 0)}, {1, 1, 1, 1}, {0, 1}, {4, 4}, 3);
    rejected(source, {}, BSplineOffsetStatus::UnsupportedCurve);
}

}  // namespace
