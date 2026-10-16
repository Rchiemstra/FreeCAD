// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2022 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include <Mod/Part/PartGlobal.h>

#include <Standard_Failure.hxx>
#include <BRep_Tool.hxx>
#include <TopTools_SequenceOfShape.hxx>
#include <TopoDS.hxx>


#include "ShapeFix/ShapeFix_FacePy.h"
#include "ShapeFix/ShapeFix_FacePy.cpp"
#include "ShapeFix/ShapeFix_WirePy.h"
#include <Mod/Part/App/GeometrySurfacePy.h>
#include <Mod/Part/App/OCCError.h>
#include <Mod/Part/App/TopoShapeFacePy.h>
#include <Mod/Part/App/TopoShapeWirePy.h>


using namespace Part;

namespace
{

const char* shapeFixFaceNeedsSurface = "ShapeFix.Face requires a face with a surface";

bool faceHasSurface(PyObject* face)
{
    const TopoDS_Shape& shape = static_cast<TopoShapePy*>(face)->getTopoShapePtr()->getShape();
    return !shape.IsNull() && !BRep_Tool::Surface(TopoDS::Face(shape)).IsNull();
}

// Translate even in debug builds: the generated wrappers omit catch(...) there.
void translateFaceException()
{
    try {
        throw;
    }
    catch (const Standard_Failure& error) {
        const char* message = error.GetMessageString();
        PyErr_SetString(PartExceptionOCCError, message ? message : "OCCT failure in ShapeFix.Face");
    }
    catch (const Base::Exception& error) {
        error.setPyException();
    }
    catch (const Py::Exception&) {
        // The Python error is already set.
    }
    catch (const std::exception& error) {
        PyErr_SetString(PyExc_RuntimeError, error.what());
    }
    catch (...) {
        PyErr_SetString(PyExc_RuntimeError, "Unknown C++ exception in ShapeFix.Face");
    }
}

void requireFaceSurface(const ShapeFix_Face& tool)
{
    if (tool.Face().IsNull() || BRep_Tool::Surface(tool.Face()).IsNull()) {
        throw Base::ValueError(shapeFixFaceNeedsSurface);
    }
}

}  // namespace

// returns a string which represents the object e.g. when printed in python
std::string ShapeFix_FacePy::representation() const
{
    return "<ShapeFix_Face object>";
}

PyObject* ShapeFix_FacePy::PyMake(struct _typeobject*, PyObject*, PyObject*)  // Python wrapper
{
    // create a new instance of ShapeFix_FacePy
    return new ShapeFix_FacePy(nullptr);
}

// constructor method
int ShapeFix_FacePy::PyInit(PyObject* args, PyObject* /*kwds*/)
try
{
    PyObject* face = nullptr;
    if (PyArg_ParseTuple(args, "|O!", &TopoShapeFacePy::Type, &face)) {
        setHandle(new ShapeFix_Face());
        if (face) {
            // A null face (Part.Face()) has no TShape. ShapeFix_Face::Init calls
            // BRep_Tool::Surface, which segfaults instead of throwing.
            if (!faceHasSurface(face)) {
                PyErr_SetString(PyExc_ValueError, shapeFixFaceNeedsSurface);
                return -1;
            }
            getShapeFix_FacePtr()->Init(
                TopoDS::Face(static_cast<TopoShapePy*>(face)->getTopoShapePtr()->getShape())
            );
        }

        return 0;
    }

    PyErr_Clear();
    double prec;
    PyObject* fwd = Py_True;
    if (PyArg_ParseTuple(args, "O!d|O!", &GeometrySurfacePy::Type, &face, &prec, &PyBool_Type, &fwd)) {
        setHandle(new ShapeFix_Face());
        if (face) {
            Handle(Geom_Surface) surf = Handle(Geom_Surface)::DownCast(
                static_cast<GeometrySurfacePy*>(face)->getGeomSurfacePtr()->handle()
            );
            if (surf.IsNull()) {
                throw Base::ValueError("ShapeFix.Face requires a non-null surface");
            }
            getShapeFix_FacePtr()->Init(surf, prec, Base::asBoolean(fwd));
        }

        return 0;
    }

    PyErr_SetString(
        PyExc_TypeError,
        "Supported arguments are:\n"
        "-- Face\n"
        "-- Surface, Precision, [Forward=True]\n"
        "   Precision is a Float\n"
        "   If Forward is the orientation will be FORWARD or REVERSED otherwise"
    );
    return -1;
}
catch (...) {
    translateFaceException();
    return -1;
}

