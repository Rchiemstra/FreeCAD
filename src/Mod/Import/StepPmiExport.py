# SPDX-License-Identifier: LGPL-2.1-or-later
"""Validation for opt-in Import[Gui].export(..., semanticPMI=True).

ExportSemanticPMI in Mod/Import defaults to false. Enabled export returns one
{object, status, reason} entry per PMI object. Source IDs are document scoped.
Older snapshots without a geometry fingerprint cannot establish unchanged geometry.
"""
import hashlib
import json
import re
from collections import Counter

import FreeCAD as App


def fingerprint(owner):
    """Hash the complete local BREP, excluding placement and tessellation caches."""
    shape = owner.Shape.copy()
    shape.Placement = App.Placement()
    brep = shape.cleaned().exportBrepToString()
    geometry, separator, topology = brep.partition("\nTShapes ")
    if not separator:
        raise ValueError("unsupported BREP fingerprint format")
    # BREP's topology records have seven flags on a line of their own. Free,
    # Modified and Checked are mutable kernel caches; keep the four shape flags.
    topology = re.sub(r"(?m)^[01]{3}([01]{4})$", r"000\1", topology)
    return hashlib.sha256((geometry + separator + topology).encode("utf-8")).hexdigest()


def objects(owners):
    documents = dict.fromkeys(owner.Document for owner in owners)
    return [obj for doc in documents for obj in doc.Objects
            if hasattr(obj, "RawMetadata") and hasattr(obj, "Category")]


def prepare(owners):
    """Return independently validated candidates; C++ binds only converter labels."""
    result = []
    current_fingerprints = {}
    for obj in objects(owners):
        item = {"object": obj, "reason": "", "record": {}, "references": []}
        result.append(item)
        try:
            raw = json.loads(obj.RawMetadata)
            record = raw["record"]
            item["record"] = record
            if obj.ResolutionStatus != "resolved":
                raise ValueError("unresolved reference: " + obj.ResolutionStatus)
            if any(getattr(group, "Warnings", []) for group in obj.InList
                   if hasattr(group, "MetadataVersion")):
                raise ValueError("unsupported source metadata; file-level reader warnings")
            if obj.Warnings or record["warnings"]:
                raise ValueError("unsupported or incomplete source semantics")
            live_references = Counter((owner, sub)
                                      for owner, subs in obj.References for sub in subs)
            snapshot_references = Counter((obj.Document.getObject(ref["objectName"]),
                                           ref["subElement"]) for ref in record["references"])
            if (live_references != snapshot_references
                    or any(not subs for _, subs in obj.References)):
                raise ValueError("shape references changed after import")
            # IDs are scoped to the importing group, so a same-ID datum from
            # another import cannot replace a removed constraint.
            groups = [group for group in obj.InList if hasattr(group, "MetadataVersion")]
            live_datums = obj.Datums
            if (sorted(datum.SourceId for datum in live_datums) != sorted(record["datumIds"])
                    or any(not any(datum in group.Group for group in groups)
                           for datum in live_datums)):
                raise ValueError("datum references changed after import")
            snapshots = raw.get("geometryFingerprints", {})
            for ref in record["references"]:
                owner = obj.Document.getObject(ref["objectName"])
                if owner is None or not hasattr(owner, "Shape") or owner.Shape.isNull():
                    raise ValueError("referenced shape removed")
                if owner.Name not in snapshots:
                    raise ValueError("missing imported geometry fingerprint")
                if owner not in current_fingerprints:
                    current_fingerprints[owner] = fingerprint(owner)
                if current_fingerprints[owner] != snapshots[owner.Name]:
                    raise ValueError("referenced shape changed after import")
                sub = ref["subElement"]
                if not sub.startswith(("Face", "Edge")):
                    raise ValueError("unsupported reference type: " + sub)
                owner.Shape.getElement(sub)
                item["references"].append((owner, sub, ref["role"]))
            if record["category"] == "Dimension":
                # Path/oriented dimensions need direction/path data absent from schema 1.
                if record["type"] not in tuple(range(2, 12)) + tuple(range(15, 29)):
                    raise ValueError("unsupported dimension type")
                if not record["hasValue"] or record.get("deviationStatus", "").startswith(
                        ("magnitudes only", "uninterpreted")):
                    raise ValueError("unsupported dimension value or unknown deviation signs")
            elif record["category"] == "Tolerance":
                if not 1 <= record["type"] <= 15:
                    raise ValueError("unsupported geometric tolerance type")
            elif record["category"] == "Datum":
                if record["numbers"].get("isDatumTarget"):
                    raise ValueError("unsupported datum target parameters")
            else:
                raise ValueError("unsupported PMI category")
        except Exception as error:
            item["reason"] = str(error)
    return result
