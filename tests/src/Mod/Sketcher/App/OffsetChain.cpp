// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <GeomConvert.hxx>
#include <Geom_Line.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Mod/Sketcher/App/OffsetChain.h>

namespace
{
using V = Base::Vector3d;
using Chain = std::vector<std::unique_ptr<Part::GeomCurve>>;
using Sketcher::OffsetChainDistanceOptions;
using Sketcher::OffsetChainDistanceStatus;

void line(Chain& chain, V first, V last)
{
    auto curve = std::make_unique<Part::GeomLineSegment>();
    curve->setPoints(first, last);
    chain.push_back(std::move(curve));
}

void quarter(Chain& chain, double radius)
{
    chain.push_back(
        std::make_unique<Part::GeomBSplineCurve>(
            std::vector<V> {V(radius, 0, 0), V(radius, radius, 0), V(0, radius, 0)},
            std::vector<double> {1, std::sqrt(0.5), 1},
            std::vector<double> {0, 1},
            std::vector<int> {3, 3},
            2
        )
    );
}

void arc(Chain& chain, double radius, double first, double last)
{
    auto curve = std::make_unique<Part::GeomArcOfCircle>(
        new Geom_Circle(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), radius)
    );
    curve->setRange(first, last, false);
    chain.push_back(std::move(curve));
}

Chain mixed(double distance = 0, double displacement = 0)
{
    // G1 line -> exact rational quarter circle -> analytic quarter circle.
    Chain chain;
    const double radius = 10 + distance;
    line(chain, V(radius + displacement, -5, 0), V(radius + displacement, 0, 0));
    quarter(chain, radius);
    arc(chain, radius, std::acos(-1.0) / 2, std::acos(-1.0));
    return chain;
}

std::vector<const Part::GeomCurve*> view(const Chain& chain)
{
    std::vector<const Part::GeomCurve*> result;
    for (const auto& curve : chain) {
        result.push_back(curve.get());
    }
    return result;
}

auto certify(const Chain& source, const Chain& candidate, OffsetChainDistanceOptions options = {})
{
    return Sketcher::certifyOffsetChainDistance(view(source), view(candidate), options);
}

TEST(OffsetChainDistance, MixedExactOutward)
{
    const auto result = certify(mixed(), mixed(1));
    EXPECT_EQ(result.status, OffsetChainDistanceStatus::Success) << result.message;
}

TEST(OffsetChainDistance, MixedExactInward)
{
    OffsetChainDistanceOptions options;
    options.distance = -1;
    const auto result = certify(mixed(), mixed(-1), options);
    EXPECT_EQ(result.status, OffsetChainDistanceStatus::Success) << result.message;
}

TEST(OffsetChainDistance, DisplacedSpan)
{
    // Distance fails even without asking this slice to check candidate joins.
    const auto result = certify(mixed(), mixed(1, 0.05));
    EXPECT_NE(result.status, OffsetChainDistanceStatus::Success);
    EXPECT_FALSE(result.message.empty());
}

TEST(OffsetChainDistance, GlobalClearanceIncludesOtherSourceBranches)
{
    auto source = mixed();
    // Continue the ordered source back beside the candidate's first line.
    // Corners/topology are deliberately outside this distance-only slice.
    line(source, V(-10, 0, 0), V(-10, -10, 0));
    line(source, V(-10, -10, 0), V(10.8, -10, 0));
    line(source, V(10.8, -10, 0), V(10.8, -2, 0));
    const auto result = certify(source, mixed(1));
    EXPECT_EQ(result.status, OffsetChainDistanceStatus::NotCertified);
    EXPECT_NE(result.message.find("clearance"), std::string::npos) << result.message;
}

TEST(OffsetChainDistance, WrongSide)
{
    const auto result = certify(mixed(), mixed(-1));
    EXPECT_NE(result.status, OffsetChainDistanceStatus::Success);
    EXPECT_NE(result.message.find("side"), std::string::npos) << result.message;
}

TEST(OffsetChainDistance, TinyBudget)
{
    OffsetChainDistanceOptions options;
    options.maxWork = 1;
    const auto result = certify(mixed(), mixed(1), options);
    EXPECT_EQ(result.status, OffsetChainDistanceStatus::BudgetExceeded);
    EXPECT_NE(result.message.find("budget"), std::string::npos) << result.message;
}

