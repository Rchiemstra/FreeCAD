// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionTransform.h"

#include <cmath>

namespace Inspection::Photo
{

AffineTransform2d::AffineTransform2d()
    : values_ {1.0, 0.0, 0.0, 1.0, 0.0, 0.0}
{}

AffineTransform2d::AffineTransform2d(const std::array<double, 6>& coefficients)
    : values_(coefficients)
{}

AffineTransform2d AffineTransform2d::identity()
{
    return {};
}

const std::array<double, 6>& AffineTransform2d::coefficients() const
{
    return values_;
}

Point2d AffineTransform2d::apply(const Point2d& source) const
{
    return {
        values_[0] * source.x + values_[1] * source.y + values_[4],
        values_[2] * source.x + values_[3] * source.y + values_[5],
    };
}

double AffineTransform2d::determinant() const
{
    return values_[0] * values_[3] - values_[1] * values_[2];
}

bool AffineTransform2d::isFinite() const
{
    for (const double value : values_) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

std::optional<AffineTransform2d> AffineTransform2d::inverse(const double minimumAbsDeterminant) const
{
    if (!isFinite() || !std::isfinite(minimumAbsDeterminant) || minimumAbsDeterminant < 0.0) {
        return std::nullopt;
    }

    const double det = determinant();
    if (!std::isfinite(det) || std::abs(det) <= minimumAbsDeterminant) {
        return std::nullopt;
    }

    const double inverseDet = 1.0 / det;
    const double i00 = values_[3] * inverseDet;
    const double i01 = -values_[1] * inverseDet;
    const double i10 = -values_[2] * inverseDet;
    const double i11 = values_[0] * inverseDet;
    const double itx = -(i00 * values_[4] + i01 * values_[5]);
    const double ity = -(i10 * values_[4] + i11 * values_[5]);

    AffineTransform2d result({i00, i01, i10, i11, itx, ity});
    if (!result.isFinite()) {
        return std::nullopt;
    }
    return result;
}

std::optional<AffineTransform2d> printerCommandFromPhysical(const AffineTransform2d& physicalFromCommand)
{
    return physicalFromCommand.inverse();
}

}  // namespace Inspection::Photo
