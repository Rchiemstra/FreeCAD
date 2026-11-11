// SPDX-License-Identifier: LGPL-2.1-or-later
#include <map>
#include <string>
#include <App/DocumentObject.h>
#include <App/ElementNamingUtils.h>
#include <App/PropertyLinks.h>
#include <Mod/Part/App/DatumFeature.h>
#include <Mod/Part/App/PropertyGeometryList.h>
#include <Mod/Sketcher/App/ExternalGeometryFacade.h>
#include "ExtractSketchReferences.h"

namespace Assembly
{
std::unique_ptr<App::Property> prepareSketchReferenceIndices(App::DocumentObject& owner,
                                                             const App::PropertyLinkSubList& links,
                                                             bool& supported)
{
    auto property =
        dynamic_cast<Part::PropertyGeometryList*>(owner.getPropertyByName("ExternalGeo"));
    if (!property) {
        return {};
    }
    std::map<std::string, int> indices;
    const auto& objects = links.getValues();
    const auto& subs = links.getSubValues();
    const auto& shadows = links.getShadowSubs();
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto& sub = shadows[i].newName.empty() ? subs[i] : shadows[i].newName;
        std::string key = std::string(objects[i]->getNameInDocument()) + '.';
        indices[key + Data::oldElementName(sub.c_str())] = static_cast<int>(i);
        if (!objects[i]->isDerivedFrom<Part::Datum>()) {
            key += Data::newElementName(sub.c_str());
        }
        indices[key] = static_cast<int>(i);
    }
    std::unique_ptr<App::Property> copy(property->Copy());
    auto geometries = static_cast<Part::PropertyGeometryList*>(copy.get());
    // Follow SketchObject::Save(export): retain geometry IDs and flags while
    // rebinding its per-geometry topology keys by external-reference index.
    // onExternalGeometryChanged() consumes these indices after the link paste.
    for (auto geometry : geometries->getValues()) {
        auto facade = Sketcher::ExternalGeometryFacade::getFacade(geometry);
        if (facade->getRef().empty()) {
            continue;
        }
        auto found = indices.find(facade->getRef());
        if (found == indices.end()) {
            supported = false;
            return {};
        }
        facade->setRefIndex(found->second);
    }
    return copy;
}
} // namespace Assembly
