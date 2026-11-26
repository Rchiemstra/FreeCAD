from Fem import FemPostPipeline
from FreeCAD.Base import Unit

def rejected(pipeline: FemPostPipeline, unit: Unit) -> None:
    pipeline.read(("one.vtk", "two.vtk"), ("bad", "values"), unit, "time")
