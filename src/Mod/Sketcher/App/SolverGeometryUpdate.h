// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <sstream>
#include <utility>
#include <Base/Tools.h>
#include <Mod/Part/App/PropertyGeometryList.h>

namespace Sketcher
{
inline void updateSolverGeometry(Part::PropertyGeometryList& geometry,
                                 Part::PropertyGeometryList&& solved)
{
    std::stringstream before, after;
    geometry.dumpToStream(before, 0);
    solved.dumpToStream(after, 0);
    // Runtime solver extensions are not saved. Equal persistence means only
    // those caches are changing; keep callbacks on all other properties tracked.
    const bool noModify = geometry.testStatus(App::Property::NoModify);
    Base::ScopeGuard restore([&]() {
        geometry.setStatus(App::Property::NoModify, noModify);
    });
    if (before.view() == after.view()) {
        geometry.setStatus(App::Property::NoModify, true);
    }
    geometry.moveValues(std::move(solved));
}
}  // namespace Sketcher
