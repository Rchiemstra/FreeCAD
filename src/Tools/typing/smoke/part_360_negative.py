import Part
from FreeCAD import Vector

poles = (Vector(), Vector())
Part.BSplineCurve('bad')
Part.BSplineCurve(poles, periodic=1)
Part.BSplineCurve(poles, degree='bad')
Part.BSplineCurve(poles, CheckRational=None)
Part.BSplineCurve(poles, weights=['bad'])
Part.BSplineCurve(poles, unknown=True)
Part.BSplineCurve(poles, poles=poles)
Part.BSplineCurve(poles, None, None, False, 3, None, True, 1)
Part.BSplineCurve([Vector(), 'bad'])
Part.BSplineCurve([(0, 'bad', 0)])
