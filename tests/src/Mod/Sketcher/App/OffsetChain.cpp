// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include <Geom_Circle.hxx>
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
    line(source, V(10, 0, 0), V(10, 10, 0));
    line(source, V(10, 10, 0), V(3.7123, 10, 0));
    line(source, V(3.7123, 10, 0), V(3.7123, -0.999, 0));
    line(candidate, V(0, -1, 0), V(10, -1, 0));
    EXPECT_NE(certify(source, candidate).status, OffsetChainDistanceStatus::Success);
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
}  // namespace
