// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Part/App/GeometryExtension.h>
#include <Mod/Sketcher/SketcherGlobal.h>

namespace Sketcher
{

// Identity is sketch-local. Snapshot and whole-sketch copies retain UUIDs.
// Geometry insertion explicitly remaps identities and complete relation groups.
class SketcherExport LinkedOffsetExtension: public Part::GeometryPersistenceExtension
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();

public:
    LinkedOffsetExtension();
    std::unique_ptr<Part::GeometryExtension> copy() const override;
    PyObject* getPyObject() override;

    std::string uuid;
    std::string membership;
    std::string version = "1";
    // Only the designated output owns the relation. Unknown versions remain opaque
    // through Save/Restore/copy and are ignored by the inspection API.
    std::string record;

protected:
    void restoreAttributes(Base::XMLReader& reader) override;
    void saveAttributes(Base::Writer& writer) const override;
};

}  // namespace Sketcher
