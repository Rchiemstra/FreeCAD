# SPDX-License-Identifier: LGPL-2.1-or-later
"""Persistent semantic STEP PMI snapshots, created while the XCAF source is alive.

Import.open/insert(..., semanticPMI=True) overrides the default-off preference
BaseApp/Preferences/Mod/Import/ImportSemanticPMI. False explicitly disables it.
Off leaves OCCT's own default GDT mode unchanged (default-on in OCCT 7.6).
These objects need neither a proxy class nor the original STEP file to restore.
Values are normalised by OCCT (mm, or degrees for angular dimensions).
RawMetadata schema 1 stores unpresented fields and reference roles/source labels;
source identifiers are XCAF label entries scoped to this import, not STEP entity IDs.
"""

import json
import re

import FreeCAD as App


def _property(obj, kind, name, value, description):
    obj.addProperty("App::Property" + kind, name, "STEP PMI", description)
    setattr(obj, name, value)
    obj.setEditorMode(name, 1)


def _kind(record):
    name = record["kind"]
    if name == "Location_LinearDistance":
        return "linear distance"
    return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", name.replace("_", " ")).lower()


def _summary(record, datums):
    result = _kind(record)
    unit = record["unit"]
    if record["hasValue"]:
        result += f" {record['value']:g} {unit}"
    if record["hasDeviations"]:
        result += f", {record['lower']:+g}/{record['upper']:+g} {unit}"
    if record.get("deviationStatus") == "magnitudes only; signs unknown":
        result += f", deviation magnitudes {record['lower']:g}/{record['upper']:g} {unit} (signs unknown)"
    if record.get("deviationStatus") == "uninterpreted magnitudes; source semantics unavailable":
        magnitudes = "/".join(f"{abs(value):g}" for value in record["values"])
        result += f", uninterpreted magnitudes {magnitudes} {unit} (source semantics unavailable)"
    if record["hasBounds"]:
        result += f", bounds {record['lower']:g} to {record['upper']:g} {unit}"
    if datums:
        result += "; datum " + ", ".join(datum.DatumIdentifier for datum in datums)
    if record["category"] == "Datum":
        result = "datum " + record["datumName"]
    return result.strip()


