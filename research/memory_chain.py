"""Executed inside FreeCAD; configuration and output paths come from the launcher."""

import ctypes
import json
import os
from pathlib import Path
import time
import traceback

import FreeCAD as App
import Part


def snapshot(stage, **extra):
    status = dict(line.split(":", 1) for line in Path("/proc/self/status").read_text().splitlines())

    class MallInfo(ctypes.Structure):
        _fields_ = [
            (name, ctypes.c_size_t)
            for name in (
                "arena",
                "ordblks",
                "smblks",
                "hblks",
                "hblkhd",
                "usmblks",
                "fsmblks",
                "uordblks",
                "fordblks",
                "keepcost",
            )
        ]

    libc = ctypes.CDLL(None)
    libc.mallinfo2.restype = MallInfo
    info = libc.mallinfo2()
    row = dict(
        stage=stage,
        elapsed=time.monotonic() - started,
        rss_kib=int(status["VmRSS"].split()[0]),
        hwm_kib=int(status["VmHWM"].split()[0]),
        heap_bytes=info.uordblks + info.hblkhd,
        **extra,
    )
    with open(os.environ["CHAIN_RESULT"], "a") as stream:
        stream.write(json.dumps(row) + "\n")


def names(shape):
    mapping = shape.ElementMap
    return dict(name_bytes=sum(map(len, mapping)), max_name=max(map(len, mapping), default=0))


started = time.monotonic()
try:
    cfg = json.loads(os.environ["CHAIN_CONFIG"])
    snapshot("startup", config=cfg)
    d = App.newDocument("memory_chain")
    if "undo" in cfg:
        d.UndoMode = cfg["undo"]
    if "hasher" in cfg:
        d.UseHasher = cfg["hasher"]
    body = d.addObject("PartDesign::Body", "Body")
    step = {"overlap": 1.8, "touch": 2.0, "separate": 2.2}[cfg["spacing"]]
    features = []
    for i in range(cfg["n"]):
        if cfg["feature"] == "box":
            o = body.newObject("PartDesign::AdditiveBox", "AdditiveBox")
            o.Length = 10
            o.Width = 10
            o.Height = 2
            o.Placement.Base = App.Vector(0, 0, i * step)
        else:
            sketch = body.newObject("Sketcher::SketchObject", "Sketch")
            points = [(0, 0), (10, 0), (10, 10), (0, 10)]
            for a, b in zip(points, points[1:] + points[:1]):
                sketch.addGeometry(Part.LineSegment(App.Vector(*a, 0), App.Vector(*b, 0)), False)
            sketch.Placement.Base = App.Vector(0, 0, i * step)
            o = body.newObject("PartDesign::Pad", "Pad")
            o.Profile = sketch
            o.Length = 2
        o.Refine = cfg["refine"]
        features.append(o)
        if cfg["each"]:
            d.recompute()
            snapshot(
                "feature",
                index=i + 1,
                elements=o.Shape.ElementMapSize,
                faces=len(o.Shape.Faces),
                state=o.State,
                **(names(o.Shape) if i < 15 else {}),
            )
            if "Invalid" in o.State:
                raise RuntimeError(f"Feature {i + 1} failed; stop before extending the chain")
            if cfg.get("drop_maps"):
                # Diagnostic only: destroys naming history, never a product fix.
                o.Shape = o.Shape.copy(noElementMap=True)
    snapshot("created", undo=d.UndoMode, hasher=d.UseHasher)
    d.recompute()
    snapshot(
        "recomputed",
        maps=[o.Shape.ElementMapSize for o in features],
        faces=[len(o.Shape.Faces) for o in features],
        states=[o.State for o in features],
        shape_hashers=[o.Shape.Hasher is not None for o in features],
    )
    # Avoid copying enormous uncompressed names into Python at the cap.
    snapshot("naming", names=[names(o.Shape) for o in features[:15]])
    if App.GuiUp:
        import FreeCADGui as Gui

        Gui.updateGui()
        snapshot("gui_updated")
    App.closeDocument(d.Name)
    snapshot("closed")
except BaseException:
    snapshot("error", error=traceback.format_exc())
finally:
    os._exit(0)
