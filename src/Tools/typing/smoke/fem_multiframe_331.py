from Fem import FemPostPipeline
from FreeCAD import DocumentObject
from FreeCAD.Base import Unit

def supported(pipeline: FemPostPipeline, unit: Unit, a: DocumentObject, b: DocumentObject) -> None:
    pipeline.read(("one.vtk", "two.vtk"), (0.5, 1.5), unit, "time")
    pipeline.read(["one.vtk", "two.vtk"], [0.5, 1.5], unit, "time")
    pipeline.load((a, b), (0.5, 1.5), unit, "time")
    pipeline.load([a, b], [0.5, 1.5], unit, "time")
