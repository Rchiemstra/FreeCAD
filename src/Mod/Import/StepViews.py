# SPDX-License-Identifier: LGPL-2.1-or-later
"""Owned AP242 saved views, schema 1; data only, with no activation on restore.

Import[Gui].open/insert(savedViews=True) overrides ImportSavedViews (default off)
in BaseApp/Preferences/Mod/Import. Scalars and points are mm when units are
verified; directions are unitless. SourceId is an import-scoped XCAF label entry
for views, a STEP entity ID for planes. StepEntityId retains the STEP ID too.
CameraPosition = Origin + Right*x + Up*y + Backward*z for ProjectionPoint in VRS.
ViewDirection is backward (+Z), UpDirection is +Y. ViewPlaneDistance is a length,
not a zoom ratio. Planes store n.x + Offset = 0 with positive retained half-space.
RawMetadata preserves supplier values even when presentation is unavailable.
Native links are authoritative after document copy; provenance names are not
replayed on restore, undo or redo. No source file or Python proxy is required.
"""
import json
import math


def _property(obj, kind, name, value, description="Owned STEP saved-view data"):
    obj.addProperty("App::Property" + kind, name, "STEP saved views", description)
    setattr(obj, name, value)
    obj.setEditorMode(name, 1)


def _finite(value):
    """Keep nonfinite source values as explicit strings in strict JSON."""
    if isinstance(value, float) and not math.isfinite(value):
        return str(value)
    if isinstance(value, list):
        return [_finite(v) for v in value]
    if isinstance(value, dict):
        return {k: _finite(v) for k, v in value.items()}
    return value


def create(document, records, pmi_names=()):
    """Materialize only owned records and links observed during this import.

    Rollback removes this call's explicit object list, preserving geometry, PMI,
    earlier groups and objects added by document observers.
    """
    if not records:
        return None
    created = []
    try:
        group = document.addObject("App::DocumentObjectGroup", "StepViews")
        created.append(group.Name)
        group.Label = "STEP saved views"
        _property(group, "Integer", "MetadataVersion", 1)
        _property(group, "String", "MetadataKind", "SavedViews")
        objects, plane_objects = [], {}
        for record in records:
            plane = record["RecordType"] == "ClippingPlane"
            key = (record["SourceId"], record["sourceLengthFactor"])
            if plane and key in plane_objects:
                continue
            obj = document.addObject("App::FeaturePython", "StepClipPlane" if plane else "StepSavedView")
            created.append(obj.Name)
            group.addObject(obj)
            obj.Label = record["Name"] or record["RecordType"]
            reason = record.get("UnavailableReason", "")
            raw = _finite(record)
            if raw != record:
                reason += "nonfinite source value; "
            for name, value in record.items():
                if name in ("planes", "references", "Name", "UnavailableReason"):
                    continue
                if isinstance(value, bool):
                    kind = "Bool"
                elif isinstance(value, str):
                    kind = "String"
                elif isinstance(value, (int, float)):
                    if not math.isfinite(value):
                        continue
                    kind = "Float"
                elif isinstance(value, list) and all(isinstance(v, (int, float)) and math.isfinite(v)
                                                   for v in value):
                    kind = "FloatList"
                else:
                    continue
                _property(obj, kind, name, value)
            _property(obj, "String", "UnavailableReason", reason)
            _property(obj, "String", "PresentationStatus", "unavailable" if reason else "data available")
            _property(obj, "String", "RawMetadata", json.dumps(
                {"schemaVersion": 1, "record": raw}, ensure_ascii=False,
                sort_keys=True, allow_nan=False))
            if plane:
                plane_objects[key] = obj
            objects.append((obj, record))

        pmi_by_id = {}
        # Names supplied by the importer are THIS call's newly materialized PMI.
        for name in pmi_names:
            obj = document.getObject(name)
            if obj is not None and hasattr(obj, "SourceId"):
                pmi_by_id.setdefault(obj.SourceId, []).append(obj)
        imported = []
        for obj, record in objects:
            if record["RecordType"] != "SavedView":
                continue
            shapes, pmi, missing = [], [], []
            for ref in record["references"]:
                reason = ref["reason"]
                if ref["role"] == "shape":
                    owner = document.getObject(ref["objectName"]) if ref["objectName"] else None
                    if owner is not None and not reason:
                        shapes.append(owner)
                    else:
                        reason = reason or "exact imported owner unavailable"
                elif ref["role"] == "pmi":
                    candidates = pmi_by_id.get(ref["sourceId"], [])
                    if len(candidates) == 1 and not reason:
                        pmi.append(candidates[0])
                    else:
                        reason = "PMI disabled, omitted or ambiguous in this import"
                else:
                    reason = "unsupported association role"
                if reason:
                    missing.append(f"{ref['role']} {ref['sourceId']} ({ref['scope']}): {reason}")
            planes = []
            for source in record["planes"]:
                plane = plane_objects.get((source, record["sourceLengthFactor"]))
                if plane is None:
                    missing.append("plane " + source + ": unavailable")
                elif plane not in planes:
                    planes.append(plane)
            _property(obj, "LinkList", "Shapes", shapes, "Exact owners from this import only")
            _property(obj, "LinkList", "Pmi", pmi, "Semantic PMI from this import only")
            _property(obj, "LinkList", "Planes", planes)
            _property(obj, "StringList", "UnresolvedReferences", missing)
            _property(obj, "String", "AssociationStatus", "unavailable" if missing else
                      "resolved" if shapes or pmi else "no supplied associations")
            for owner in shapes + pmi:
                if owner not in imported:
                    imported.append(owner)
        _property(group, "LinkList", "ImportedObjects", imported, "Observed owners in this import")
        return group
    except Exception:
        for name in reversed(created):
            try:
                document.removeObject(name)
            except Exception:
                pass
        raise
