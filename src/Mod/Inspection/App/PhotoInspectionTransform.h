// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <optional>

#include <Mod/Inspection/InspectionGlobal.h>

namespace Inspection::Photo
{

struct InspectionExport Point2d
{
    double x {0.0};
    double y {0.0};
};

// A named-direction 2D affine transform:
//
//     target = linear * source + translation
//
// Coefficients are [m00, m01, m10, m11, tx, ty].
class InspectionExport AffineTransform2d
{
public:
    AffineTransform2d();
    explicit AffineTransform2d(const std::array<double, 6>& coefficients);

    static AffineTransform2d identity();

    const std::array<double, 6>& coefficients() const;
    Point2d apply(const Point2d& source) const;
    double determinant() const;
    bool isFinite() const;
    std::optional<AffineTransform2d> inverse(double minimumAbsDeterminant = 1.0e-12) const;

private:
    std::array<double, 6> values_;
};

// The printer profile maps print-command coordinates to measured physical
// sheet coordinates. Pre-compensation is its inverse.
InspectionExport std::optional<AffineTransform2d> printerCommandFromPhysical(
    const AffineTransform2d& physicalFromCommand
);

}  // namespace Inspection::Photo
