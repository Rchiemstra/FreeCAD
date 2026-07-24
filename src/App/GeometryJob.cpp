// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryJob.h"
#include "Document.h"

namespace App
{

GeometryCommitScope::GeometryCommitScope(Document* doc, DocumentObject* obj)
    : _doc(doc), _obj(obj)
{
    if (_doc) {
        _doc->setCommittingGeometryJob(true);
    }
}

GeometryCommitScope::~GeometryCommitScope()
{
    if (_doc) {
        _doc->setCommittingGeometryJob(false);
        _doc->advanceModelGeneration();
    }
}

} // namespace App
