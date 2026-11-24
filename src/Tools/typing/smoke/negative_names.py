import Mesh
import Sketcher

def rejected(sketch: Sketcher.SketchObject, mesh: Mesh.Mesh) -> None:
    sketch.addGeometry(42)
    mesh.cut(["not a vector"], 0)