TEST(OffsetChainDistance, ClosedMixedCircleBothSigns)
{
    Chain source;
    quarter(source, 10);
    arc(source, 10, std::acos(-1.0) / 2, 2 * std::acos(-1.0));
    for (double distance : {1.0, -1.0}) {
        Chain candidate;
        quarter(candidate, 10 + distance);
        arc(candidate, 10 + distance, std::acos(-1.0) / 2, 2 * std::acos(-1.0));
        OffsetChainDistanceOptions options;
        options.distance = distance;
        const auto result = certify(source, candidate, options);
        EXPECT_EQ(result.status, OffsetChainDistanceStatus::Success) << result.message;
    }
}

TEST(OffsetChainDistance, EndpointsAreSourceWitnesses)
{
    Chain source, candidate;
    line(source, V(0, 0, 0), V(0.001, 0, 0));
    line(candidate, V(-0.001, -1, 0), V(0.002, -1, 0));
    EXPECT_EQ(certify(source, candidate).status, OffsetChainDistanceStatus::Success);
}

TEST(OffsetChainDistance, NarrowBetweenSampleClearanceViolation)
{
    Chain source, candidate;
    line(source, V(0, 0, 0), V(10, 0, 0));
    // A triangular excursion only 2e-8 wide in both x and native parameter.
    // Uniform whole-curve sampling misses it unless it happens to hit the tip.
    const double x = 3.7123456789, width = 1e-8;
    candidate.push_back(std::make_unique<Part::GeomBSplineCurve>(
        std::vector<V> {V(0, -1, 0), V(x - width, -1, 0), V(x, -1.02, 0),
                        V(x + width, -1, 0), V(10, -1, 0)},
        std::vector<double> {1, 1, 1, 1, 1},
        std::vector<double> {0, x - width, x, x + width, 10},
        std::vector<int> {2, 1, 1, 1, 2}, 1
    ));
    const auto result = certify(source, candidate);
    EXPECT_NE(result.status, OffsetChainDistanceStatus::Success) << result.message;
}

TEST(OffsetChainDistance, ToleranceBoundary)
{
    for (double distance : {0.99001, 1.00999, 0.98999, 1.01001}) {
        SCOPED_TRACE(distance);
        Chain source, candidate;
        line(source, V(0, 0, 0), V(0.001, 0, 0));
        line(candidate, V(0.00025, -distance, 0), V(0.00075, -distance, 0));
        const auto result = certify(source, candidate);
        EXPECT_EQ(result.status, distance > 0.99 && distance < 1.01
                      ? OffsetChainDistanceStatus::Success
                      : OffsetChainDistanceStatus::NotCertified) << result.message;
    }
}

TEST(OffsetChainDistance, UnequalWeightMultipleSpans)
{
    Chain source, candidate;
    for (double y : {0.0, -1.0}) {
        auto curve = std::make_unique<Part::GeomBSplineCurve>(
            std::vector<V> {V(0, y, 0), V(0.002, y, 0), V(0.004, y, 0),
                            V(0.006, y, 0), V(0.008, y, 0)},
            std::vector<double> {1, 2, 1, 3, 1},
            std::vector<double> {0, 0.3, 0.7, 1}, std::vector<int> {3, 1, 1, 3}, 2
        );
        (y == 0 ? source : candidate).push_back(std::move(curve));
    }
    const auto result = certify(source, candidate);
    EXPECT_EQ(result.status, OffsetChainDistanceStatus::Success) << result.message;
}

TEST(OffsetChainDistance, ClockwiseArc)
{
    Chain source, candidate;
    for (double radius : {10.0, 9.0}) {
        auto curve = std::make_unique<Part::GeomArcOfCircle>(new Geom_Circle(
            gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, -1)), radius
        ));
        curve->setRange(0, 0.1, false);
        (radius == 10 ? source : candidate).push_back(std::move(curve));
    }
    const auto result = certify(source, candidate);
    EXPECT_EQ(result.status, OffsetChainDistanceStatus::Success) << result.message;
}

TEST(OffsetChainDistance, SeamCrossingArc)
{
    Chain source, candidate;
    arc(source, 10, 6.2, 6.4);
    arc(candidate, 11, 6.2, 6.4);
    const auto result = certify(source, candidate);
    EXPECT_EQ(result.status, OffsetChainDistanceStatus::Success) << result.message;
}

