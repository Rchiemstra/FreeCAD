// SPDX-License-Identifier: LGPL-2.1-or-later

#include "BSplineOffset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <Geom_BSplineCurve.hxx>
#include <Geom_OffsetCurve.hxx>
#include <GeomConvert_ApproxCurve.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>

namespace Sketcher
{
namespace
{
using Status = BSplineOffsetStatus;

struct Failure
{
    Status status;
    const char* message;
};

// Outward-rounded interval arithmetic. Subdividing homogeneous Bernstein poles
// gives enclosures on rational position and its first two derivatives, including
// between samples. Unresolved enclosures are rejected, never treated as regular.
struct Interval
{
    double lo = 0.0;
    double hi = 0.0;
    Interval() = default;
    Interval(double value)
        : lo(value)
        , hi(value)
    {}
    Interval(double low, double high)
        : lo(low)
        , hi(high)
    {}
};

constexpr double infinity = std::numeric_limits<double>::infinity();

double down(double x)
{
    return std::nextafter(x, -infinity);
}

double up(double x)
{
    return std::nextafter(x, infinity);
}

Interval operator+(Interval a, Interval b)
{
    return {down(a.lo + b.lo), up(a.hi + b.hi)};
}

Interval operator-(Interval a, Interval b)
{
    return {down(a.lo - b.hi), up(a.hi - b.lo)};
}

Interval operator*(Interval a, Interval b)
{
    const std::array<double, 4> p {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
    return {down(*std::min_element(p.begin(), p.end())), up(*std::max_element(p.begin(), p.end()))};
}

Interval operator/(Interval a, Interval b)
{
    if (b.lo <= 0.0 && b.hi >= 0.0) {
        throw Failure {Status::VerificationFailed, "An interval denominator could not be bounded."};
    }
    return a * Interval(down(1.0 / b.hi), up(1.0 / b.lo));
}

Interval square(Interval a)
{
    double low = a.lo <= 0.0 && a.hi >= 0.0 ? 0.0 : std::min(a.lo * a.lo, a.hi * a.hi);
    return {std::max(0.0, down(low)), up(std::max(a.lo * a.lo, a.hi * a.hi))};
}

using Vector = std::array<Interval, 3>;
using Pole = std::array<Interval, 4>;

Vector operator+(const Vector& a, const Vector& b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Vector operator-(const Vector& a, const Vector& b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vector operator*(const Vector& a, Interval b)
{
    return {a[0] * b, a[1] * b, a[2] * b};
}

Vector operator/(const Vector& a, Interval b)
{
    return {a[0] / b, a[1] / b, a[2] / b};
}

Interval dot(const Vector& a, const Vector& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vector cross(const Vector& a, const Vector& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Interval norm(const Vector& a)
{
    auto s = square(a[0]) + square(a[1]) + square(a[2]);
    return {std::max(0.0, down(std::sqrt(std::max(0.0, s.lo)))), up(std::sqrt(s.hi))};
}

struct Span
{
    double first;
    double last;
    std::vector<Pole> poles;
};

Span bezier(const Handle(Geom_BSplineCurve) & curve, double first, double last, const gp_Pnt& origin)
{
    // Evaluate the polar form directly from ORIGINAL homogeneous poles and knots.
    // Each Bezier pole is blossom(first, ..., first, last, ..., last). Interval
    // de Boor includes every subtraction, division and multiply in knot insertion;
    // no rounded OCCT conversion pole is ever treated as exact. The resulting
    // envelopes feed regularity, crossing AND correspondence-error bounds.
    std::vector<Interval> knots;
    for (int i = 1; i <= curve->NbKnots(); ++i) {
        knots.insert(knots.end(), curve->Multiplicity(i), Interval(curve->Knot(i)));
    }
    const int degree = curve->Degree();
    if (curve->IsPeriodic()) {
        // Extend ORIGINAL knots by one period and repeat original poles below.
        // Enclose the period arithmetic too, rather than treating rounded kernel
        // conversion poles or extended knots as exact input data.
        const auto base = knots;
        const auto period = Interval(curve->LastParameter()) - Interval(curve->FirstParameter());
        const int extra = degree + 1 - curve->Multiplicity(1);
        const int count = static_cast<int>(base.size());
        const int cycle = count - curve->Multiplicity(curve->NbKnots());
        const auto extended = [&](int index) {
            int turns = index / cycle;
            int pole = index % cycle;
            if (pole < 0) {
                pole += cycle;
                --turns;
            }
            return base[pole] + period * Interval(static_cast<double>(turns));
        };
        for (int i = 0; i < extra; ++i) {
            knots.insert(knots.begin() + i, extended(i - extra));
            knots.push_back(extended(count + i));
        }
    }
    const auto& flat = curve->KnotSequence();
    int k = degree;
    const int end = flat.Length() - degree - 1;
    while (k < end && !(flat(k + 1) <= first && last <= flat(k + 2) && flat(k + 1) < flat(k + 2))) {
        ++k;
    }
    if (k == end) {
        throw Failure {Status::VerificationFailed, "Could not isolate a spline knot span."};
    }
    std::vector<Pole> original;
    for (int i = k - degree; i <= k; ++i) {
        const int index = i % curve->NbPoles() + 1;
        auto p = curve->Pole(index);
        Interval w(curve->Weight(index));
        // A shared reference point for source AND fit avoids large translation
        // terms in rational derivative bounds. Subtraction is also enclosed;
        // all positions and comparisons remain in the same translated frame.
        original.push_back(
            {(Interval(p.X()) - Interval(origin.X())) * w,
             (Interval(p.Y()) - Interval(origin.Y())) * w,
             (Interval(p.Z()) - Interval(origin.Z())) * w,
             w}
        );
    }
    Span span {first, last, {}};
    for (int pole = 0; pole <= degree; ++pole) {
        auto work = original;
        for (int r = 1; r <= degree; ++r) {
            const Interval parameter(r <= degree - pole ? first : last);
            for (int j = degree; j >= r; --j) {
                const int i = k - degree + j;
                const Interval alpha = (parameter - knots[i])
                    / (knots[i + degree - r + 1] - knots[i]);
                for (size_t c = 0; c < 4; ++c) {
                    work[j][c] = (Interval(1.0) - alpha) * work[j - 1][c] + alpha * work[j][c];
                }
            }
        }
        span.poles.push_back(work[degree]);
    }
    return span;
}

std::pair<Span, Span> split(const Span& span)
{
    const double middle = span.first + (span.last - span.first) / 2.0;
    if (middle == span.first || middle == span.last) {
        throw Failure {Status::VerificationFailed, "Parameter resolution exhausted during validation."};
    }
    Span left {span.first, middle, {}}, right {middle, span.last, {}};
    // The stored midpoint can round away from the exact half parameter. Enclose
    // its actual fraction so child poles and child parameter intervals agree.
    const Interval fraction = (Interval(middle) - Interval(span.first))
        / (Interval(span.last) - Interval(span.first));
    auto poles = span.poles;
    while (!poles.empty()) {
        left.poles.push_back(poles.front());
        right.poles.push_back(poles.back());
        for (size_t i = 0; i + 1 < poles.size(); ++i) {
            for (size_t c = 0; c < 4; ++c) {
                poles[i][c] = poles[i][c] * (Interval(1.0) - fraction) + poles[i + 1][c] * fraction;
            }
        }
        poles.pop_back();
    }
    std::reverse(right.poles.begin(), right.poles.end());
    return {std::move(left), std::move(right)};
}

Pole hull(const std::vector<Pole>& poles)
{
    Pole result = poles.front();
    for (const auto& pole : poles) {
        for (size_t c = 0; c < 4; ++c) {
            result[c].lo = std::min(result[c].lo, pole[c].lo);
            result[c].hi = std::max(result[c].hi, pole[c].hi);
        }
    }
    return result;
}

Pole midpoint(std::vector<Pole> poles)
{
    while (poles.size() > 1) {
        for (size_t i = 0; i + 1 < poles.size(); ++i) {
            for (size_t c = 0; c < 4; ++c) {
                poles[i][c] = (poles[i][c] + poles[i + 1][c]) * Interval(0.5);
            }
        }
        poles.pop_back();
    }
    return poles.front();
}

std::vector<Pole> derivative(const std::vector<Pole>& poles, Interval length)
{
    if (poles.size() == 1) {
        return {Pole {}};
    }
    std::vector<Pole> result(poles.size() - 1);
    Interval scale = Interval(static_cast<double>(poles.size() - 1)) / length;
    for (size_t i = 0; i < result.size(); ++i) {
        for (size_t c = 0; c < 4; ++c) {
            result[i][c] = (poles[i + 1][c] - poles[i][c]) * scale;
        }
    }
    return result;
}

Vector xyz(const Pole& p)
{
    return {p[0], p[1], p[2]};
}

struct Bounds
{
    Vector position;
    Vector tangent;
    Vector acceleration;
};

Bounds bounds(const Span& span, bool atMidpoint = false)
{
    const auto enclose = [atMidpoint](const std::vector<Pole>& poles) {
        return atMidpoint ? midpoint(poles) : hull(poles);
    };
    auto h = enclose(span.poles);
    const Interval length = Interval(span.last) - Interval(span.first);
    auto d = derivative(span.poles, length);
    auto h1 = enclose(d);
    auto h2 = enclose(derivative(d, length));
    Vector p = xyz(h) / h[3];
    Vector v = (xyz(h1) - p * h1[3]) / h[3];
    Vector a = (xyz(h2) - v * (Interval(2.0) * h1[3]) - p * h2[3]) / h[3];
    return {p, v, a};
}

struct Validator
{
    const BSplineOffsetOptions& options;
    BSplineOffsetComplexity& complexity;
    Vector normal;
    int comparisons = 0;
    double closedFirst = infinity;
    double closedLast = -infinity;

    void consume()
    {
        if (++complexity.validationSpans > options.maxValidationSpans) {
            throw Failure {
                Status::ComplexityExceeded,
                "Adaptive validation budget exceeded; simplify the source or relax tolerance."
            };
        }
    }

    void compare()
    {
        if (++comparisons > options.maxValidationSpans * 64) {
            throw Failure {Status::ComplexityExceeded, "Self-intersection validation budget exceeded."};
        }
    }

    Bounds offsetBounds(const Span& span, bool offset, bool& regular, bool atMidpoint = false)
    {
        consume();
        auto b = bounds(span, atMidpoint);
        const auto transverse = cross(b.tangent, normal);
        const auto speed = norm(transverse);
        regular = speed.lo > Precision::Confusion();
        if (!regular || !offset) {
            return b;
        }
        // For positive tangent x normal distance, offset derivative is
        // (1 + d * (C' x C'').normal / |C'|^3) C'. Require the factor
        // strictly positive on the WHOLE interval, including curvature extrema.
        auto factor = Interval(1.0)
            + Interval(options.distance) * dot(cross(b.tangent, b.acceleration), normal)
                / (speed * speed * speed);
        regular = factor.lo > 1e-8;
        b.position = b.position + transverse * Interval(options.distance) / speed;
        // Use the full derivative, also accounting for the roundoff allowed by
        // the planarity check. The planar factor above is the singularity veto.
        const auto change = cross(b.acceleration, normal);
        b.tangent = b.tangent + change * Interval(options.distance) / speed
            - transverse
                * (Interval(options.distance) * dot(transverse, change) / (speed * speed * speed));
        return b;
    }

    struct Region
    {
        Span span;
        Bounds enclosure;
    };

    void regularSpans(const Span& span, std::vector<Region>& regions, bool offset, int depth = 0)
    {
        bool regular;
        auto b = offsetBounds(span, offset, regular);
        // Also make each local piece injective: one derivative component must
        // have a strict sign. This is a proof on an interval, not a chord test.
        bool monotone = false;
        for (const auto& component : b.tangent) {
            monotone |= component.lo > 0.0 || component.hi < 0.0;
        }
        if (regular && monotone) {
            regions.push_back({span, b});
            return;
        }
        if (depth >= options.maxRefinementDepth) {
            auto speed = norm(bounds(span).tangent);
            throw Failure {
                speed.lo <= Precision::Confusion() ? Status::ZeroTangent : Status::SingularOffset,
                "Zero tangent, singular offset or unresolved regularity; reduce the distance or "
                "simplify the spline."
            };
        }
        auto halves = split(span);
        regularSpans(halves.first, regions, offset, depth + 1);
        regularSpans(halves.second, regions, offset, depth + 1);
    }

    static bool disjoint(const Bounds& a, const Bounds& b)
    {
        for (size_t c = 0; c < 3; ++c) {
            if (a.position[c].hi < b.position[c].lo || b.position[c].hi < a.position[c].lo) {
                return true;
            }
        }
        return false;
    }

    bool commonDirection(const std::vector<Region>& regions, size_t first, size_t last)
    {
        for (size_t c = 0; c < 3; ++c) {
            bool positive = true, negative = true;
            for (size_t i = first; i <= last; ++i) {
                compare();
                positive &= regions[i].enclosure.tangent[c].lo > 0.0;
                negative &= regions[i].enclosure.tangent[c].hi < 0.0;
                if (!positive && !negative) {
                    break;
                }
            }
            if (positive || negative) {
                return true;
            }
        }
        return false;
    }

    void separate(const Region& a, const Region& b, bool offset, int depth = 0)
    {
        compare();
        if (disjoint(a.enclosure, b.enclosure)) {
            return;
        }
        const bool seam = (a.span.first == closedFirst && b.span.last == closedLast)
            || (b.span.first == closedFirst && a.span.last == closedLast);
        if (a.span.last == b.span.first || b.span.last == a.span.first || seam) {
            // Adjacent spans necessarily share an endpoint. After refinement a
            // common strict tangent component proves injectivity across that
            // endpoint, including the seam. Otherwise box overlap would reject
            // regular arcs whose end tangents turn through 90 degrees.
            for (size_t c = 0; c < 3; ++c) {
                if ((a.enclosure.tangent[c].lo > 0.0 && b.enclosure.tangent[c].lo > 0.0)
                    || (a.enclosure.tangent[c].hi < 0.0 && b.enclosure.tangent[c].hi < 0.0)) {
                    return;
                }
            }
        }
        if (depth >= options.maxRefinementDepth) {
            throw Failure {
                Status::SelfIntersection,
                "Offset self-crossing or near-contact cannot be excluded; reduce the distance."
            };
        }
        // Bisect the larger parameter interval, retaining conservative boxes.
        if (a.span.last - a.span.first < b.span.last - b.span.first) {
            separate(b, a, offset, depth);
            return;
        }
        auto halves = split(a.span);
        bool leftRegular, rightRegular;
        auto left = offsetBounds(halves.first, offset, leftRegular);
        auto right = offsetBounds(halves.second, offset, rightRegular);
        if (!leftRegular || !rightRegular) {
            throw Failure {
                Status::VerificationFailed,
                "Regularity could not be bounded during self-intersection validation."
            };
        }
        separate({std::move(halves.first), left}, b, offset, depth + 1);
        separate({std::move(halves.second), right}, b, offset, depth + 1);
    }

    void simple(const std::vector<Region>& regions, bool offset)
    {
        for (size_t i = 0; i < regions.size(); ++i) {
            for (size_t j = i + 1; j < regions.size(); ++j) {
                compare();
                if (disjoint(regions[i].enclosure, regions[j].enclosure)
                    || commonDirection(regions, i, j)) {
                    continue;
                }
                separate(regions[i], regions[j], offset);
            }
        }
    }

    int orientation(std::vector<Span> spans)
    {
        // Bound the signed area integral, including the entire seam spans.
        // Only a strict sign is displayed; unresolved orientation fails closed.
        for (int depth = 0; depth <= options.maxRefinementDepth; ++depth) {
            Interval area;
            for (const auto& span : spans) {
                consume();
                const auto b = bounds(span);
                area = area
                    + dot(cross(b.position, b.tangent), normal)
                        * (Interval(span.last) - Interval(span.first));
            }
            if (area.lo > 0.0 || area.hi < 0.0) {
                return area.lo > 0.0 ? 1 : -1;
            }
            std::vector<Span> refined;
            for (const auto& span : spans) {
                auto halves = split(span);
                refined.push_back(std::move(halves.first));
                refined.push_back(std::move(halves.second));
            }
            spans = std::move(refined);
        }
        throw Failure {
            Status::VerificationFailed,
            "Closed spline orientation could not be bounded."
        };
    }

    double error(
        const Span& source,
        const Span& fit,
        const Handle(Geom_OffsetCurve) & exact,
        const Handle(Geom_BSplineCurve) & output,
        double allowance,
        int depth = 0
    )
    {
        bool regular;
        auto a = offsetBounds(source, true, regular);
        auto b = bounds(fit);
        const double middle = source.first + (source.last - source.first) / 2.0;
        bool middleRegular;
        auto exactMiddle = offsetBounds(source, true, middleRegular, true);
        const double sample = std::max(
            norm(exactMiddle.position - bounds(fit, true).position).hi,
            up(exact->Value(middle).Distance(output->Value(middle)))
        );
        // Fundamental theorem of calculus bounds correspondence error at EVERY
        // parameter by midpoint error + half-span * sup |exact' - fitted'|.
        // Correspondence bounds both directed Hausdorff distances; no projection
        // minimum or sampling-density assumption is used.
        const double bound = (Interval(sample)
                              + (Interval(source.last) - Interval(source.first)) * Interval(0.5)
                                  * Interval(norm(a.tangent - b.tangent).hi))
                                 .hi;
        if (regular && middleRegular && std::isfinite(bound) && bound <= allowance) {
            return bound;
        }
        if (sample > allowance || depth >= options.maxRefinementDepth) {
            throw Failure {
                Status::VerificationFailed,
                "Independent offset error exceeds tolerance or could not be bounded; relax "
                "tolerance or increase fit limits."
            };
        }
        auto s = split(source);
        auto f = split(fit);
        const double left = error(s.first, f.first, exact, output, allowance, depth + 1);
        const double right = error(s.second, f.second, exact, output, allowance, depth + 1);
        return std::max(left, right);
    }
};

bool validOptions(const BSplineOffsetOptions& o)
{
    const auto& n = o.sketchNormal;
    return std::isfinite(o.distance) && std::isfinite(o.tolerance)
        && std::abs(o.distance) > 2.0 * o.tolerance && o.tolerance >= Precision::Confusion()
        && std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z)
        && std::isfinite(n.Length()) && n.Length() > Precision::Confusion() && o.maxInputPoles >= 2
        && o.maxInputPoles <= 1024 && o.maxOutputPoles >= 2 && o.maxOutputPoles <= 4096
        && o.maxSegments >= 1 && o.maxSegments <= 256 && o.maxDegree >= 3 && o.maxDegree <= 14
        && o.maxValidationSpans >= 16 && o.maxValidationSpans <= 65536 && o.maxRefinementDepth >= 1
        && o.maxRefinementDepth <= 24;
}

std::vector<double> knots(const Handle(Geom_BSplineCurve) & curve)
{
    std::vector<double> result {curve->FirstParameter()};
    for (int i = 1; i <= curve->NbKnots(); ++i) {
        if (curve->Knot(i) > curve->FirstParameter() && curve->Knot(i) < curve->LastParameter()) {
            result.push_back(curve->Knot(i));
        }
    }
    result.push_back(curve->LastParameter());
    return result;
}

// Use one-sided derivatives: periodic evaluation must not hide a G0 seam by
// wrapping the last parameter onto the first span.
gp_Vec seamTangent(const Handle(Geom_BSplineCurve) & curve)
{
    gp_Pnt p, q;
    gp_Vec a, b;
    const int first = curve->FirstUKnotIndex(), last = curve->LastUKnotIndex();
    curve->LocalD1(curve->FirstParameter(), first, first + 1, p, a);
    curve->LocalD1(curve->LastParameter(), last - 1, last, q, b);
    if (a.Magnitude() <= Precision::Confusion() || b.Magnitude() <= Precision::Confusion()) {
        throw Failure {
            Status::ZeroTangent,
            "A zero tangent prevents checking the closed spline seam."
        };
    }
    if (a.Dot(b) <= 0.0 || a.Crossed(b).Magnitude() > 1e-7 * a.Magnitude() * b.Magnitude()) {
        throw Failure {
            Status::SeamCorner,
            "The closed B-spline has a G0-only corner at the seam; a G1 tangent is required."
        };
    }
    return a.Normalized();
}

void closeFit(
    const Handle(Geom_BSplineCurve) & output,
    const Handle(Geom_OffsetCurve) & exact,
    const gp_Vec& tangent,
    bool periodic
)
{
    // ApproxCurve supplies a clamped fit. Make the endpoint and its two handles
    // share an exact point and tangent. ALL resulting pole changes, including
    // SetPeriodic, are subsequently covered by independent interval validation.
    if (output->IsPeriodic()) {
        output->Segment(output->FirstParameter(), output->LastParameter());
    }
    const int n = output->NbPoles();
    if (n < 4 || output->Multiplicity(1) != output->Degree() + 1
        || output->Multiplicity(output->NbKnots()) != output->Degree() + 1) {
        throw Failure {Status::ApproximationFailed, "Unable to close the fitted B-spline seam."};
    }
    const auto point = exact->Value(output->FirstParameter());
    const double firstHandle = gp_Vec(output->Pole(1), output->Pole(2)).Dot(tangent);
    const double lastHandle = gp_Vec(output->Pole(n - 1), output->Pole(n)).Dot(tangent);
    if (firstHandle <= 0.0 || lastHandle <= 0.0) {
        throw Failure {Status::VerificationFailed, "The fitted seam tangent reversed direction."};
    }
    output->SetPole(1, point);
    output->SetPole(n, point);
    output->SetPole(2, point.Translated(tangent * firstHandle));
    output->SetPole(n - 1, point.Translated(tangent * -lastHandle));
    if (periodic) {
        output->SetPeriodic();
    }
    seamTangent(output);
}

}  // namespace

BSplineOffsetStatus checkBSplineOffsetInput(
    const Part::GeomBSplineCurve& source,
    const BSplineOffsetOptions& options
)
{
    if (!validOptions(options)) {
        return Status::InvalidOptions;
    }
    const auto curve = Handle(Geom_BSplineCurve)::DownCast(source.handle());
    if (curve->NbPoles() > options.maxInputPoles || curve->NbKnots() - 1 > options.maxSegments
        || curve->Degree() > 14) {
        return Status::ComplexityExceeded;
    }
    return Status::Success;
}

BSplineOffsetResult makeBSplineOffset(
    const Part::GeomBSplineCurve& source,
    const BSplineOffsetOptions& options
)
{
    BSplineOffsetResult result;
    try {
        result.status = checkBSplineOffsetInput(source, options);
        if (result.status != Status::Success) {
            throw Failure {result.status, "Invalid offset options or input complexity limits exceeded."};
        }
        auto original = Handle(Geom_BSplineCurve)::DownCast(source.handle());
        result.complexity.inputPoles = original->NbPoles();
        const bool closed = original->IsPeriodic() || original->IsClosed();
        if (!original->IsCN(2)) {
            throw Failure {Status::UnsupportedCurve, "Select one open, nonperiodic, C2 B-spline."};
        }
        gp_Dir direction(options.sketchNormal.x, options.sketchNormal.y, options.sketchNormal.z);
        gp_Vec normal(direction);
        const auto origin = original->Pole(1);
        for (int i = 1; i <= original->NbPoles(); ++i) {
            const auto p = original->Pole(i);
            if (!std::isfinite(p.X()) || !std::isfinite(p.Y()) || !std::isfinite(p.Z())
                || !std::isfinite(original->Weight(i)) || original->Weight(i) <= 0.0
                || std::abs(gp_Vec(origin, p).Dot(normal)) > Precision::Confusion()) {
                throw Failure {
                    Status::UnsupportedCurve,
                    "Spline must be finite, planar in the sketch plane and have positive weights."
                };
            }
        }
        gp_Vec tangent;
        if (closed) {
            tangent = seamTangent(original);
        }
        auto copy = Handle(Geom_BSplineCurve)::DownCast(original->Copy());
        Validator validator {
            options,
            result.complexity,
            {Interval(direction.X()), Interval(direction.Y()), Interval(direction.Z())}
        };
        if (closed) {
            validator.closedFirst = original->FirstParameter();
            validator.closedLast = original->LastParameter();
        }
        auto sourceKnots = knots(original);
        std::vector<Validator::Region> regions;
        if (closed) {
            // Inside/outside is meaningful only for a simple SOURCE. Use the
            // same interval proof as for the offset, including the seam pair.
            for (size_t i = 1; i < sourceKnots.size(); ++i) {
                validator.regularSpans(
                    bezier(original, sourceKnots[i - 1], sourceKnots[i], origin),
                    regions,
                    false
                );
            }
            try {
                validator.simple(regions, false);
            }
            catch (const Failure& failure) {
                if (failure.status == Status::SelfIntersection) {
                    throw Failure {
                        Status::SourceSelfIntersection,
                        "Source spline crossing or near-contact cannot be excluded; "
                        "inside/outside is undefined."
                    };
                }
                throw;
            }
            std::vector<Span> spans;
            for (const auto& region : regions) {
                spans.push_back(region.span);
            }
            result.orientation = validator.orientation(std::move(spans));
            regions.clear();
        }
        for (size_t i = 1; i < sourceKnots.size(); ++i) {
            validator.regularSpans(
                bezier(original, sourceKnots[i - 1], sourceKnots[i], origin),
                regions,
                true
            );
        }
        validator.simple(regions, true);
        Handle(Geom_OffsetCurve) exact = new Geom_OffsetCurve(copy, options.distance, direction);
        GeomConvert_ApproxCurve approximation(
            exact,
            options.tolerance / 4.0,
            GeomAbs_C1,
            options.maxSegments,
            options.maxDegree
        );
        if (!approximation.IsDone() || approximation.Curve().IsNull()
            || !std::isfinite(approximation.MaxError()) || approximation.MaxError() < 0.0) {
            throw Failure {
                Status::ApproximationFailed,
                "Kernel could not fit the offset within tolerance and complexity limits."
            };
        }
        auto output = approximation.Curve();
        if (closed) {
            closeFit(output, exact, tangent, original->IsPeriodic());
        }
        result.complexity.outputPoles = output->NbPoles();
        result.complexity.segments = output->NbKnots() - 1;
        result.complexity.degree = output->Degree();
        if (output->NbPoles() > options.maxOutputPoles
            || output->NbKnots() - 1 > options.maxSegments || output->Degree() > options.maxDegree) {
            throw Failure {Status::ComplexityExceeded, "Fitted spline exceeds output complexity limits."};
        }
        auto outputKnots = knots(output);
        regions.clear();
        for (size_t i = 1; i < outputKnots.size(); ++i) {
            validator.regularSpans(
                bezier(output, outputKnots[i - 1], outputKnots[i], origin),
                regions,
                false
            );
        }
        validator.simple(regions, false);
        if (output->FirstParameter() != original->FirstParameter()
            || output->LastParameter() != original->LastParameter()) {
            throw Failure {
                Status::VerificationFailed,
                "Kernel fit did not preserve the parameter interval."
            };
        }
        sourceKnots.insert(sourceKnots.end(), outputKnots.begin(), outputKnots.end());
        std::sort(sourceKnots.begin(), sourceKnots.end());
        sourceKnots.erase(std::unique(sourceKnots.begin(), sourceKnots.end()), sourceKnots.end());
        const double allowance = options.tolerance - approximation.MaxError();
        double independentError = 0.0;
        for (size_t i = 1; i < sourceKnots.size(); ++i) {
            const double first = sourceKnots[i - 1], last = sourceKnots[i];
            independentError = std::max(
                independentError,
                validator.error(
                    bezier(original, first, last, origin),
                    bezier(output, first, last, origin),
                    exact,
                    output,
                    allowance
                )
            );
        }
        result.achievedMaxError = up(approximation.MaxError() + independentError);
        if (result.achievedMaxError > options.tolerance) {
            throw Failure {
                Status::VerificationFailed,
                "Total offset error exceeds the requested tolerance."
            };
        }
        result.curve = std::make_unique<Part::GeomBSplineCurve>(output);
        result.status = Status::Success;
        result.message = "Independent approximate B-spline offset verified within tolerance.";
    }
    catch (const Failure& failure) {
        result.status = failure.status;
        result.message = failure.message;
    }
    catch (const Standard_Failure& failure) {
        result.status = Status::ApproximationFailed;
        const char* message = failure.GetMessageString();
        result.message = std::string("Offset kernel failure: ")
            + (message ? message : "unknown error");
    }
    return result;
}

}  // namespace Sketcher
