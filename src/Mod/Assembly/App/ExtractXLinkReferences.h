// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <memory>
#include <vector>
namespace App
{
class DocumentObject;
class Property;
class PropertyXLinkSubList;
} // namespace App
namespace Assembly
{
struct ReferenceReplacement;
std::unique_ptr<App::Property>
prepareXLinkReferences(App::PropertyXLinkSubList& property,
                       const std::vector<ReferenceReplacement>& replacements,
                       std::vector<App::DocumentObject*>& referenced);
} // namespace Assembly