TEST(OffsetChainDistance, OppositeSideNearestTies)
{
    Chain source, candidate;
    // The first nearest branch has the requested side. The other equally near
    // branch has the opposite side; a single nearest-point witness is unsound.
    line(source, V(0, 2, 0), V(2, 2, 0));
    line(source, V(2, 2, 0), V(2, 3, 0));
    line(source, V(2, 3, 0), V(-2, 3, 0));
    line(source, V(-2, 3, 0), V(-2, 0, 0));
    line(source, V(-2, 0, 0), V(2, 0, 0));
    line(candidate, V(0.00025, 1, 0), V(0.00075, 1, 0));
    EXPECT_NE(certify(source, candidate).status, OffsetChainDistanceStatus::Success);
}

TEST(OffsetChainDistance, CornerOneSidedTangents)
{
    for (bool opposing : {false, true}) {
        SCOPED_TRACE(opposing);
        Chain source, candidate;
        line(source, V(-0.001, 0, 0), V(0, 0, 0));
        line(source, V(0, 0, 0), opposing ? V(-0.001, -0.001, 0) : V(0, 0.001, 0));
        const V q(2 / std::sqrt(5.0), -1 / std::sqrt(5.0), 0);
        line(candidate, q, q * 1.0001);
        const auto result = certify(source, candidate);
        if (opposing) {
            EXPECT_NE(result.status, OffsetChainDistanceStatus::Success);
        }
        else {
            EXPECT_EQ(result.status, OffsetChainDistanceStatus::Success) << result.message;
        }
    }
}

TEST(OffsetChainDistance, DegenerateSplineSpan)
{
    for (bool inSource : {false, true}) {
        SCOPED_TRACE(inSource);
        Chain source, candidate;
        line(source, V(0, 0, 0), V(0.001, 0, 0));
        line(candidate, V(0.00025, -1, 0), V(0.00075, -1, 0));
        auto& chain = inSource ? source : candidate;
        const V point = inSource ? V(0, 0, 0) : V(0.0005, -1, 0);
        chain.push_back(std::make_unique<Part::GeomBSplineCurve>(
            std::vector<V> {point, point}, std::vector<double> {1, 1},
            std::vector<double> {0, 1}, std::vector<int> {2, 2}, 1
        ));
        EXPECT_EQ(certify(source, candidate).status, OffsetChainDistanceStatus::InvalidInput);
    }
}

TEST(OffsetChainDistance, DegenerateInteriorSplineSpan)
{
    for (bool inSource : {false, true}) {
        SCOPED_TRACE(inSource);
        Chain source, candidate;
        line(source, V(0, 0, 0), V(0.001, 0, 0));
        line(candidate, V(0.00025, -1, 0), V(0.00075, -1, 0));
        const double y = inSource ? 0 : -1;
        (inSource ? source : candidate).push_back(std::make_unique<Part::GeomBSplineCurve>(
            std::vector<V> {V(0, y, 0), V(0.0005, y, 0), V(0.0005, y, 0), V(0.001, y, 0)},
            std::vector<double> {1, 2, 3, 1}, std::vector<double> {0, 0.3, 0.7, 1},
            std::vector<int> {2, 1, 1, 2}, 1
        ));
        OffsetChainDistanceOptions options;
        options.maxWork = 10000;
        EXPECT_EQ(
            certify(source, candidate, options).status, OffsetChainDistanceStatus::InvalidInput
        );
    }
}

TEST(OffsetChainDistance, ExtremeTranslation)
{
    Chain source, candidate;
    const V origin(1e12, -1e12, 0);
    line(source, origin, origin + V(0.01, 0, 0));
    line(candidate, origin + V(0.0025, -1, 0), origin + V(0.0075, -1, 0));
    const auto result = certify(source, candidate);
    EXPECT_EQ(result.status, OffsetChainDistanceStatus::Success) << result.message;
}

TEST(OffsetChainDistance, ExtremeScale)
{
    for (double scale : {1e-4, 1e100}) {
        SCOPED_TRACE(scale);
        Chain source, candidate;
        line(source, V(0, 0, 0), V(0.001 * scale, 0, 0));
        line(candidate, V(0.00025 * scale, -scale, 0), V(0.00075 * scale, -scale, 0));
        OffsetChainDistanceOptions options;
        options.distance = scale;
        options.tolerance = 0.01 * scale;
        const auto result = certify(source, candidate, options);
        EXPECT_EQ(result.status, OffsetChainDistanceStatus::Success) << result.message;
    }
}

