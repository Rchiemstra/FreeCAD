// SPDX-License-Identifier: LGPL-2.1-or-later

#include <memory>

#include <Base/Exception.h>
#include <Base/PyObjectBase.h>

#include "DocumentObject.h"
#include "DocumentObjectPy.h"
#include "Expression.h"
#include "ExpressionParser.h"
#include "ObjectIdentifier.h"
#include "Property.h"

using namespace App;

PyObject* DocumentObjectPy::getExpressionReferences(PyObject* args) const
{
    const char* text = nullptr;
    if (!PyArg_ParseTuple(args, "s", &text)) {
        return nullptr;
    }

    // Parse a temporary expression. Do not evaluate it, modify the engine, or
    // rebuild dependencies. Resolution errors are evidence, not exceptions.
    PY_TRY
    {
        std::unique_ptr<Expression> expression(Expression::parse(getDocumentObjectPtr(), text));
        Py::List result;
        if (!expression) {
            return Py::new_reference_to(result);
        }
        for (const auto& [identifier, hidden] : expression->getIdentifiers()) {
            Py::Dict reference;
            reference["identifier"] = Py::String(identifier.toString());
            reference["hidden"] = Py::Boolean(hidden);
            reference["external_document"] = Py::Boolean(
                !identifier.getDocumentName().getString().empty() && !identifier.getDocument());
            auto* object = identifier.getDocumentObject();
            int pseudoProperty = 0;
            auto* property = identifier.getProperty(&pseudoProperty);
            reference["error"] = Py::String(
                property || pseudoProperty ? "" : identifier.resolveErrorString());
            if (property) {
                if (auto* owner = dynamic_cast<DocumentObject*>(property->getContainer())) {
                    object = owner;
                }
            }
            reference["object"] = object ? Py::Object(object->getPyObject(), true) : Py::None();
            // Only ask for the property name when resolution succeeded: unresolved
            // identifiers need not have a valid property component index.
            std::string path;
            if (property) {
                auto canonical = identifier.canonicalPath();
                path = canonical.getPropertyName() + canonical.getSubPathStr();
            }
            reference["property"] = Py::String(path);
            result.append(reference);
        }
        return Py::new_reference_to(result);
    }
    PY_CATCH
}
