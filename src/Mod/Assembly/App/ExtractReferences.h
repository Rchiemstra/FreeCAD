// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <Python.h>

namespace App
{
class DocumentObject;
}
namespace Assembly
{
struct ReferenceReplacement
{
    App::DocumentObject* oldObject;
    App::DocumentObject* link;
    App::DocumentObject* geometry;
    App::DocumentObject* definition;
    bool component;
    App::DocumentObject* shape;
    App::DocumentObject* globalGeometry;
    App::DocumentObject* attachment;
};
PyObject* extractionReferences(PyObject* args);
} // namespace Assembly