TEST(OffsetChainDistance, DepthExhaustion)
{
    OffsetChainDistanceOptions options;
    options.maxDepth = 1;
    const auto result = certify(mixed(), mixed(1), options);
    EXPECT_EQ(result.status, OffsetChainDistanceStatus::BudgetExceeded);
    EXPECT_NE(result.message.find("refinement"), std::string::npos) << result.message;
    EXPECT_LT(result.work, options.maxWork);
}

TEST(OffsetChainDistance, NonfiniteNativeLine)
{
    for (bool inSource : {false, true}) {
        SCOPED_TRACE(inSource);
        Chain source, candidate;
        line(source, V(0, 0, 0), V(0.001, 0, 0));
        line(candidate, V(0.00025, -1, 0), V(0.00075, -1, 0));
        Handle(Geom_Line) native = new Geom_Line(
            gp_Pnt(std::numeric_limits<double>::infinity(), 0, 0), gp_Dir(1, 0, 0)
        );
        auto invalid = std::make_unique<Part::GeomLineSegment>();
        invalid->setHandle(new Geom_TrimmedCurve(native, 0, 1));
        (inSource ? source : candidate).push_back(std::move(invalid));
        EXPECT_EQ(certify(source, candidate).status, OffsetChainDistanceStatus::InvalidInput);
    }
}

TEST(OffsetChainDistance, InvalidNativeDefinitions)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (bool inSource : {false, true}) {
        for (int kind = 0; kind < 6; ++kind) {
            SCOPED_TRACE(inSource);
            SCOPED_TRACE(kind);
            Chain source, candidate;
            line(source, V(0, 0, 0), V(0.001, 0, 0));
            line(candidate, V(0.00025, -1, 0), V(0.00075, -1, 0));
            auto& chain = inSource ? source : candidate;
            if (kind == 0) {
                Handle(Geom_Line) native = new Geom_Line(gp_Pnt(0, 0, 0), gp_Dir(nan, 1, 0));
                auto curve = std::make_unique<Part::GeomLineSegment>();
                curve->setHandle(new Geom_TrimmedCurve(native, 0, 1));
                chain.push_back(std::move(curve));
            }
            else {
                Handle(Geom_Circle) native = new Geom_Circle(
                    gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 1
                );
                if (kind == 1) {
                    native->SetLocation(gp_Pnt(0, inf, 0));
                }
                else if (kind == 2) {
                    native->SetAxis(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, nan, 1)));
                }
                else {
                    native->SetRadius(kind == 3 ? inf : kind == 4 ? nan : 0);
                }
                auto curve = std::make_unique<Part::GeomArcOfCircle>(native);
                curve->setRange(0, 0.001, false);
                chain.push_back(std::move(curve));
            }
            const auto result = certify(source, candidate);
            EXPECT_EQ(result.status, OffsetChainDistanceStatus::InvalidInput) << result.message;
        }
    }
}

TEST(OffsetChainDistance, InvalidSplineDefinitions)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (bool inSource : {false, true}) {
        for (int kind = 0; kind < 8; ++kind) {
            SCOPED_TRACE(inSource);
            SCOPED_TRACE(kind);
            Chain source, candidate;
            line(source, V(0, 0, 0), V(0.001, 0, 0));
            line(candidate, V(0.00025, -1, 0), V(0.00075, -1, 0));
            auto curve = std::make_unique<Part::GeomBSplineCurve>(
                std::vector<V> {V(0, 0, 0), V(0.0005, 0, 0), V(0.001, 0, 0)},
                std::vector<double> {1, 2, 1}, std::vector<double> {0, 0.5, 1},
                std::vector<int> {2, 1, 2}, 1
            );
            auto native = Handle(Geom_BSplineCurve)::DownCast(curve->handle());
            if (kind == 0) {
                native->SetPole(2, gp_Pnt(inf, 0, 0));
            }
            else if (kind <= 4) {
                // Bypass setters which can normalize or reject malformed data.
                auto* weights = const_cast<TColStd_Array1OfReal*>(native->Weights());
                ASSERT_NE(weights, nullptr);
                const double values[] {nan, inf, 0, -1};
                weights->SetValue(2, values[kind - 1]);
            }
            else {
                auto& knots = const_cast<TColStd_Array1OfReal&>(native->Knots());
                knots.SetValue(2, kind == 5 ? nan : kind == 6 ? inf : 0);
            }
            (inSource ? source : candidate).push_back(std::move(curve));
            OffsetChainDistanceOptions options;
            options.maxWork = 10000;
            const auto result = certify(source, candidate, options);
            EXPECT_EQ(result.status, OffsetChainDistanceStatus::InvalidInput) << result.message;
        }
    }
}

