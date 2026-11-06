// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <memory>

namespace App
{
class DocumentObject;
class Property;
class PropertyLinkSubList;
} // namespace App
namespace Assembly
{
std::unique_ptr<App::Property> prepareSketchReferenceIndices(App::DocumentObject& owner,
                                                             const App::PropertyLinkSubList& links,
                                                             bool& supported);
}