def create(document, records, warnings=()):
    """Materialise owned extraction records using ordinary native properties only."""
    if not records and not warnings:
        return None
    created = []
    try:
        group = document.addObject("App::DocumentObjectGroup", "StepPMI")
        created.append(group.Name)
        group.Label = "STEP PMI"
        _property(group, "Integer", "MetadataVersion", 1, "Raw metadata schema version")
        _property(group, "StringList", "Warnings", list(warnings), "File-level STEP reader warnings")
        _property(group, "String", "RawMetadata", json.dumps(
            {"schemaVersion": 1, "warnings": list(warnings)}, ensure_ascii=False, sort_keys=True
        ), "File-level reader metadata; independent of the STEP source file")
        objects = {}
        counts = {}
        for record in records:
            category = record["category"]
            counts[category] = counts.get(category, 0) + 1
            obj = document.addObject("App::FeaturePython", "StepPMI" + category)
            created.append(obj.Name)
            group.addObject(obj)
            obj.Label = ("Datum " + record["datumName"] if category == "Datum"
                         else f"{category} {counts[category]}")
            objects[record["sourceId"]] = obj
            for name, value, description in (
                ("Category", category, "Semantic PMI record category"),
                ("Kind", _kind(record), "Dimension or geometric tolerance type"),
                ("Units", record["unit"], "Normalised units: mm or deg; empty for datums/labels"),
                ("SourceId", record["sourceId"], "XCAF label entry, scoped to this import"),
                ("SemanticName", record["name"], "Supplier semantic name"),
                ("DatumIdentifier", record["datumName"], "Datum identification"),
            ):
                _property(obj, "String", name, value, description)
            _property(obj, "FloatList", "Values", record["values"], "OCCT value array in normalised units")
            _property(obj, "Bool", "HasValue", record["hasValue"], "A numeric value is present")
            if record["hasValue"]:
                _property(obj, "Float", "Value", record["value"], "Nominal value in Units")
            _property(obj, "String", "DeviationStatus", record.get("deviationStatus", ""),
                      "Whether deviation signs were verified against the STEP source")
            if record.get("deviationStatus") == "magnitudes only; signs unknown":
                _property(obj, "Float", "LowerDeviationMagnitude", record["lower"],
                          "Unsigned lower deviation magnitude")
                _property(obj, "Float", "UpperDeviationMagnitude", record["upper"],
                          "Unsigned upper deviation magnitude")
            if record.get("deviationStatus") == "uninterpreted magnitudes; source semantics unavailable":
                _property(obj, "FloatList", "UninterpretedMagnitudes",
                          [abs(value) for value in record["values"]],
                          "OCCT magnitudes; cannot distinguish a range from lost tolerance data")
            _property(obj, "Bool", "HasDeviations", record["hasDeviations"], "Signed deviations are present")
            if record["hasDeviations"]:
                _property(obj, "Float", "LowerDeviation", record["lower"], "Signed lower deviation in Units")
                _property(obj, "Float", "UpperDeviation", record["upper"], "Signed upper deviation in Units")
            _property(obj, "Bool", "HasBounds", record["hasBounds"], "Absolute range bounds are present")
            if record["hasBounds"]:
                _property(obj, "Float", "LowerBound", record["lower"], "Absolute lower bound in Units")
                _property(obj, "Float", "UpperBound", record["upper"], "Absolute upper bound in Units")
            _property(obj, "Integer", "DatumPosition", record["datumPosition"], "Datum system compartment position")
            _property(obj, "StringList", "Modifiers", record["modifiers"], "OCCT modifiers, including unknown codes")
            _property(obj, "StringList", "Warnings", record["warnings"], "Unsupported or missing semantic fields")
            _property(obj, "String", "RawMetadata", json.dumps(
                {"schemaVersion": 1, "record": record}, ensure_ascii=False, sort_keys=True
            ), "Versioned semantic metadata; independent of the STEP source file")

        from StepPmiExport import fingerprint

        geometry_fingerprints = {}
        for record in records:
            obj = objects[record["sourceId"]]
            datum_ids = record["datumIds"]
            datums = [objects[source] for source in datum_ids if source in objects]
            datums.sort(key=lambda datum: datum.DatumPosition)
            _property(obj, "LinkList", "Datums", datums, "Datum system in compartment order")
            _property(obj, "IntegerList", "DatumSystemPositions",
                      [datum.DatumPosition for datum in datums], "Positions; equal values share a compartment")
            references = []
            fingerprints = {}
            unresolved = []
            for ref in record["references"]:
                owner = document.getObject(ref["objectName"]) if ref["objectName"] else None
                if ref["status"] == "resolved" and owner is not None:
                    references.append((owner, [ref["subElement"]] if ref["subElement"] else []))
                    if hasattr(owner, "Shape"):
                        if owner.Name not in geometry_fingerprints:
                            geometry_fingerprints[owner.Name] = fingerprint(owner)
                        fingerprints[owner.Name] = geometry_fingerprints[owner.Name]
                else:
                    unresolved.append(f"{ref['role']} {ref['sourceId']}: {ref['status']}")
            raw = json.loads(obj.RawMetadata)
            raw["geometryFingerprints"] = fingerprints
            obj.RawMetadata = json.dumps(raw, ensure_ascii=False, sort_keys=True)
            unresolved.extend("datum " + source for source in datum_ids if source not in objects)
            unsupported = any("unsupported association" in warning for warning in record["warnings"])
            if unsupported:
                status = "unsupported association"
            elif unresolved:
                status = "unresolved reference"
            elif not record["references"]:
                status = "no shape reference"
            else:
                status = "resolved"
            _property(obj, "LinkSubList", "References", references, "Exact imported object/subshape associations")
            _property(obj, "StringList", "UnresolvedReferences", unresolved, "Unresolved source labels and reasons")
            _property(obj, "String", "ResolutionStatus", status, "Geometry association status")
            _property(obj, "String", "Summary", _summary(record, datums), "Readable semantic PMI summary")
        if any(obj.ResolutionStatus != "resolved" for obj in objects.values()):
            App.Console.PrintWarning("STEP PMI: some geometry associations are unresolved; see ResolutionStatus.\n")
        return group
    except Exception:
        for name in reversed(created):
            try:
                document.removeObject(name)
            except Exception:
                # Continue attempting every owned object even if a removal observer fails.
                pass
        raise