TEST(OffsetChainDistance, NativeArcEndpointPolicy)
{
    for (bool inSource : {false, true}) {
        for (bool lowerEndpoint : {false, true}) {
            SCOPED_TRACE(inSource);
            SCOPED_TRACE(lowerEndpoint);
            Chain source, candidate;
            line(source, V(0, 0, 0), V(0.001, 0, 0));
            line(candidate, V(0.00025, -1, 0), V(0.00075, -1, 0));
            Handle(Geom_Circle) native = new Geom_Circle(
                gp_Ax2(gp_Pnt(0.0005, inSource ? 100 : -1, 0), gp_Dir(0, 0, 1)), 0.0001
            );
            const double first = lowerEndpoint ? -8.1 : 6;
            const double last = lowerEndpoint ? -6 : 8.1;
            auto curve = std::make_unique<Part::GeomArcOfCircle>();
            // Preserve native parameters, including the negative lower endpoint.
            curve->setHandle(new Geom_TrimmedCurve(native, first, last, true, false));
            const auto trimmed = Handle(Geom_TrimmedCurve)::DownCast(curve->handle());
            trimmed->SetTrim(first, last, true, false);
            ASSERT_DOUBLE_EQ(trimmed->FirstParameter(), first);
            ASSERT_DOUBLE_EQ(trimmed->LastParameter(), last);
            (inSource ? source : candidate).push_back(std::move(curve));
            EXPECT_EQ(
                certify(source, candidate).status, OffsetChainDistanceStatus::UnsupportedCurve
            );
        }
    }
}

TEST(OffsetChainDistance, ReversedNormalReversesSide)
{
    OffsetChainDistanceOptions options;
    options.sketchNormal = V(0, 0, -1);
    const auto result = certify(mixed(), mixed(-1), options);
    EXPECT_EQ(result.status, OffsetChainDistanceStatus::Success) << result.message;
}

TEST(OffsetChainDistance, InvalidInput)
{
    OffsetChainDistanceOptions options;
    options.tolerance = 0;
    EXPECT_EQ(certify(mixed(), mixed(1), options).status, OffsetChainDistanceStatus::InvalidInput);
    EXPECT_EQ(certify({}, mixed(1)).status, OffsetChainDistanceStatus::InvalidInput);
    EXPECT_EQ(
        Sketcher::certifyOffsetChainDistance({nullptr}, {}, {}).status,
        OffsetChainDistanceStatus::InvalidInput
    );
}

using Sketcher::OffsetChainTopologyOptions;
using Sketcher::OffsetChainTopologyStatus;

std::vector<Sketcher::OffsetChainPiece> pieces(const Chain& chain)
{
    std::vector<Sketcher::OffsetChainPiece> result;
    for (size_t i = 0; i < chain.size(); ++i) {
        result.push_back({chain[i].get(), i});
    }
    return result;
}

auto topology(const Chain& chain, OffsetChainTopologyOptions options = {})
{
    return Sketcher::certifyOffsetChainTopology(pieces(chain), options);
}

void bezierCurve(Chain& chain, const std::vector<V>& poles)
{
    const int degree = static_cast<int>(poles.size()) - 1;
    chain.push_back(std::make_unique<Part::GeomBSplineCurve>(
        poles, std::vector<double>(poles.size(), 1), std::vector<double> {0, 1},
        std::vector<int> {degree + 1, degree + 1}, degree
    ));
}

void nativeLine(Chain& chain, V start, V direction, double length)
{
    auto curve = std::make_unique<Part::GeomLineSegment>();
    curve->setHandle(new Geom_TrimmedCurve(
        new Geom_Line(gp_Pnt(start.x, start.y, start.z),
                      gp_Dir(direction.x, direction.y, direction.z)), 0, length
    ));
    chain.push_back(std::move(curve));
}

TEST(OffsetChainTopology, ShortLineCannotClose)
{
    Chain chain;
    nativeLine(chain, V(0, 0, 0), V(1, 0, 0), std::ldexp(1.0, -24));
    OffsetChainTopologyOptions options;
    options.closed = true;
    const auto result = topology(chain, options);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::NotCertified) << result.message;
    EXPECT_EQ(result.message, "Piece length is not safely above join tolerance.");
}

