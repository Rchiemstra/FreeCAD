# SPDX-License-Identifier: LGPL-2.1-or-later
"""Research-only serial probe; copy to .FCMacro and run with FreeCADCmd.

Existing FCStd inputs and repository files are never saved over. Generated fixtures
and JSONL go into the explicitly supplied M0_OUT directory. See the M0 report for
commands, provenance requirements, limitations, and the remaining native gates.
"""

import hashlib
import json
import os
from pathlib import Path
import platform
import sys
import time


def make_fixture(app, case):
    family, count = case.split(":")
    count = int(count)
    if not 1 <= count <= 512:
        raise ValueError("fixture count must be in [1, 512]")
    doc = app.newDocument("M0")
    edit = None

    def box(name, dimensions, position):
        obj = doc.addObject("Part::Box", name)
        obj.Length, obj.Width, obj.Height = dimensions
        obj.Placement.Base = app.Vector(*position)
        return obj

    if family == "box":
        for i in range(count):
            obj = box(f"Box{i:04}", (10, 20, 30), (60 * i, 0, 0))
            if edit is None:
                edit = (obj, "Length", 10.0)
    elif family in ("cut", "fuse", "common", "pair"):
        for i in range(count):
            x = 60 * i
            base = box(f"Base{i:04}", (40, 30, 20), (x, 0, 0))
            tool = box(f"Tool{i:04}", (10, 10, 30), (x + 15, 10, -5))
            kind = "Cut" if family == "pair" else family.capitalize()
            result = doc.addObject(f"Part::{kind}", f"Result{i:04}")
            result.Base, result.Tool, result.Refine = base, tool, False
            if family == "pair":
                lug = box(f"Lug{i:04}", (8, 12, 20), (x + 36, 9, 0))
                final = doc.addObject("Part::Fuse", f"Final{i:04}")
                final.Base, final.Tool, final.Refine = result, lug, False
            if edit is None:
                edit = (base, "Length", 40.0)
    elif family in ("chain", "bodies"):
        for i in range(1 if family == "chain" else count):
            body = doc.addObject("PartDesign::Body", f"Body{i:04}")
            for j in range(count if family == "chain" else 1):
                obj = body.newObject("PartDesign::AdditiveBox", f"Add{i:04}_{j:04}")
                obj.Length, obj.Width, obj.Height = 10, 10, 1
                obj.Placement.Base = app.Vector(20 * i, 0, 0.9 * j)
                if edit is None:
                    edit = (obj, "Length", 10.0)
            if family == "bodies":
                hole = body.newObject("PartDesign::SubtractiveCylinder", f"Hole{i:04}")
                hole.Radius, hole.Height = 2, 2
                hole.Placement.Base = app.Vector(20 * i + 5, 5, -0.5)
    else:
        raise ValueError(f"unknown fixture family: {family}")
    return doc, edit


def verify(doc):
    invalid = [obj.Name for obj in doc.Objects if not obj.isValid()]
    unsettled = [obj.Name for obj in doc.Objects if "Touched" in obj.State]
    if invalid or unsettled:
        raise RuntimeError(f"invalid={invalid}, unsettled={unsettled}")


def shape_summary(doc):
    # Outside the timer; this deliberately is not a naming/equivalence oracle.
    result = {}
    for obj in doc.Objects:
        if "Shape" not in obj.PropertiesList:
            continue
        # FEM mesh Shape is an App::PropertyLink, not a TopoShape. Its linked
        # Part object's actual BRep is checked separately in this same document.
        if obj.getTypeIdOfProperty("Shape") != "Part::PropertyPartShape":
            continue
        shape = obj.Shape
        if shape.isNull():
            if obj.TypeId in {"Part::Box", "Part::Cut", "Part::Fuse", "Part::Common",
                              "PartDesign::AdditiveBox", "PartDesign::SubtractiveCylinder"}:
                raise RuntimeError(f"null target shape: {obj.Name}")
            result[obj.Name] = {"null": True}
        else:
            if not shape.isValid():
                raise RuntimeError(f"invalid BRep: {obj.Name}")
            result[obj.Name] = {
                "valid": shape.isValid(),
                "solids": len(shape.Solids), "faces": len(shape.Faces),
                "edges": len(shape.Edges), "vertices": len(shape.Vertexes),
                "area": shape.Area, "volume": shape.Volume,
                "placement": str(obj.Placement) if "Placement" in obj.PropertiesList else None,
            }
    return result


