// SPDX-License-Identifier: LGPL-2.1-or-later

#include <Python.h>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <Base/Reader.h>
#include <Base/Writer.h>
#include "LinkedOffsetExtension.h"

using namespace Sketcher;

TYPESYSTEM_SOURCE(Sketcher::LinkedOffsetExtension, Part::GeometryPersistenceExtension)

LinkedOffsetExtension::LinkedOffsetExtension()
    : uuid(boost::uuids::to_string(boost::uuids::random_generator()()))
{}

std::unique_ptr<Part::GeometryExtension> LinkedOffsetExtension::copy() const
{
    auto result = std::make_unique<LinkedOffsetExtension>();
    copyAttributes(result.get());
    result->uuid = uuid;
    result->membership = membership;
    result->blockToken = blockToken;
    result->invalidatedUuid = invalidatedUuid;
    result->version = version;
    result->record = record;
    return result;
}

PyObject* LinkedOffsetExtension::getPyObject()
{
    Py_INCREF(Py_None);
    return Py_None;
}

void LinkedOffsetExtension::saveAttributes(Base::Writer& writer) const
{
    Part::GeometryPersistenceExtension::saveAttributes(writer);
    const auto encode = Base::Persistence::encodeAttribute;
    writer.Stream() << "\" uuid=\"" << encode(uuid) << "\" membership=\"" << encode(membership)
                    << "\" invalidatedUuid=\"" << encode(invalidatedUuid)
                    << "\" blockToken=\"" << encode(blockToken)
                    << "\" version=\"" << encode(version) << "\" record=\"" << encode(record);
}

void LinkedOffsetExtension::restoreAttributes(Base::XMLReader& reader)
{
    Part::GeometryPersistenceExtension::restoreAttributes(reader);
    version = reader.hasAttribute("version") ? reader.getAttribute<const char*>("version") : "";
    uuid = reader.hasAttribute("uuid") ? reader.getAttribute<const char*>("uuid") : "";
    membership = reader.hasAttribute("membership") ? reader.getAttribute<const char*>("membership")
                                                   : "";
    invalidatedUuid = reader.hasAttribute("invalidatedUuid")
        ? reader.getAttribute<const char*>("invalidatedUuid") : "";
    blockToken = reader.hasAttribute("blockToken") ? reader.getAttribute<const char*>("blockToken")
                                                  : "";
    record = reader.hasAttribute("record") ? reader.getAttribute<const char*>("record") : "";
}