TEST(OffsetChainTopology, ToleratedReversedJunction)
{
    Chain chain;
    nativeLine(chain, V(0, 0, 0), V(1, 0, 0), 1);
    nativeLine(chain, V(1 + std::ldexp(1.0, -24), 0, 0), V(-1, 0, 0),
               std::ldexp(1.0, -25));
    const auto result = topology(chain);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::NotCertified) << result.message;
    EXPECT_EQ(result.message, "Piece length is not safely above join tolerance.");
}

TEST(OffsetChainTopology, DisjointReversedJunctionNeedsLocalProof)
{
    // Both pieces are long; disabling the short-piece guard cannot admit this join.
    Chain chain;
    nativeLine(chain, V(0, 0, 0), V(1, 0, 0), 1);
    nativeLine(chain, V(1, std::ldexp(1.0, -24), 0), V(-1, 0, 0), 0.5);
    const auto result = topology(chain);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::BudgetExceeded) << result.message;
    EXPECT_EQ(result.message, "Topology certificate budget exhausted.");
}

TEST(OffsetChainTopology, DisjointReversedClosingSeamNeedsLocalProof)
{
    const double gap = std::ldexp(1.0, -24);
    Chain chain;
    line(chain, V(0, 0, 0), V(1, 0, 0));
    line(chain, V(1, 0, 0), V(1, -1, 0));
    line(chain, V(1, -1, 0), V(2, -1, 0));
    line(chain, V(2, -1, 0), V(2, gap, 0));
    line(chain, V(2, gap, 0), V(0, gap, 0));
    OffsetChainTopologyOptions options;
    options.closed = true;
    const auto result = topology(chain, options);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::BudgetExceeded) << result.message;
    EXPECT_EQ(result.message, "Topology certificate budget exhausted.");
}

TEST(OffsetChainTopology, PieceLengthAboveJoinTolerance)
{
    OffsetChainTopologyOptions options;
    for (double factor : {3.999, 4.001}) {
        SCOPED_TRACE(factor);
        Chain chain;
        nativeLine(chain, V(0, 0, 0), V(1, 0, 0), factor * options.joinTolerance);
        const auto result = topology(chain, options);
        EXPECT_EQ(result.status, factor < 4 ? OffsetChainTopologyStatus::NotCertified
                                           : OffsetChainTopologyStatus::Success)
            << result.message;
        if (factor < 4) {
            EXPECT_EQ(result.message, "Piece length is not safely above join tolerance.");
        }
    }
}

TEST(OffsetChainTopology, NonAdjacentTouchAndCrossAtExtremeScale)
{
    for (double scale : {1e-100, 1e100}) {
        for (bool cross : {false, true}) {
            SCOPED_TRACE(scale);
            SCOPED_TRACE(cross);
            Chain chain;
            line(chain, V(0, 0, 0), V(2 * scale, 0, 0));
            line(chain, V(2 * scale, 0, 0), V(2 * scale, scale, 0));
            line(chain, V(2 * scale, scale, 0), V(scale, scale, 0));
            line(chain, V(scale, scale, 0), V(scale, cross ? -scale : 0, 0));
            OffsetChainTopologyOptions options;
            options.joinTolerance = 0;
            const auto result = topology(chain, options);
            EXPECT_EQ(result.status, OffsetChainTopologyStatus::BudgetExceeded) << result.message;
            EXPECT_EQ(result.message, "Topology certificate budget exhausted.");
        }
    }
}

TEST(OffsetChainTopology, NonAdjacentTouchAndCrossAcrossArcSeam)
{
    for (bool cross : {false, true}) {
        SCOPED_TRACE(cross);
        Chain chain;
        arc(chain, 1, 5.5, 7);
        // A forward gap avoids relying on rounded trigonometric endpoint equality.
        const V start(std::cos(7) - 1e-8 * std::sin(7),
                      std::sin(7) + 1e-8 * std::cos(7), 0);
        line(chain, start, V(2, 2, 0));
        line(chain, V(2, 2, 0), V(cross ? 0.9 : 1, 2, 0));
        line(chain, V(cross ? 0.9 : 1, 2, 0), V(cross ? 0.9 : 1, -2, 0));
        const auto result = topology(chain);
        EXPECT_EQ(result.status, OffsetChainTopologyStatus::BudgetExceeded) << result.message;
        EXPECT_EQ(result.message, "Topology certificate budget exhausted.");
    }
}

