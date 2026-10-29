// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <array>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <Base/Tools.h>
#include <Mod/Sketcher/App/SketchObject.h>

namespace SketcherGui
{

// Keep Cancel's full backup separate from the normalized comparison and from
// recompute requests, which are not all represented in persistence.
class SketchEditState
{
public:
    void capture(Sketcher::SketchObject& sketch)
    {
        placement = sketch.Placement.getValue();
        touched = sketch.testStatus(App::ObjectStatus::Touch);
        enforced = sketch.testStatus(App::ObjectStatus::Enforce);
        explicitlyTouched = false;
        persistentTouches.clear();
        touchedProperties = sketch.getTouchedProperties();
        entryTouchedProperties.clear();
        std::vector<App::Property*> properties;
        sketch.getPropertyList(properties);
        for (const auto* property : properties) {
            if (property->isTouched()) {
                entryTouchedProperties.insert(property->getName());
            }
        }
        for (std::size_t i = 0; i < temporaryProperties.size(); ++i) {
            propertyTouched[i] = sketch.getPropertyByName(temporaryProperties[i])->isTouched();
        }
        snapshot = normalizedSnapshot(sketch);
    }

    void restorePlacement(Sketcher::SketchObject& sketch) const
    {
        // XML's axis/angle rotation round trip can change quaternion bits.
        sketch.Placement.setValue(placement);
    }

    void noteExplicitTouch() { explicitlyTouched = true; }

    void notePersistentChange(const App::Property& property)
    {
        if (property.isTouched() && !property.testStatus(App::Property::NoModify)
            && !property.testStatus(App::Property::Transient)
            && !property.testStatus(App::Property::PropTransient)
            && !property.testStatus(App::Property::PropNoPersist)) {
            persistentTouches.insert(property.getName());
        }
    }

    bool unchanged(Sketcher::SketchObject& sketch) const
    {
        return normalizedSnapshot(sketch) == snapshot;
    }

    void restoreRecomputeState(Sketcher::SketchObject& sketch, bool cancelled = false) const
    {
        if (cancelled) {
            std::vector<App::Property*> properties;
            sketch.getPropertyList(properties);
            for (auto* property : properties) {
                property->setStatus(App::Property::Touched,
                                    entryTouchedProperties.contains(property->getName()));
            }
            sketch.restoreTouchedProperties(touchedProperties);
            sketch.setStatus(App::ObjectStatus::Touch, touched);
            sketch.setStatus(App::ObjectStatus::Enforce, enforced);
            return;
        }
        // Only these properties are touched by edit initialization. Retain
        // other touches, entry requests, and explicit touch() during editing.
        auto properties = sketch.getTouchedProperties();
        for (std::size_t i = 0; i < temporaryProperties.size(); ++i) {
            sketch.getPropertyByName(temporaryProperties[i])->setStatus(
                App::Property::Touched,
                propertyTouched[i] || persistentTouches.contains(temporaryProperties[i]));
            if (!touchedProperties.contains(temporaryProperties[i])) {
                properties.erase(temporaryProperties[i]);
            }
        }
        properties.insert(touchedProperties.begin(), touchedProperties.end());
        properties.insert(persistentTouches.begin(), persistentTouches.end());
        sketch.restoreTouchedProperties(properties);
        const bool enforce = enforced
            || ((explicitlyTouched || !persistentTouches.empty())
                && sketch.testStatus(App::ObjectStatus::Enforce));
        sketch.setStatus(App::ObjectStatus::Enforce, enforce);
        sketch.setStatus(App::ObjectStatus::Touch,
                         touched || explicitlyTouched || enforce || !properties.empty());
    }

private:
    static std::string normalizedSnapshot(Sketcher::SketchObject& sketch)
    {
        std::array<bool, 4> status;
        for (std::size_t i = 0; i < temporaryProperties.size(); ++i) {
            status[i] = sketch.getPropertyByName(temporaryProperties[i])->isTouched();
        }
        Base::ScopeGuard restore([&]() {
            for (std::size_t i = 0; i < temporaryProperties.size(); ++i) {
                sketch.getPropertyByName(temporaryProperties[i])->setStatus(
                    App::Property::Touched, status[i]);
            }
        });
        for (const auto* name : temporaryProperties) {
            sketch.getPropertyByName(name)->purgeTouched();
        }
        std::stringstream stream;
        sketch.dumpToStream(stream, 0);
        return stream.str();
    }

    static constexpr std::array<const char*, 4> temporaryProperties {
        "Constraints", "FullyConstrained", "Geometry", "Visibility"};
    std::string snapshot;
    Base::Placement placement;
    std::unordered_set<std::string> touchedProperties;
    std::unordered_set<std::string> entryTouchedProperties;
    std::unordered_set<std::string> persistentTouches;
    std::array<bool, 4> propertyTouched {};
    bool touched {false};
    bool enforced {false};
    bool explicitlyTouched {false};
};

}  // namespace SketcherGui
