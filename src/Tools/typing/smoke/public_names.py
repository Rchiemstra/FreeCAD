from typing_extensions import assert_type
import FreeCAD
import Mesh
import Part
import Robot
import Sketcher
import TechDraw


def supported(sketch: Sketcher.SketchObject, geo: Part.Geometry, mesh: Mesh.Mesh,
              vector: FreeCAD.Vector, edge: TechDraw.CosmeticEdge,
              robot: Robot.RobotObject) -> None:
    assert_type(sketch.addGeometry(geo), int)
    assert_type(sketch.addGeometry([geo]), tuple[int, ...])
    mesh.cut([vector, (1.0, 2.0, 3.0)], 0)
    assert_type(edge.Start, FreeCAD.Vector)
    assert_type(robot.Base, FreeCAD.Placement)
