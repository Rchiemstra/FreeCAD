# SPDX-License-Identifier: LGPL-2.1-or-later
"""Adapt the observational diagnostics report to Sketcher constraint indices.

Keep classification and dependency traversal in expression_diagnostics. A pending
stored value or an expression which cannot safely be evaluated is not evidence
of a broken expression. Only actual failures and their consequences are marked.
"""
import re

import FreeCAD as App

from freecad.expression_diagnostics import diagnose, _path_related

translate = App.Qt.translate

_FAILURES = {"unresolved_reference", "external_document", "unit_error",
             "evaluation_error", "dependency_cycle"}


def _stored_parameters(full_name):
    document_name, object_name = full_name.split("#", 1)
    document = App.listDocuments().get(document_name)
    obj = document.getObject(object_name) if document is not None else None
    # A plain VarSet stores parameters; it has no generated outputs. Keep
    # execution-dependent feature outputs (including derived types) conservative.
    return obj is not None and obj.TypeId == "App::VarSet"


def _binding_depends_on(sketch, path, root):
    """Check actual bindings when the report's shortest chain is coarse.

    An alternate, longer binding chain may still reach the root. Plain VarSet
    inputs do not require execution; generated feature outputs remain exposed
    to failures in the feature's bindings and structural inputs.
    """
    pending, seen = [(sketch, path)], set()
    while pending:
        obj, path = pending.pop()
        key = (obj.FullName, path)
        if key in seen:
            continue
        seen.add(key)
        if key[0] == root["object"] and (not path or _path_related(path, root["property"])):
            return True
        bindings = [(bound.lstrip("."), expression) for bound, expression in obj.ExpressionEngine
                    if expression]
        matching = [(bound, expression) for bound, expression in bindings
                    if path and _path_related(path, bound)]
        if not matching and obj.TypeId == "App::VarSet":
            continue
        for _, expression in matching or bindings:
            for ref in obj.getExpressionReferences(expression):
                if ref["object"] is not None:
                    pending.append((ref["object"], ref["property"]))
        if not matching:
            for edge in obj.OutListProp:
                if edge.FromProp != "ExpressionEngine" and edge.ToObj is not None:
                    pending.append((edge.ToObj, ""))
    return False


def references_solver_outputs(sketch):
    """Solver publication matters only if a binding reads those outputs.

    Inspect dependency metadata under the caller's read reservation; do not
    evaluate expressions just to decide whether the snapshot is invalidated.
    """
    pending, seen = list(sketch.Document.Objects), set()
    while pending:
        obj = pending.pop()
        if obj.FullName in seen:
            continue
        seen.add(obj.FullName)
        for _, expression in obj.ExpressionEngine:
            if not expression:
                continue
            try:
                references = obj.getExpressionReferences(expression)
            except Exception:
                continue
            for ref in references:
                source = ref["object"]
                if source is None:
                    continue
                if source == sketch and ref["property"].split(".")[0].split("[")[0] in {
                        "Geometry", "FullyConstrained"}:
                    return True
                pending.append(source)
        for edge in obj.OutListProp:
            if edge.FromProp != "ExpressionEngine" and edge.ToObj is not None:
                pending.append(edge.ToObj)
    return False


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
        if (row["role"] == "affected"
                and any(not step["property"] and _stored_parameters(step["object"])
                        for step in intermediate)
                and not _binding_depends_on(sketch, row["property"], root)):
            # An object-only hop through a plain VarSet is coarse scheduling
            # exposure, even when followed by more property bindings. Stored
            # parameters propagate failures only through their actual bindings.
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
            kinds = {
                "unresolved_reference": translate("SketcherExpressionDiagnostics", "Unresolved reference"),
                "external_document": translate("SketcherExpressionDiagnostics", "Unavailable external document"),
                "unit_error": translate("SketcherExpressionDiagnostics", "Unit error"),
                "evaluation_error": translate("SketcherExpressionDiagnostics", "Evaluation error"),
                "dependency_cycle": translate("SketcherExpressionDiagnostics", "Dependency cycle"),
            }
            reason += "\n" + translate(
                "SketcherExpressionDiagnostics", "Depends on {object}.{property} ({kind})."
            ).format(object=root["object"], property=root["property"], kind=kinds[root["kind"]])
        reasons = errors.setdefault(index, [])
        if reason not in reasons:
            reasons.append(reason)
    return {index: "\n".join(reasons) for index, reasons in errors.items()}
