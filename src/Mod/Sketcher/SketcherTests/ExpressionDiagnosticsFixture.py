# SPDX-License-Identifier: LGPL-2.1-or-later
"""The deleted-VarSet box from FreeCAD/FreeCAD#26496."""
import FreeCAD as App
import Part
import Sketcher


def make_box(doc):
    variables = doc.addObject("App::VarSet", "Dimensions")
    for name, value in (("Width", 20), ("Height", 10)):
        variables.addProperty("App::PropertyLength", name)
        setattr(variables, name, value)
    sketch = doc.addObject("Sketcher::SketchObject", "Sketch")
    points = [App.Vector(x, y, 0) for x, y in ((0, 0), (20, 0), (20, 10), (0, 10))]
    for i in range(4):
        sketch.addGeometry(Part.LineSegment(points[i], points[(i + 1) % 4]), False)
    for i in range(4):
        sketch.addConstraint(Sketcher.Constraint("Coincident", i, 2, (i + 1) % 4, 1))
        sketch.addConstraint(Sketcher.Constraint("Horizontal" if i % 2 == 0 else "Vertical", i))
    for name, edge, value in (("Width", 0, 20), ("Height", 1, 10)):
        index = sketch.addConstraint(Sketcher.Constraint("Distance", edge, value))
        sketch.renameConstraint(index, name)
        sketch.setExpression("Constraints." + name, "Dimensions." + name)
    return sketch
