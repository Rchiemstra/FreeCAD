// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <Mod/Part/App/Geometry.h>
#include <Mod/Sketcher/App/Constraint.h>
#include <Mod/Sketcher/App/GeoList.h>

namespace SketcherGui::PresentationInputs
{
// These queries deliberately accept captured inputs rather than a live SketchObject.
inline int groupHandle(const std::vector<Sketcher::Constraint*>& constraints, int geoId)
{
    if (geoId >= 0) {
        for (const auto* constraint : constraints) {
            if (constraint && (constraint->Type == Sketcher::Group || constraint->Type == Sketcher::Text)
                && constraint->hasElement(0)) {
                for (int index = 1; constraint->hasElement(index); ++index) {
                    if (constraint->getGeoId(index) == geoId) {
                        return constraint->getGeoId(0);
                    }
                }
            }
        }
    }
    return geoId;
}

inline bool hasAvailableGeometry(const Sketcher::GeoListFacade& geometry,
                                 const Sketcher::Constraint& constraint)
{
    using namespace Sketcher;
    bool optionalSecond = true;
    bool optionalThird = true;
    switch (constraint.Type) {
        case Symmetric:
        case SnellsLaw:
            optionalThird = false;
            [[fallthrough]];
        case Coincident:
        case PointOnObject:
        case Parallel:
        case Perpendicular:
        case Tangent:
        case Equal:
        case InternalAlignment:
            optionalSecond = false;
            break;
        default:
            break;
    }
    const auto available = [&](int id, PointPos pos, bool optional) {
        if (id == GeoEnum::GeoUndef) {
            return optional && pos == PointPos::none;
        }
        const auto* geo = geometry.getGeometryFromGeoId(id);
        if (!geo) {
            return false;
        }
        if (pos == PointPos::none) {
            return true;
        }
        if (pos != PointPos::start && pos != PointPos::end && pos != PointPos::mid) {
            return false;
        }
        if (geo->is<Part::GeomPoint>()) {
            return true;
        }
        if (geo->is<Part::GeomLineSegment>() || geo->is<Part::GeomBSplineCurve>()) {
            return pos == PointPos::start || pos == PointPos::end;
        }
        if (geo->is<Part::GeomCircle>() || geo->is<Part::GeomEllipse>()) {
            return pos == PointPos::mid;
        }
        return geo->is<Part::GeomArcOfCircle>() || geo->is<Part::GeomArcOfEllipse>()
            || geo->is<Part::GeomArcOfHyperbola>() || geo->is<Part::GeomArcOfParabola>();
    };
    return available(constraint.First, constraint.FirstPos, false)
        && available(constraint.Second, constraint.SecondPos, optionalSecond)
        && available(constraint.Third, constraint.ThirdPos, optionalThird);
}
}  // namespace SketcherGui::PresentationInputs
