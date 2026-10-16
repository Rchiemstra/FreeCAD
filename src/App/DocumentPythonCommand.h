// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentCommandHandle.h"

#include <FCGlobal.h>

struct _object;
using PyObject = _object;

namespace App
{

AppExport PyObject* makeDocumentCommandHandlePy(const DocumentCommandHandle& handle);

}  // namespace App
