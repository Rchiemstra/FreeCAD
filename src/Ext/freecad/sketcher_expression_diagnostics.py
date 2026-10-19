# SPDX-License-Identifier: LGPL-2.1-or-later
"""Adapt the observational diagnostics report to Sketcher constraint indices.

Keep classification and dependency traversal in expression_diagnostics. A pending
stored value or an expression which cannot safely be evaluated is not evidence
of a broken expression. Only actual failures and their consequences are marked.
"""
import re

import FreeCAD as App

from freecad.expression_diagnostics import diagnose

_FAILURES = {"unresolved_reference", "external_document", "unit_error",
             "evaluation_error", "dependency_cycle"}


def _stored_parameters(full_name):
    document_name, object_name = full_name.split("#", 1)
    document = App.listDocuments().get(document_name)
    obj = document.getObject(object_name) if document is not None else None
    # A plain VarSet stores parameters; it has no generated outputs. Keep
    # execution-dependent feature outputs (including derived types) conservative.
    return obj is not None and obj.TypeId == "App::VarSet"


def constraint_errors(sketch):
    """Return {zero_based_constraint_index: reason}, without changing the sketch."""
    if not any(path.lstrip(".").startswith("Constraints") and expression
               for path, expression in sketch.ExpressionEngine):
        return {}
    rows = diagnose(sketch.Document)
    roots = {(row["object"], row["property"]): row for row in rows
             if row["role"] == "root" and row["kind"] in _FAILURES}
    errors = {}
    for row in rows:
        if row["object"] != sketch.FullName or not row["chain"]:
            continue
        first = row["chain"][0]
        root = roots.get((first["object"], first["property"]))
        if root is None:
            continue
        intermediate = row["chain"][1:-1]
        if (row["role"] == "affected" and intermediate
                and _stored_parameters(root["object"])
                and all(not step["property"] and _stored_parameters(step["object"])
                        for step in intermediate)):
            # Coarse scheduling exposes every consumer of a failed object.
            # Another stored VarSet input is still valid; a property-to-property
            # chain through a failing binding remains an actual constraint cause.
            continue
        path = row["property"]
        match = re.fullmatch(r"Constraints\[(\d+)\]", path)
        if match:
            index = int(match[1])
        elif path.startswith("Constraints."):
            name = path[len("Constraints."):]
            if name.startswith("<<") and name.endswith(">>"):
                name = name[2:-2]
            try:
                index = sketch.getIndexByName(name)
            except (ValueError, LookupError):
                continue
        else:
            continue
        if not 0 <= index < sketch.ConstraintCount:
            continue
        reason = f'{path} = {row["expression"]}\n{root["message"]}'
        if row["role"] == "affected":
            reason += "\n" + row["message"]
        reasons = errors.setdefault(index, [])
        if reason not in reasons:
            reasons.append(reason)
    return {index: "\n".join(reasons) for index, reasons in errors.items()}