def main():
    import FreeCAD as app
    import Part
    from PySide6.QtCore import qVersion

    out = Path(os.environ["M0_OUT"]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    case = os.environ.get("M0_CASE", "pair:24")
    scenario = os.environ.get("M0_SCENARIO", "all")
    warmups = int(os.environ.get("M0_WARMUPS", "5"))
    repetitions = int(os.environ.get("M0_REPETITIONS", "30"))
    if scenario not in ("all", "edit", "noop") or warmups < 0 or repetitions < 1:
        raise ValueError("invalid scenario or sample counts")

    start = time.perf_counter_ns()
    if case == "file":
        source = Path(os.environ["M0_FILE"]).resolve(strict=True)
        doc = app.openDocument(str(source))
        edit = None
        if "M0_EDIT" in os.environ:
            name, prop = os.environ["M0_EDIT"].split(":")
            obj = doc.getObject(name)
            edit = (obj, prop, float(getattr(obj, prop)))
    else:
        doc, edit = make_fixture(app, case)
        doc.recompute(None, False, True)
        verify(doc)
        source = out / "fixture.FCStd"
        if source.exists():
            raise FileExistsError(source)
        doc.saveAs(str(source))
        # Measure restored archives, not the still-live creation graph. Rebind
        # the edit target because wrappers from the closed document are invalid.
        edit_spec = (edit[0].Name, edit[1], edit[2]) if edit else None
        created_types = {obj.Name: obj.TypeId for obj in doc.Objects}
        app.closeDocument(doc.Name)
        doc = app.openDocument(str(source))
        if {obj.Name: obj.TypeId for obj in doc.Objects} != created_types:
            raise RuntimeError("synthetic object names/types changed on restore")
        edit = (doc.getObject(edit_spec[0]), *edit_spec[1:]) if edit_spec else None
    load_or_create_ns = time.perf_counter_ns() - start
    unresolved = [obj.Name for obj in doc.Objects
                  if "Proxy" in obj.PropertiesList and obj.Proxy is None]
    (out / "restore.json").write_text(json.dumps({
        "objects": len(doc.Objects), "types": {obj.Name: obj.TypeId for obj in doc.Objects},
        "unresolved_proxies": unresolved, "load_or_create_ns": load_or_create_ns,
    }, sort_keys=True) + "\n")
    if unresolved:
        raise RuntimeError(f"unrestored proxies: {unresolved}")
    if scenario == "edit" and edit is None:
        raise ValueError("file edit scenario requires M0_EDIT=ObjectName:NumericProperty")

    with (out / "samples.jsonl").open("x") as stream:
        def emit(record):
            stream.write(json.dumps(record, sort_keys=True) + "\n")
            stream.flush()

        emit({
            "kind": "manifest", "case": case, "scenario": scenario,
            "freecad_version": list(app.Version()), "occ_version": Part.OCC_VERSION,
            "qt_version": qVersion(),
            "build_sha_supplied": os.environ["M0_BUILD_SHA"],
            "python": sys.version, "platform": platform.platform(),
            "fixture": str(source), "fixture_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "load_or_create_ns": load_or_create_ns, "objects": len(doc.Objects),
            "types": {obj.Name: obj.TypeId for obj in doc.Objects},
            "coarse_outlist_only": {obj.Name: [dep.Name for dep in obj.OutList] for obj in doc.Objects},
            "warmups": warmups, "repetitions": repetitions,
            "edit": [edit[0].Name, edit[1], edit[2]] if edit else None,
            "preferences": {
                "check_model": app.ParamGet("User parameter:BaseApp/Preferences/Mod/Part/Boolean").GetBool("CheckModel", True),
                "refine_default": app.ParamGet("User parameter:BaseApp/Preferences/Mod/Part/Boolean").GetBool("RefineModel", True),
                "random_color": app.ParamGet("User parameter:BaseApp/Preferences/View").GetBool("RandomColor", False),
                "material_external": app.ParamGet("User parameter:BaseApp/Preferences/Mod/Material/ExternalInterface").GetBool("UseExternal", False),
            },
            "venue": "existing-serial", "phase_timings": None,
            "aggregate_rss": None, "gui_presentation": None, "native_overlap": None,
        })
        for index in range(warmups + repetitions):
            action_start = time.perf_counter_ns()
            if scenario == "all":
                for obj in doc.Objects:
                    obj.touch()
            elif scenario == "edit":
                obj, prop, original = edit
                setattr(obj, prop, original + (0.01 if index % 2 == 0 else 0.0))
            mutation_ns = time.perf_counter_ns() - action_start
            dirty = [obj.Name for obj in doc.Objects if "Touched" in obj.State]
            cpu_start = time.process_time_ns()
            compute_start = time.perf_counter_ns()
            returned = doc.recompute(None, False, True)
            recompute_ns = time.perf_counter_ns() - compute_start
            cpu_ns = time.process_time_ns() - cpu_start
            action_total_ns = time.perf_counter_ns() - action_start
            verify(doc)
            emit({
                "kind": "sample", "warmup": index < warmups, "index": index,
                "mutation_ns": mutation_ns, "recompute_ns": recompute_ns,
                "action_total_ns": action_total_ns, "process_cpu_ns": cpu_ns,
                "dirty_before": dirty, "returned_count": returned,
            })
        emit({"kind": "summary", "shapes": shape_summary(doc), "completed": True})
    app.closeDocument(doc.Name)


if __name__ == "__main__":
    main()
