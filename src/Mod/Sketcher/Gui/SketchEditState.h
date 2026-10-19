// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <functional>
#include <sstream>
#include <string_view>

#include <Base/Tools.h>
#include <Mod/Sketcher/App/SketchObject.h>

namespace SketcherGui
{

// Like upstream #32277, compare deterministic dumpToStream() snapshots. Keep the
// Cancel backup intact, and ignore the property touches caused by editing.
inline bool sketchEditStateUnchanged(
    Sketcher::SketchObject& sketch, const std::stringstream& backup)
{
    const auto hashBeforeEditing = std::hash<std::string_view>{}(backup.view());
    const bool constraintsTouched = sketch.Constraints.isTouched();
    const bool fullyConstrainedTouched = sketch.FullyConstrained.isTouched();
    const bool geometryTouched = sketch.Geometry.isTouched();
    const bool visibilityTouched = sketch.Visibility.isTouched();
    Base::ScopeGuard restoreTouched([&]() {
        // touch() also emits onChanged and would dirty the fork's authoritative
        // file-change state. Restore only the status, including on dump failure.
        sketch.Constraints.setStatus(App::Property::Touched, constraintsTouched);
        sketch.FullyConstrained.setStatus(App::Property::Touched, fullyConstrainedTouched);
        sketch.Geometry.setStatus(App::Property::Touched, geometryTouched);
        sketch.Visibility.setStatus(App::Property::Touched, visibilityTouched);
    });
    sketch.Constraints.purgeTouched();
    sketch.FullyConstrained.purgeTouched();
    sketch.Geometry.purgeTouched();
    sketch.Visibility.purgeTouched();

    std::stringstream current;
    sketch.dumpToStream(current, 0);
    return std::hash<std::string_view>{}(current.view()) == hashBeforeEditing;
}

}  // namespace SketcherGui
