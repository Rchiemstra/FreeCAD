# SPDX-License-Identifier: LGPL-2.1-or-later
"""Translated presentation for machine-readable expression reports.

Object names, paths, formulas and native exception details remain technical data.
All explanatory text belongs here, shared by Tools, the Data tab and Sketcher.
"""
import FreeCAD as App

translate = App.Qt.translate


def role_text(role):
    return {
        "root": translate("ExpressionDiagnostics", "Root cause"),
        "affected": translate("ExpressionDiagnostics", "Affected"),
    }.get(role, translate("ExpressionDiagnostics", "Unknown"))


def kind_text(kind):
    return {
        "unresolved_reference": translate("ExpressionDiagnostics", "Unresolved reference"),
        "external_document": translate("ExpressionDiagnostics", "Unavailable external document"),
        "unit_error": translate("ExpressionDiagnostics", "Unit error"),
        "evaluation_error": translate("ExpressionDiagnostics", "Evaluation error"),
        "dependency_cycle": translate("ExpressionDiagnostics", "Dependency cycle"),
        "stale_value": translate("ExpressionDiagnostics", "Stored value needs recompute"),
        "not_evaluated": translate("ExpressionDiagnostics", "Not evaluated"),
        "dependency": translate("ExpressionDiagnostics", "Dependency"),
    }.get(kind, translate("ExpressionDiagnostics", "Unknown"))


def reason_text(row):
    kind = row["kind"]
    if row["role"] == "affected":
        root = row["chain"][0]
        return translate("ExpressionDiagnostics", "Depends on {object}.{property}.").format(
            object=root["object"], property=root["property"])
    if kind == "stale_value":
        return translate("ExpressionDiagnostics", "Stored value differs from the expression evaluated with current inputs.")
    if kind == "dependency_cycle":
        chain = " → ".join(step["object"] + "." + step["property"] for step in row["chain"])
        return translate("ExpressionDiagnostics", "Expression dependency cycle: {chain}").format(chain=chain)
    if kind == "not_evaluated":
        return translate("ExpressionDiagnostics", "Evaluation was skipped because accessing Python attributes may modify the document.")
    return translate("ExpressionDiagnostics", "{kind}. Details: {details}").format(
        kind=kind_text(kind), details=row["message"])
