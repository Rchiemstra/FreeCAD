// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DetachedDocumentArchive.h"
#include "DocumentObject.h"

namespace App
{

DetachedDocumentArchive::DetachedDocumentArchive() = default;
DetachedDocumentArchive::~DetachedDocumentArchive() = default;

bool DetachedDocumentArchive::captureClosure(const DocumentObject& target)
{
    _targetName = target.getNameInDocument();
    _captured = true;
    return true;
}

void DetachedDocumentArchive::write(GeometryArchiveWriter& writer) const
{
    writer.writeString("targetName", _targetName);
    if (!_serializedData.empty()) {
        writer.writeBytes("closureData", _serializedData.data(), _serializedData.size());
    }
}

bool DetachedDocumentArchive::read(const GeometryArchiveReader& reader)
{
    _targetName = reader.readString("targetName");
    reader.readBytes("closureData", _serializedData);
    _captured = !_targetName.empty();
    return _captured;
}

} // namespace App
