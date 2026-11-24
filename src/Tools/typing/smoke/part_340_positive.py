from typing import cast
import Part
from FreeCAD import Vector
curve = cast(Part.BSplineCurve, None)
poles: list[Vector] = [Vector(), Vector()]
curve.interpolate(poles, True, 1e-6, Vector(), Vector(), poles, [True, False], [0.0, 1.0, 2.0], False)
curve.interpolate(Points=poles, PeriodicFlag=True, Tolerance=1e-6, InitialTangent=Vector(), FinalTangent=Vector(), Tangents=poles, TangentFlags=[True, False], Parameters=[0.0, 1.0, 2.0], Scale=False)
curve.buildFromPolesMultsKnots(poles, [2, 2], [0.0, 1.0], False, 1, [1.0, 1.0], True)
curve.buildFromPolesMultsKnots(poles=poles, mults=[2, 2], knots=[0.0, 1.0], periodic=False, degree=1, weights=[1.0, 1.0], CheckRational=True)
curve.interpolate(poles)
curve.buildFromPolesMultsKnots(poles)
surface = cast(Part.GeometrySurface, None)
surface.toBSpline(UContinuity='custom')
surface.toBSpline(1e-7, 'C2', 'G1')
ellipse = cast(Part.Ellipse, None)
Part.ArcOfEllipse(ellipse, 0.0, 1.0)
Part.SurfaceOfRevolution(curve, Vector(), Vector())
curve.interpolate((Vector(0, 0, 0), Vector(1, 0, 0)))
curve.buildFromPolesMultsKnots((Vector(0, 0, 0), Vector(1, 0, 0)), (2, 2), (0.0, 1.0), degree=1)

# Coordinate sequences use the same native conversion as Vector items.
curve.interpolate([(0, 0, 0), (1, 0, 0)])
curve.interpolate([[0, 0, 0], [1, 0, 0]], Tangents=[(1, 0, 0), [1, 0, 0]], TangentFlags=[True, 'truthy'])
# The stored TestPartApp.py setup uses these poles, multiplicities, and knots.
curve.buildFromPolesMultsKnots([[0, 0, 0], [1, 1, 0], [2, 0, 0], [1, -1, 0]], (3, 1, 3), (0, 0.5, 1), False, 2)
curve.buildFromPolesMultsKnots(poles, None, None, weights=None)
