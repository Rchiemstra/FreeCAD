// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "GeometryJob.h"
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace App
{

class Document;
class DocumentObject;

class AppExport DetachedDocumentArchive
{
public:
    DetachedDocumentArchive();
    ~DetachedDocumentArchive();

    bool captureClosure(const DocumentObject& target);
    void write(GeometryArchiveWriter& writer) const;
    bool read(const GeometryArchiveReader& reader);

    bool isCaptured() const { return _captured; }
    const std::string& targetObjectName() const { return _targetName; }

private:
    std::string _targetName;
    std::vector<uint8_t> _serializedData;
    bool _captured {false};
};

} // namespace App