TEST(OffsetChainTopology, ContinuousMixedOpen)
{
    auto chain = mixed();
    // Rotate the arc's native frame: its start parameter is exactly zero,
    // so the spline/arc junction is algebraically equal, without a rounded pi/2.
    auto circle = std::make_unique<Part::GeomArcOfCircle>(new Geom_Circle(
        gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(0, 1, 0)), 10
    ));
    circle->setRange(0, std::acos(-1.0) / 2, false);
    chain.back() = std::move(circle);
    const auto result = topology(chain);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::Success) << result.message;
}

TEST(OffsetChainTopology, SimpleClosed)
{
    Chain chain;
    line(chain, V(0, 0, 0), V(1, 0, 0));
    line(chain, V(1, 0, 0), V(1, 1, 0));
    line(chain, V(1, 1, 0), V(0, 1, 0));
    line(chain, V(0, 1, 0), V(0, 0, 0));
    OffsetChainTopologyOptions options;
    options.closed = true;
    const auto result = topology(chain, options);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::Success) << result.message;
    const auto open = topology(chain);
    EXPECT_EQ(open.status, OffsetChainTopologyStatus::NotCertified) << open.message;
    EXPECT_EQ(open.message, "Expected distinct open ends.");
}

TEST(OffsetChainTopology, JoinToleranceBoundary)
{
    OffsetChainTopologyOptions options;
    options.joinTolerance = 5e-8;
    for (double factor : {0.999, 1.001}) {
        SCOPED_TRACE(factor);
        Chain chain;
        line(chain, V(0, 0, 0), V(1, 0, 0));
        line(chain, V(1 + factor * options.joinTolerance, 0, 0), V(2, 0, 0));
        const auto result = topology(chain, options);
        EXPECT_EQ(result.status, factor < 1 ? OffsetChainTopologyStatus::Success
                                           : OffsetChainTopologyStatus::NotCertified)
            << result.message;
    }
}

TEST(OffsetChainTopology, AdjacentRecrossingNearJunction)
{
    Chain chain;
    line(chain, V(0, 0, 0), V(1, 0, 0));
    // G1 at the junction, then crosses the line again at t=0.1:
    // x(t) = 1 + 1e-5*t - 4e-4*t^2, y(t) = 1e-6*t^2*(t-0.1).
    bezierCurve(chain, {V(1, 0, 0), V(1 + 1e-5 / 3, 0, 0),
                         V(1 + 2e-5 / 3 - 4e-4 / 3, -1e-7 / 3, 0),
                         V(1 + 1e-5 - 4e-4, 9e-7, 0)});
    const auto result = topology(chain);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::BudgetExceeded) << result.message;
    EXPECT_EQ(result.message, "Topology certificate budget exhausted.");
}

TEST(OffsetChainTopology, TinyLoopInsideSingleSpan)
{
    Chain chain;
    // Cubic (u^2, u^3-epsilon^2*u), u=t-a, has a loop at u=+-epsilon.
    // All of it lies inside ONE native knot span, away from common sample grids.
    const double a = 0.37123456789, e = 1e-8;
    const double x0 = a * a, x1 = -2 * a;
    const double y0 = -a * a * a + e * e * a;
    const double y1 = 3 * a * a - e * e, y2 = -3 * a;
    bezierCurve(chain, {V(x0, y0, 0), V(x0 + x1 / 3, y0 + y1 / 3, 0),
                         V(x0 + 2 * x1 / 3 + 1.0 / 3, y0 + 2 * y1 / 3 + y2 / 3, 0),
                         V(x0 + x1 + 1, y0 + y1 + y2 + 1, 0)});
    const auto result = topology(chain);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::BudgetExceeded) << result.message;
    EXPECT_EQ(result.message, "Topology certificate budget exhausted.");
}

TEST(OffsetChainTopology, DuplicateOverlappingSpan)
{
    Chain chain;
    line(chain, V(0, 0, 0), V(1, 0, 0));
    line(chain, V(1, 0, 0), V(0, 0, 0));
    line(chain, V(0, 0, 0), V(1, 0, 0));
    const auto result = topology(chain);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::BudgetExceeded) << result.message;
    EXPECT_EQ(result.message, "Topology certificate budget exhausted.");
}