PyObject* ShapeFix_FacePy::init(PyObject* args)
try
{
    PyObject* face;
    if (PyArg_ParseTuple(args, "O!", &TopoShapeFacePy::Type, &face)) {
        if (!faceHasSurface(face)) {
            PyErr_SetString(PyExc_ValueError, shapeFixFaceNeedsSurface);
            return nullptr;
        }
        getShapeFix_FacePtr()->Init(
            TopoDS::Face(static_cast<TopoShapePy*>(face)->getTopoShapePtr()->getShape())
        );
        Py_Return;
    }

    PyErr_Clear();
    double prec;
    PyObject* fwd = Py_True;
    if (PyArg_ParseTuple(args, "O!d|O!", &GeometrySurfacePy::Type, &face, &prec, &PyBool_Type, &fwd)) {
        if (face) {
            Handle(Geom_Surface) surf = Handle(Geom_Surface)::DownCast(
                static_cast<GeometrySurfacePy*>(face)->getGeomSurfacePtr()->handle()
            );
            if (surf.IsNull()) {
                throw Base::ValueError("ShapeFix.Face requires a non-null surface");
            }
            getShapeFix_FacePtr()->Init(surf, prec, Base::asBoolean(fwd));
        }

        Py_Return;
    }

    PyErr_SetString(
        PyExc_TypeError,
        "Supported arguments are:\n"
        "-- Face\n"
        "-- Surface, Precision, [Forward=True]\n"
        "   Precision is a Float\n"
        "   If Forward is the orientation will be FORWARD or REVERSED otherwise"
    );
    return nullptr;
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::fixWireTool(PyObject* args)
try
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    Handle(ShapeFix_Wire) tool = getShapeFix_FacePtr()->FixWireTool();
    ShapeFix_WirePy* wire = new ShapeFix_WirePy(nullptr);
    wire->setHandle(tool);
    return wire;
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::clearModes(PyObject* args)
try
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    getShapeFix_FacePtr()->ClearModes();
    Py_Return;
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::add(PyObject* args)
try
{
    PyObject* wire;
    if (!PyArg_ParseTuple(args, "O!", &TopoShapeWirePy::Type, &wire)) {
        return nullptr;
    }

    const TopoDS_Shape& wireShape =
        static_cast<TopoShapePy*>(wire)->getTopoShapePtr()->getShape();
    // ShapeFix_Face::Add uses TopoDS_Builder::Add, which segfaults when the
    // tool was never given a face (the default constructor, or Init failed).
    if (wireShape.IsNull()) {
        throw Base::ValueError("ShapeFix.Face.add requires a non-null wire");
    }
    requireFaceSurface(*getShapeFix_FacePtr());
    getShapeFix_FacePtr()->Add(TopoDS::Wire(wireShape));
    Py_Return;
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::fixOrientation(PyObject* args)
try
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    requireFaceSurface(*getShapeFix_FacePtr());
    Standard_Boolean ok = getShapeFix_FacePtr()->FixOrientation();
    return Py::new_reference_to(Py::Boolean(ok));
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::fixAddNaturalBound(PyObject* args)
try
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    requireFaceSurface(*getShapeFix_FacePtr());
    Standard_Boolean ok = getShapeFix_FacePtr()->FixAddNaturalBound();
    return Py::new_reference_to(Py::Boolean(ok));
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::fixMissingSeam(PyObject* args)
try
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    requireFaceSurface(*getShapeFix_FacePtr());
    Standard_Boolean ok = getShapeFix_FacePtr()->FixMissingSeam();
    return Py::new_reference_to(Py::Boolean(ok));
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::fixSmallAreaWire(PyObject* args)
try
{
    PyObject* removeSmall;
    if (!PyArg_ParseTuple(args, "O!", &PyBool_Type, &removeSmall)) {
        return nullptr;
    }

    requireFaceSurface(*getShapeFix_FacePtr());
    Standard_Boolean ok = getShapeFix_FacePtr()->FixSmallAreaWire(Base::asBoolean(removeSmall));
    return Py::new_reference_to(Py::Boolean(ok));
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::fixLoopWire(PyObject* args)
try
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    TopTools_SequenceOfShape aResWires;
    requireFaceSurface(*getShapeFix_FacePtr());
    Standard_Boolean ok = getShapeFix_FacePtr()->FixLoopWire(aResWires);
    Py::List list;
    for (int index = aResWires.Lower(); index <= aResWires.Upper(); index++) {
        TopoShape sh = aResWires(index);
        list.append(Py::asObject(sh.getPyObject()));
    }
    return Py::new_reference_to(Py::TupleN(Py::Boolean(ok), list));
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::fixIntersectingWires(PyObject* args)
try
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    requireFaceSurface(*getShapeFix_FacePtr());
    Standard_Boolean ok = getShapeFix_FacePtr()->FixIntersectingWires();
    return Py::new_reference_to(Py::Boolean(ok));
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::fixWiresTwoCoincidentEdges(PyObject* args)
try
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    requireFaceSurface(*getShapeFix_FacePtr());
    Standard_Boolean ok = getShapeFix_FacePtr()->FixWiresTwoCoincEdges();
    return Py::new_reference_to(Py::Boolean(ok));
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::fixPeriodicDegenerated(PyObject* args)
try
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    requireFaceSurface(*getShapeFix_FacePtr());
    Standard_Boolean ok = getShapeFix_FacePtr()->FixPeriodicDegenerated();
    return Py::new_reference_to(Py::Boolean(ok));
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::perform(PyObject* args)
try
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    requireFaceSurface(*getShapeFix_FacePtr());
    Standard_Boolean ok = getShapeFix_FacePtr()->Perform();
    return Py::new_reference_to(Py::Boolean(ok));
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::face(PyObject* args)
try
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    TopoShape shape = getShapeFix_FacePtr()->Face();
    return shape.getPyObject();
}
catch (...) {
    translateFaceException();
    return nullptr;
}

PyObject* ShapeFix_FacePy::result(PyObject* args)
try
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    TopoShape shape = getShapeFix_FacePtr()->Result();
    return shape.getPyObject();
}
catch (...) {
    translateFaceException();
    return nullptr;
}

Py::Boolean ShapeFix_FacePy::getFixWireMode() const
try
{
    return Py::Boolean(getShapeFix_FacePtr()->FixWireMode());
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

void ShapeFix_FacePy::setFixWireMode(Py::Boolean arg)
try
{
    getShapeFix_FacePtr()->FixWireMode() = arg;
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

Py::Boolean ShapeFix_FacePy::getFixOrientationMode() const
try
{
    return Py::Boolean(getShapeFix_FacePtr()->FixOrientationMode());
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

void ShapeFix_FacePy::setFixOrientationMode(Py::Boolean arg)
try
{
    getShapeFix_FacePtr()->FixOrientationMode() = arg;
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

Py::Boolean ShapeFix_FacePy::getFixAddNaturalBoundMode() const
try
{
    return Py::Boolean(getShapeFix_FacePtr()->FixAddNaturalBoundMode());
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

void ShapeFix_FacePy::setFixAddNaturalBoundMode(Py::Boolean arg)
try
{
    getShapeFix_FacePtr()->FixAddNaturalBoundMode() = arg;
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

Py::Boolean ShapeFix_FacePy::getFixMissingSeamMode() const
try
{
    return Py::Boolean(getShapeFix_FacePtr()->FixMissingSeamMode());
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

void ShapeFix_FacePy::setFixMissingSeamMode(Py::Boolean arg)
try
{
    getShapeFix_FacePtr()->FixMissingSeamMode() = arg;
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

Py::Boolean ShapeFix_FacePy::getFixSmallAreaWireMode() const
try
{
    return Py::Boolean(getShapeFix_FacePtr()->FixSmallAreaWireMode());
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

void ShapeFix_FacePy::setFixSmallAreaWireMode(Py::Boolean arg)
try
{
    getShapeFix_FacePtr()->FixSmallAreaWireMode() = arg;
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

Py::Boolean ShapeFix_FacePy::getRemoveSmallAreaFaceMode() const
try
{
    return Py::Boolean(getShapeFix_FacePtr()->RemoveSmallAreaFaceMode());
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

void ShapeFix_FacePy::setRemoveSmallAreaFaceMode(Py::Boolean arg)
try
{
    getShapeFix_FacePtr()->RemoveSmallAreaFaceMode() = arg;
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

Py::Boolean ShapeFix_FacePy::getFixIntersectingWiresMode() const
try
{
    return Py::Boolean(getShapeFix_FacePtr()->FixIntersectingWiresMode());
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

void ShapeFix_FacePy::setFixIntersectingWiresMode(Py::Boolean arg)
try
{
    getShapeFix_FacePtr()->FixIntersectingWiresMode() = arg;
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

Py::Boolean ShapeFix_FacePy::getFixLoopWiresMode() const
try
{
    return Py::Boolean(getShapeFix_FacePtr()->FixLoopWiresMode());
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

void ShapeFix_FacePy::setFixLoopWiresMode(Py::Boolean arg)
try
{
    getShapeFix_FacePtr()->FixLoopWiresMode() = arg;
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

Py::Boolean ShapeFix_FacePy::getFixSplitFaceMode() const
try
{
    return Py::Boolean(getShapeFix_FacePtr()->FixSplitFaceMode());
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

void ShapeFix_FacePy::setFixSplitFaceMode(Py::Boolean arg)
try
{
    getShapeFix_FacePtr()->FixSplitFaceMode() = arg;
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

Py::Boolean ShapeFix_FacePy::getAutoCorrectPrecisionMode() const
try
{
    return Py::Boolean(getShapeFix_FacePtr()->AutoCorrectPrecisionMode());
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

void ShapeFix_FacePy::setAutoCorrectPrecisionMode(Py::Boolean arg)
try
{
    getShapeFix_FacePtr()->AutoCorrectPrecisionMode() = arg;
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

Py::Boolean ShapeFix_FacePy::getFixPeriodicDegeneratedMode() const
try
{
    return Py::Boolean(getShapeFix_FacePtr()->FixPeriodicDegeneratedMode());
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

void ShapeFix_FacePy::setFixPeriodicDegeneratedMode(Py::Boolean arg)
try
{
    getShapeFix_FacePtr()->FixPeriodicDegeneratedMode() = arg;
}
catch (...) {
    translateFaceException();
    throw Py::Exception();
}

PyObject* ShapeFix_FacePy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int ShapeFix_FacePy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
