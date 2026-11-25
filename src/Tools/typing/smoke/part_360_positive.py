import Part
from FreeCAD import Vector

poles = (Vector(), Vector())
Part.BSplineCurve()
# PyInit ignores all keywords when the positional tuple is empty.
Part.BSplineCurve(poles='ignored', arbitrary=object())
Part.BSplineCurve(poles)
Part.BSplineCurve(poles, (2, 2), (0.0, 1.0), False, 1, (1.0, 1.0), True)
Part.BSplineCurve(poles, mults=None, knots=None, periodic=False, degree=3, weights=None, CheckRational=True)
Part.BSplineCurve(poles, degree=1)

# TestPartApp.py's stored poles, multiplicities and knots, via the constructor.
Part.BSplineCurve([[0, 0, 0], [1, 1, 0], [2, 0, 0], [1, -1, 0]], (3, 1, 3), (0, 0.5, 1), False, 2)
Part.BSplineCurve(((0, 0, 0), Vector(), [1, 1, 0]), mults=(3, 3), knots=(0, 1), degree=2)