TEST(OffsetChainTopology, ClosingSeamCrossing)
{
    Chain chain;
    line(chain, V(0, 0, 0), V(1, 0, 0));
    line(chain, V(1, 0, 0), V(1, 1, 0));
    // Last piece crosses the first at x=1/3 before returning to the seam.
    bezierCurve(chain, {V(1, 1, 0), V(0.5, -1, 0), V(0, 0, 0)});
    OffsetChainTopologyOptions options;
    options.closed = true;
    const auto result = topology(chain, options);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::BudgetExceeded) << result.message;
    EXPECT_EQ(result.message, "Topology certificate budget exhausted.");
}

TEST(OffsetChainTopology, NonAdjacentNearTouchAboveClearance)
{
    OffsetChainTopologyOptions options;
    options.clearance = 1e-6;
    const double gap = options.clearance * 1.001;
    Chain chain;
    line(chain, V(0, 0, 0), V(1, 0, 0));
    line(chain, V(1, 0, 0), V(1, 1, 0));
    line(chain, V(1, 1, 0), V(0, 1, 0));
    line(chain, V(0, 1, 0), V(0, gap, 0));
    const auto result = topology(chain, options);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::Success) << result.message;
}

TEST(OffsetChainTopology, TinyWorkBudget)
{
    OffsetChainTopologyOptions options;
    options.maxWork = 1;
    const auto result = topology(mixed(), options);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::BudgetExceeded);
    EXPECT_NE(result.message.find("budget"), std::string::npos) << result.message;
}

TEST(OffsetChainTopology, SingleClosedSplineAndPeriodicSeam)
{
    for (bool periodic : {false, true}) {
        auto native = GeomConvert::CurveToBSplineCurve(
            new Geom_Circle(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 10),
            Convert_QuasiAngular
        );
        if (!periodic) {
            native->Segment(native->FirstParameter(), native->LastParameter());
        }
        Chain chain;
        chain.push_back(std::make_unique<Part::GeomBSplineCurve>(native));
        OffsetChainTopologyOptions options;
        options.closed = true;
        const auto result = topology(chain, options);
        EXPECT_EQ(result.status, OffsetChainTopologyStatus::Success) << result.message;
    }
}

TEST(OffsetChainTopology, InvalidDefinitionsAndIdentities)
{
    auto chain = mixed();
    auto input = pieces(chain);
    input[1].identity = input[0].identity;
    EXPECT_EQ(Sketcher::certifyOffsetChainTopology(input).status,
              OffsetChainTopologyStatus::InvalidInput);
    OffsetChainTopologyOptions options;
    options.joinTolerance = 1e-6;
    EXPECT_EQ(topology(chain, options).status, OffsetChainTopologyStatus::InvalidInput);
    EXPECT_EQ(topology({}).status, OffsetChainTopologyStatus::InvalidInput);
    EXPECT_EQ(Sketcher::certifyOffsetChainTopology({{nullptr, 0}}).status,
              OffsetChainTopologyStatus::InvalidInput);
    Chain degenerate;
    bezierCurve(degenerate, {V(1, 0, 0), V(1, 0, 0)});
    EXPECT_EQ(topology(degenerate).status, OffsetChainTopologyStatus::InvalidInput);
    auto native = Handle(Geom_BSplineCurve)::DownCast(chain[1]->handle());
    native->SetPole(2, gp_Pnt(std::numeric_limits<double>::infinity(), 0, 0));
    EXPECT_EQ(topology(chain).status, OffsetChainTopologyStatus::InvalidInput);
}

TEST(OffsetChainTopology, DepthExhaustionAndStationaryPoint)
{
    Chain chain;
    quarter(chain, 10);
    arc(chain, 10, std::acos(-1.0) / 2, 2 * std::acos(-1.0));
    OffsetChainTopologyOptions options;
    options.closed = true;
    options.maxDepth = 1;
    const auto result = topology(chain, options);
    EXPECT_EQ(result.status, OffsetChainTopologyStatus::BudgetExceeded) << result.message;
    EXPECT_NE(result.message.find("budget"), std::string::npos);
    Chain stationary;
    bezierCurve(stationary, {V(-0.125, 0, 0), V(0.125, 0, 0),
                              V(-0.125, 0, 0), V(0.125, 0, 0)});
    const auto stopped = topology(stationary);
    EXPECT_EQ(stopped.status, OffsetChainTopologyStatus::BudgetExceeded) << stopped.message;
    EXPECT_EQ(stopped.message, "Topology certificate budget exhausted.");
}
}  // namespace
