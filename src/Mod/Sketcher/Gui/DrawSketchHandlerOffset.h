// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2022 Boyer Pierre-Louis <pierrelouis.boyer@gmail.com>   *
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


#pragma once

#include <FCConfig.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

#include <QApplication>
#include <QStringList>

#include <BRep_Tool.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <ShapeFix_Wire.hxx>
#include <Mod/Part/App/BRepOffsetAPI_MakeOffsetFix.h>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <Base/Console.h>
#include <Base/Exception.h>

#include <Gui/BitmapFactory.h>
#include <Gui/Notifications.h>
#include <Gui/Command.h>
#include <Gui/CommandT.h>

#include <Mod/Sketcher/App/SketchObject.h>

#include <Mod/Sketcher/App/GeometryFacade.h>

#include "DrawSketchDefaultWidgetController.h"
#include "DrawSketchControllableHandler.h"

#include "GeometryCreationMode.h"
#include "Utils.h"


using namespace Sketcher;

namespace SketcherGui
{

extern GeometryCreationMode geometryCreationMode;  // defined in CommandCreateGeo.cpp

class DrawSketchHandlerOffset;

namespace ConstructionMethods
{

enum class OffsetConstructionMethod
{
    Arc,
    // Tangent,
    Intersection,
    ConstrainedClearance,
    End  // Must be the last one
};

/* OCC offer various modes as follows, but we use only Arc and Intersection as the rest are buggy.
enum class JoinMode {
    Arc,
    Tangent,
    Intersection
};
//We use Pipe by default only. Skin is buggy.
enum class ModeEnums {
    Skin,
    Pipe,
    RectoVerso
};*/
}  // namespace ConstructionMethods

using DSHOffsetController = DrawSketchDefaultWidgetController<
    DrawSketchHandlerOffset,
    StateMachines::OneSeekEnd,
    /*PAutoConstraintSize =*/0,
    /*OnViewParametersT =*/OnViewParameters<1, 1, 1>,
    /*WidgetParametersT =*/WidgetParameters<0, 0, 0>,
    /*WidgetCheckboxesT =*/WidgetCheckboxes<3, 3, 3>,
    /*WidgetComboboxesT =*/WidgetComboboxes<1, 1, 1>,
    /*WidgetLineEditsT =*/WidgetLineEdits<0, 0, 0>,
    ConstructionMethods::OffsetConstructionMethod,
    /*bool PFirstComboboxIsConstructionMethod =*/true>;

using DSHOffsetControllerBase = DSHOffsetController::ControllerBase;

using DrawSketchHandlerOffsetBase = DrawSketchControllableHandler<DSHOffsetController>;

class DrawSketchHandlerOffset: public DrawSketchHandlerOffsetBase
{
    Q_DECLARE_TR_FUNCTIONS(SketcherGui::DrawSketchHandlerOffset)

    friend DSHOffsetController;
    friend DSHOffsetControllerBase;

public:
    DrawSketchHandlerOffset(
        std::vector<int> listOfGeoIds,
        ConstructionMethod constrMethod = ConstructionMethod::Arc
    )
        : DrawSketchHandlerOffsetBase(constrMethod)
        , selectedGeoIds(listOfGeoIds)
        , listOfGeoIds(listOfGeoIds)
        , deleteOriginal(false)
        , offsetLengthSet(false)
        , offsetConstraint(false)
        , onlySingleLines(true)
        , chainLink(false)
        , offsetLength(1.)
    {}

    ~DrawSketchHandlerOffset() override = default;


private:
    void updateDataAndDrawToPosition(Base::Vector2d onSketchPos) override
    {
        if (state() == SelectMode::SeekFirst) {
            endpoint = onSketchPos;

            if (!offsetLengthSet) {
                findOffsetLength();
                toolWidgetManager.drawDoubleAtCursor(onSketchPos, offsetLength);
            }

            if (fabs(offsetLength) > Precision::Confusion()) {
                drawOffsetPreview();
            }
        }
    }

    void executeCommands() override
    {
        if (fabs(offsetLength) > Precision::Confusion()) {
            createOffset();
        }
    }

    std::string getToolName() const override
    {
        return "DSH_Offset";
    }

    QString getCrosshairCursorSVGName() const override
    {
        return QStringLiteral("Sketcher_Pointer_Create_Offset");
    }

    std::unique_ptr<QWidget> createWidget() const override
    {
        return std::make_unique<SketcherToolDefaultWidget>();
    }

    bool isWidgetVisible() const override
    {
        return true;
    };

    QPixmap getToolIcon() const override
    {
        return Gui::BitmapFactory().pixmap("Sketcher_Offset");
    }

    QString getToolWidgetText() const override
    {
        return QString(tr("Offset Parameters"));
    }

    void activated() override
    {
        DrawSketchDefaultHandler::activated();
        continuousMode = false;
        firstCurveCreated = getHighestCurveIndex() + 1;

        refreshSourceGeometry();
    }

public:
    std::list<Gui::InputHint> getToolHints() const override
    {
        using enum Gui::InputHint::UserInput;

        return {
            {tr("%1 set offset direction and distance", "Sketcher Offset: hint"), {MouseLeft}},
        };
    }

private:
    class CoincidencePointPos
    {
    public:
        PointPos firstPos1;
        PointPos secondPos1;
        PointPos firstPos2;
        PointPos secondPos2;
    };

    std::vector<int> selectedGeoIds;
    std::vector<int> listOfGeoIds;
    std::vector<std::vector<int>> vCC;
    std::vector<std::vector<int>> vCCO;
    Base::Vector2d endpoint, pointOnSourceWire;
    std::vector<TopoDS_Wire> sourceWires;

    bool deleteOriginal, offsetLengthSet, offsetConstraint, onlySingleLines, chainLink;
    double offsetLength;
    int firstCurveCreated;
    std::set<std::tuple<std::string, int, int, int, int>> constraintDedupKeys;
    std::vector<int> offsetWireOrderedGeoIds;
    std::vector<int> offsetConnectivityChain;

    bool isConstrainedClearanceMode() const
    {
        return constructionMethod() == ConstructionMethod::ConstrainedClearance;
    }

    static std::tuple<std::string, int, int, int, int> makeConstraintKey(
        const std::string& type,
        int geo1,
        int pos1,
        int geo2,
        int pos2
    )
    {
        if (type == "Parallel" || type == "Equal" || type == "Coincident" || type == "Tangent"
            || type == "Distance" || type == "DistanceX" || type == "DistanceY") {
            if (geo1 > geo2 || (geo1 == geo2 && pos1 > pos2)) {
                std::swap(geo1, geo2);
                std::swap(pos1, pos2);
            }
        }

        return {type, geo1, pos1, geo2, pos2};
    }

    bool tryAddConstraint(
        std::stringstream& stream,
        const std::string& type,
        int geo1,
        int pos1,
        int geo2,
        int pos2,
        const std::string& constraintExpression
    )
    {
        if (!constraintDedupKeys.insert(makeConstraintKey(type, geo1, pos1, geo2, pos2)).second) {
            return false;
        }

        stream << "conList.append(" << constraintExpression << ")\n";
        return true;
    }

    void executeConstraintScript(std::stringstream& stream)
    {
        const std::string script = stream.str();
        if (script == "conList = []\n") {
            return;
        }

        stream << Gui::Command::getObjectCmd(sketchgui->getObject()) << ".addConstraint(conList)\n";
        stream << "del conList\n";
        Gui::Command::doCommand(Gui::Command::Doc, stream.str().c_str());
    }

    bool findSharedEndpoints(
        int geoId1,
        int geoId2,
        PointPos& pos1,
        PointPos& pos2
    )
    {
        Base::Vector3d firstStartPoint;
        Base::Vector3d firstEndPoint;
        Base::Vector3d secondStartPoint;
        Base::Vector3d secondEndPoint;
        if (!getFirstSecondPoints(geoId1, firstStartPoint, firstEndPoint)
            || !getFirstSecondPoints(geoId2, secondStartPoint, secondEndPoint)) {
            return false;
        }

        if ((firstStartPoint - secondStartPoint).Length() < Precision::Confusion()) {
            pos1 = PointPos::start;
            pos2 = PointPos::start;
            return true;
        }
        if ((firstStartPoint - secondEndPoint).Length() < Precision::Confusion()) {
            pos1 = PointPos::start;
            pos2 = PointPos::end;
            return true;
        }
        if ((firstEndPoint - secondStartPoint).Length() < Precision::Confusion()) {
            pos1 = PointPos::end;
            pos2 = PointPos::start;
            return true;
        }
        if ((firstEndPoint - secondEndPoint).Length() < Precision::Confusion()) {
            pos1 = PointPos::end;
            pos2 = PointPos::end;
            return true;
        }

        return false;
    }

    void appendJunctionConstraint(
        std::stringstream& stream,
        int geoId1,
        int geoId2,
        PointPos pos1,
        PointPos pos2
    )
    {
        if (geoId1 == geoId2) {
            return;
        }

        SketchObject* Obj = sketchgui->getSketchObject();
        const Part::Geometry* geo1 = Obj->getGeometry(geoId1);
        const Part::Geometry* geo2 = Obj->getGeometry(geoId2);
        if (!geo1 || !geo2 || isGeometryDegenerate(geo1) || isGeometryDegenerate(geo2)) {
            return;
        }

        const bool tangent = needTangent(geoId1, geoId2, pos1, pos2);
        const std::string constraintType = tangent ? "Tangent" : "Coincident";
        std::ostringstream expression;
        expression << "Sketcher.Constraint('" << constraintType << "'," << geoId1 << ","
                   << static_cast<int>(pos1) << ", " << geoId2 << ","
                   << static_cast<int>(pos2) << ")";
        tryAddConstraint(stream, constraintType, geoId1, static_cast<int>(pos1), geoId2,
                         static_cast<int>(pos2), expression.str());
    }

    bool hasTangentConstraintAtEndpoint(int geoId, PointPos pos)
    {
        SketchObject* Obj = sketchgui->getSketchObject();
        const std::vector<Constraint*>& vals = Obj->Constraints.getValues();
        for (const auto* cstr : vals) {
            if (cstr->Type != Tangent) {
                continue;
            }

            if ((cstr->First == geoId && cstr->FirstPos == pos)
                || (cstr->Second == geoId && cstr->SecondPos == pos)) {
                return true;
            }
        }

        return false;
    }

    bool arcHasTangentJunctionAtBothEnds(int arcGeoId)
    {
        return hasTangentConstraintAtEndpoint(arcGeoId, PointPos::start)
            && hasTangentConstraintAtEndpoint(arcGeoId, PointPos::end);
    }

    bool getGeometryEndpoints(
        const Part::Geometry* geo,
        Base::Vector3d& startPoint,
        Base::Vector3d& endPoint
    ) const
    {
        if (!geo) {
            return false;
        }

        if (isLineSegment(*geo)) {
            const auto* line = static_cast<const Part::GeomLineSegment*>(geo);
            startPoint = line->getStartPoint();
            endPoint = line->getEndPoint();
            return true;
        }

        if (isArcOfCircle(*geo) || isArcOfEllipse(*geo) || isArcOfHyperbola(*geo)
            || isArcOfParabola(*geo)) {
            const auto* arcOfConic = static_cast<const Part::GeomArcOfConic*>(geo);
            startPoint = arcOfConic->getStartPoint(true);
            endPoint = arcOfConic->getEndPoint(true);
            return true;
        }

        if (isBSplineCurve(*geo)) {
            const auto* bSpline = static_cast<const Part::GeomBSplineCurve*>(geo);
            startPoint = bSpline->getStartPoint();
            endPoint = bSpline->getEndPoint();
            return true;
        }

        return false;
    }

    bool orientGeometryToStartAt(Part::Geometry* geo, const Base::Vector3d& requiredStart)
    {
        Base::Vector3d startPoint;
        Base::Vector3d endPoint;
        if (!getGeometryEndpoints(geo, startPoint, endPoint)) {
            return false;
        }

        if (pointsCoincident(startPoint, requiredStart)) {
            return true;
        }

        if (pointsCoincident(endPoint, requiredStart)) {
            if (isLineSegment(*geo)) {
                auto* line = static_cast<Part::GeomLineSegment*>(geo);
                line->setPoints(endPoint, startPoint);
            }
            else {
                geo->reverseIfReversed();
            }
            return true;
        }

        return false;
    }

    void collectOrderedWireEdges(const TopoDS_Shape& shape, std::vector<TopoDS_Wire>& wires)
    {
        if (shape.IsNull()) {
            return;
        }

        if (shape.ShapeType() == TopAbs_WIRE) {
            wires.push_back(TopoDS::Wire(shape));
            return;
        }

        if (shape.ShapeType() == TopAbs_EDGE) {
            BRepBuilderAPI_MakeWire mkWire(TopoDS::Edge(shape));
            if (mkWire.IsDone()) {
                wires.push_back(mkWire.Wire());
            }
            return;
        }

        if (shape.ShapeType() == TopAbs_COMPOUND) {
            for (TopExp_Explorer explorer(shape, TopAbs_WIRE); explorer.More(); explorer.Next()) {
                wires.push_back(TopoDS::Wire(explorer.Current()));
            }
        }
    }

    std::unique_ptr<Part::Geometry> edgeToGeometry(const TopoDS_Edge& edge)
    {
        BRepAdaptor_Curve curve(edge);
        if (curve.GetType() == GeomAbs_Line) {
            return std::unique_ptr<Part::Geometry>(curveToLine(curve));
        }
        if (curve.GetType() == GeomAbs_Circle) {
            return std::unique_ptr<Part::Geometry>(curveToCircleOrArc(curve, edge));
        }
        if (curve.GetType() == GeomAbs_Ellipse) {
            return std::unique_ptr<Part::Geometry>(curveToEllipseOrArc(curve, edge));
        }

        return nullptr;
    }

    bool isGeometryDegenerate(const Part::Geometry* geo) const
    {
        Base::Vector3d startPoint;
        Base::Vector3d endPoint;
        if (!getGeometryEndpoints(geo, startPoint, endPoint)) {
            return false;
        }

        return pointsCoincident(startPoint, endPoint);
    }

    bool findDirectedChainJunction(
        int geoFrom,
        int geoTo,
        PointPos& posFrom,
        PointPos& posTo
    )
    {
        if (geoFrom == geoTo) {
            return false;
        }

        SketchObject* Obj = sketchgui->getSketchObject();
        const Part::Geometry* fromGeo = Obj->getGeometry(geoFrom);
        const Part::Geometry* toGeo = Obj->getGeometry(geoTo);
        if (!fromGeo || !toGeo || isGeometryDegenerate(fromGeo) || isGeometryDegenerate(toGeo)) {
            return false;
        }

        Base::Vector3d fromStart;
        Base::Vector3d fromEnd;
        Base::Vector3d toStart;
        Base::Vector3d toEnd;
        if (!getFirstSecondPoints(geoFrom, fromStart, fromEnd)
            || !getFirstSecondPoints(geoTo, toStart, toEnd)) {
            return false;
        }

        if (pointsCoincident(fromEnd, toStart)) {
            posFrom = PointPos::end;
            posTo = PointPos::start;
            return true;
        }

        return findSharedEndpoints(geoFrom, geoTo, posFrom, posTo);
    }

    std::vector<int> buildConnectedOffsetChain(
        const std::vector<int>& geoIds,
        const std::vector<int>& preferredOrder
    )
    {
        std::set<int> remaining(geoIds.begin(), geoIds.end());
        std::vector<int> chain;

        for (int geoId : preferredOrder) {
            if (remaining.contains(geoId)) {
                chain.push_back(geoId);
                remaining.erase(geoId);
            }
        }

        if (chain.empty() && !remaining.empty()) {
            chain.push_back(*remaining.begin());
            remaining.erase(chain.front());
        }

        auto attachToChainEnd = [&](bool& extended) {
            for (auto it = remaining.begin(); it != remaining.end();) {
                PointPos posFrom = PointPos::none;
                PointPos posTo = PointPos::none;
                if (findDirectedChainJunction(chain.back(), *it, posFrom, posTo)) {
                    chain.push_back(*it);
                    it = remaining.erase(it);
                    extended = true;
                    return;
                }
                ++it;
            }
        };

        auto attachToChainStart = [&](bool& extended) {
            for (auto it = remaining.begin(); it != remaining.end();) {
                PointPos posFrom = PointPos::none;
                PointPos posTo = PointPos::none;
                if (findDirectedChainJunction(*it, chain.front(), posFrom, posTo)) {
                    chain.insert(chain.begin(), *it);
                    it = remaining.erase(it);
                    extended = true;
                    return;
                }
                ++it;
            }
        };

        bool extended = true;
        while (extended) {
            extended = false;
            attachToChainEnd(extended);
            attachToChainStart(extended);
        }

        for (int orphanGeoId : remaining) {
            Base::Console().warning(
                "Sketcher offset: could not connect geo %d into the offset profile chain.\n",
                orphanGeoId
            );
        }

        return chain;
    }

    TopoDS_Shape makeOffsetShape(bool allowOpenResult = false)
    {
        if (sourceWires.empty()) {
            return {};
        }

        // in OCC the JointTypes are : Arc(0), Tangent(1), Intersection(2)
        const short joinType =
            constructionMethod() == DrawSketchHandlerOffset::ConstructionMethod::Intersection ? 2 : 0;

        // Offset will fail for single lines if we don't set a plane in ctor.
        // But if we set a plane, then the direction of offset is forced...
        // so we set a plane if and only if there are not a single sourceWires with more than single
        // line.
        Part::BRepOffsetAPI_MakeOffsetFix mkOffset;

        if (onlySingleLines) {
            const TopoDS_Face workingPlane = BRepBuilderAPI_MakeFace(gp_Pln(gp::Origin(), gp::DZ()));
            mkOffset.Init(workingPlane, GeomAbs_JoinType(joinType), allowOpenResult);
        }
        else {
            mkOffset.Init(GeomAbs_JoinType(joinType), allowOpenResult);
        }

        for (const TopoDS_Wire& wire : sourceWires) {
            mkOffset.AddWire(wire);
        }

        if (fabs(offsetLength) <= Precision::Confusion()) {
            return {};
        }

        try {
#if defined(__GNUC__) && defined(FC_OS_LINUX)
            Base::SignalException se;
#endif
            mkOffset.Perform(offsetLength);
        }
        catch (Standard_Failure&) {
            throw;
        }
        catch (...) {
            throw Base::CADKernelError(
                "BRepOffsetAPI_MakeOffset has crashed! (Unknown exception caught)"
            );
        }

        if (!mkOffset.IsDone()) {
            return {};
        }

        TopoDS_Shape offsetShape = mkOffset.Shape();

        if (offsetShape.IsNull()) {
            return offsetShape;
        }

        offsetShape = mkOffset.Replace(GeomAbs_OffsetCurve, offsetShape);

        // Copying shape to fix strange orientation behavior, OCC7.0.0. See bug #2699
        //  http://www.freecad.org/tracker/view.php?id=2699
        offsetShape = BRepBuilderAPI_Copy(offsetShape).Shape();
        return offsetShape;
    }

    Part::Geometry* curveToLine(BRepAdaptor_Curve curve)
    {
        double first = curve.FirstParameter();
        if (fabs(first) > 1E99) {
            first = -10000;
        }

        double last = curve.LastParameter();
        if (fabs(last) > 1E99) {
            last = +10000;
        }

        gp_Pnt P1 = curve.Value(first);
        gp_Pnt P2 = curve.Value(last);

        Base::Vector3d p1(P1.X(), P1.Y(), P1.Z());
        Base::Vector3d p2(P2.X(), P2.Y(), P2.Z());
        auto* line = new Part::GeomLineSegment();
        line->setPoints(p1, p2);
        GeometryFacade::setConstruction(line, false);
        return line;
    }

    Part::Geometry* curveToCircleOrArc(BRepAdaptor_Curve curve, const TopoDS_Edge& /*edge*/)
    {
        gp_Circ circle = curve.Circle();
        gp_Pnt cnt = circle.Location();
        gp_Pnt beg = curve.Value(curve.FirstParameter());
        gp_Pnt end = curve.Value(curve.LastParameter());

        if (beg.SquareDistance(end) < Precision::Confusion()) {
            auto* gCircle = new Part::GeomCircle();
            gCircle->setRadius(circle.Radius());
            gCircle->setCenter(Base::Vector3d(cnt.X(), cnt.Y(), cnt.Z()));

            GeometryFacade::setConstruction(gCircle, false);
            return gCircle;
        }
        else {
            Handle(Geom_Circle) hCircle = new Geom_Circle(circle);

            double u1 = curve.FirstParameter();
            double u2 = curve.LastParameter();

            auto* gArc = new Part::GeomArcOfCircle();
            Handle(Geom_TrimmedCurve) tCurve = new Geom_TrimmedCurve(hCircle, u1, u2);
            gArc->setHandle(tCurve);

            gArc->reverseIfReversed();

            GeometryFacade::setConstruction(gArc, false);
            return gArc;
        }
    }

    Part::Geometry* curveToEllipseOrArc(BRepAdaptor_Curve curve, const TopoDS_Edge& /*edge*/)
    {
        gp_Elips ellipse = curve.Ellipse();
        gp_Pnt beg = curve.Value(curve.FirstParameter());
        gp_Pnt end = curve.Value(curve.LastParameter());

        if (beg.SquareDistance(end) < Precision::Confusion()) {
            auto* gEllipse = new Part::GeomEllipse();
            Handle(Geom_Ellipse) hEllipse = new Geom_Ellipse(ellipse);

            gEllipse->setHandle(hEllipse);

            gEllipse->reverseIfReversed();

            GeometryFacade::setConstruction(gEllipse, false);
            return gEllipse;
        }
        else {
            Handle(Geom_Ellipse) hEllipse = new Geom_Ellipse(ellipse);

            double u1 = curve.FirstParameter();
            double u2 = curve.LastParameter();

            Handle(Geom_TrimmedCurve) tCurve = new Geom_TrimmedCurve(hEllipse, u1, u2);
            auto* gArc = new Part::GeomArcOfEllipse();
            gArc->setHandle(tCurve);

            gArc->reverseIfReversed();

            GeometryFacade::setConstruction(gArc, false);
            return gArc;
        }
    }

    bool pointsCoincident(const Base::Vector3d& p1, const Base::Vector3d& p2) const
    {
        return (p1 - p2).Length() < Precision::Confusion();
    }

    bool isExternalGeoId(int geoId) const
    {
        return geoId <= GeoEnum::RefExt;
    }

    bool isSupportedOffsetSourceGeometry(int geoId) const
    {
        const Part::Geometry* geo = sketchgui->getSketchObject()->getGeometry(geoId);
        return geo && !isPoint(*geo) && !isBSplineCurve(*geo) && !isEllipse(*geo)
            && !isArcOfEllipse(*geo) && !isArcOfHyperbola(*geo) && !isArcOfParabola(*geo)
            && !GeometryFacade::isInternalAligned(geo);
    }

    bool sourceClassAllowed(int geoId, bool allowExternal, bool allowConstruction, bool allowReal)
    {
        if (isExternalGeoId(geoId)) {
            return allowExternal;
        }

        const Part::Geometry* geo = sketchgui->getSketchObject()->getGeometry(geoId);
        if (!geo) {
            return false;
        }

        return GeometryFacade::getConstruction(geo) ? allowConstruction : allowReal;
    }

    std::vector<int> collectChainCandidates(bool allowExternal, bool allowConstruction, bool allowReal)
    {
        SketchObject* Obj = sketchgui->getSketchObject();

        std::vector<int> candidates;
        for (int geoId = 0; geoId <= Obj->getHighestCurveIndex(); ++geoId) {
            if (sourceClassAllowed(geoId, allowExternal, allowConstruction, allowReal)
                && isSupportedOffsetSourceGeometry(geoId)) {
                candidates.push_back(geoId);
            }
        }

        for (int extGeoId = 0; extGeoId < Obj->getExternalGeometryCount(); ++extGeoId) {
            int geoId = GeoEnum::RefExt - extGeoId;
            if (sourceClassAllowed(geoId, allowExternal, allowConstruction, allowReal)
                && isSupportedOffsetSourceGeometry(geoId)) {
                candidates.push_back(geoId);
            }
        }

        return candidates;
    }

    std::vector<int> expandConnectedGeoIds(const std::vector<int>& seedGeoIds)
    {
        bool allowExternal = false;
        bool allowConstruction = false;
        bool allowReal = false;

        for (int geoId : seedGeoIds) {
            if (isExternalGeoId(geoId)) {
                allowExternal = true;
                continue;
            }

            const Part::Geometry* geo = sketchgui->getSketchObject()->getGeometry(geoId);
            if (!geo) {
                continue;
            }

            if (GeometryFacade::getConstruction(geo)) {
                allowConstruction = true;
            }
            else {
                allowReal = true;
            }
        }

        std::vector<int> candidates =
            collectChainCandidates(allowExternal, allowConstruction, allowReal);

        std::vector<int> connectedGeoIds;
        std::set<int> connectedSet;
        for (int geoId : seedGeoIds) {
            if (std::ranges::find(candidates, geoId) != candidates.end()
                && connectedSet.insert(geoId).second) {
                connectedGeoIds.push_back(geoId);
            }
        }

        bool added = true;
        while (added) {
            added = false;

            for (int candidateGeoId : candidates) {
                if (connectedSet.contains(candidateGeoId)) {
                    continue;
                }

                bool connected = false;
                for (int connectedGeoId : connectedGeoIds) {
                    if (areCoincident(candidateGeoId, connectedGeoId)) {
                        connected = true;
                        break;
                    }
                }

                if (connected) {
                    connectedSet.insert(candidateGeoId);
                    connectedGeoIds.push_back(candidateGeoId);
                    added = true;
                }
            }
        }

        return connectedGeoIds;
    }

    void refreshSourceGeometry()
    {
        listOfGeoIds = chainLink ? expandConnectedGeoIds(selectedGeoIds) : selectedGeoIds;
        std::set<int> seenGeoIds;
        std::vector<int> uniqueGeoIds;
        uniqueGeoIds.reserve(listOfGeoIds.size());
        for (int geoId : listOfGeoIds) {
            if (seenGeoIds.insert(geoId).second) {
                uniqueGeoIds.push_back(geoId);
            }
        }
        listOfGeoIds = std::move(uniqueGeoIds);

        sourceWires.clear();
        vCC.clear();
        onlySingleLines = true;
        generateSourceWires();
    }

    bool areSameLineSegments(const Part::GeomLineSegment& first, const Part::GeomLineSegment& second)
    {
        const Base::Vector3d firstStart = first.getStartPoint();
        const Base::Vector3d firstEnd = first.getEndPoint();
        const Base::Vector3d secondStart = second.getStartPoint();
        const Base::Vector3d secondEnd = second.getEndPoint();

        return (pointsCoincident(firstStart, secondStart) && pointsCoincident(firstEnd, secondEnd))
            || (pointsCoincident(firstStart, secondEnd) && pointsCoincident(firstEnd, secondStart));
    }

    bool areSameCircles(const Part::GeomCircle& first, const Part::GeomCircle& second)
    {
        return pointsCoincident(first.getCenter(), second.getCenter())
            && fabs(first.getRadius() - second.getRadius()) < Precision::Confusion();
    }

    bool areSameArcOfCircles(const Part::GeomArcOfCircle& first, const Part::GeomArcOfCircle& second)
    {
        if (!pointsCoincident(first.getCenter(), second.getCenter())
            || fabs(first.getRadius() - second.getRadius()) >= Precision::Confusion()) {
            return false;
        }

        const Base::Vector3d firstStart = first.getStartPoint(true);
        const Base::Vector3d firstEnd = first.getEndPoint(true);
        const Base::Vector3d secondStart = second.getStartPoint(true);
        const Base::Vector3d secondEnd = second.getEndPoint(true);

        return (pointsCoincident(firstStart, secondStart) && pointsCoincident(firstEnd, secondEnd))
            || (pointsCoincident(firstStart, secondEnd) && pointsCoincident(firstEnd, secondStart));
    }

    bool areSameGeometries(const Part::Geometry& first, const Part::Geometry& second)
    {
        if (isLineSegment(first) && isLineSegment(second)) {
            return areSameLineSegments(
                static_cast<const Part::GeomLineSegment&>(first),
                static_cast<const Part::GeomLineSegment&>(second)
            );
        }
        if (isCircle(first) && isCircle(second)) {
            return areSameCircles(
                static_cast<const Part::GeomCircle&>(first),
                static_cast<const Part::GeomCircle&>(second)
            );
        }
        if (isArcOfCircle(first) && isArcOfCircle(second)) {
            return areSameArcOfCircles(
                static_cast<const Part::GeomArcOfCircle&>(first),
                static_cast<const Part::GeomArcOfCircle&>(second)
            );
        }

        return false;
    }

    bool appendOffsetGeometry(
        std::unique_ptr<Part::Geometry> geometry,
        std::vector<Part::Geometry*>& geometriesToAdd,
        std::vector<int>& listOfOffsetGeoIds
    )
    {
        if (!geometry) {
            return false;
        }

        if (isGeometryDegenerate(geometry.get())) {
            return false;
        }

        for (const Part::Geometry* existingGeometry : geometriesToAdd) {
            if (areSameGeometries(*geometry, *existingGeometry)) {
                return false;
            }
        }

        listOfOffsetGeoIds.push_back(firstCurveCreated + static_cast<int>(geometriesToAdd.size()));
        geometriesToAdd.push_back(geometry.release());
        return true;
    }

    void getOffsetGeos(std::vector<Part::Geometry*>& geometriesToAdd, std::vector<int>& listOfOffsetGeoIds)
    {
        try {
            offsetWireOrderedGeoIds.clear();
            TopoDS_Shape offsetShape = makeOffsetShape(isConstrainedClearanceMode());
            if (offsetShape.IsNull()) {
                return;
            }

            std::vector<TopoDS_Wire> wires;
            collectOrderedWireEdges(offsetShape, wires);

            std::optional<Base::Vector3d> chainAnchor;
            for (const TopoDS_Wire& wire : wires) {
                for (BRepTools_WireExplorer explorer(wire); explorer.More(); explorer.Next()) {
                    std::unique_ptr<Part::Geometry> geometryToAdd =
                        edgeToGeometry(explorer.Current());
                    if (!geometryToAdd) {
                        continue;
                    }

                    if (chainAnchor) {
                        orientGeometryToStartAt(geometryToAdd.get(), *chainAnchor);
                    }

                    Base::Vector3d edgeStartPoint;
                    Base::Vector3d edgeEndPoint;
                    const bool hasEdgeEndpoints =
                        getGeometryEndpoints(geometryToAdd.get(), edgeStartPoint, edgeEndPoint);

                    if (!appendOffsetGeometry(
                            std::move(geometryToAdd),
                            geometriesToAdd,
                            listOfOffsetGeoIds
                        )) {
                        if (hasEdgeEndpoints) {
                            chainAnchor = edgeEndPoint;
                        }
                        continue;
                    }

                    const int geoId = listOfOffsetGeoIds.back();
                    offsetWireOrderedGeoIds.push_back(geoId);

                    if (hasEdgeEndpoints) {
                        chainAnchor = edgeEndPoint;
                    }
                }
            }

            if (offsetWireOrderedGeoIds.empty()) {
                TopExp_Explorer expl(offsetShape, TopAbs_EDGE);
                for (; expl.More(); expl.Next()) {
                    const TopoDS_Edge& edge = TopoDS::Edge(expl.Current());
                    std::unique_ptr<Part::Geometry> geometryToAdd = edgeToGeometry(edge);
                    if (appendOffsetGeometry(
                            std::move(geometryToAdd),
                            geometriesToAdd,
                            listOfOffsetGeoIds
                        )) {
                        offsetWireOrderedGeoIds.push_back(listOfOffsetGeoIds.back());
                    }
                }
            }
        }
        catch (const Base::Exception&) {
            throw;
        }
    }

    void drawOffsetPreview()
    {
        try {
            std::vector<Part::Geometry*> geometriesToAdd;
            std::vector<int> listOfOffsetGeoIds;
            getOffsetGeos(geometriesToAdd, listOfOffsetGeoIds);

            drawEdit(geometriesToAdd);
        }
        catch (const Base::Exception& e) {
            e.reportException();
        }
    }

    void createOffset()
    {
        if (isConstrainedClearanceMode()) {
            deleteOriginal = false;
        }

        std::vector<Part::Geometry*> geometriesToAdd;
        std::vector<int> listOfOffsetGeoIds;
        try {
            getOffsetGeos(geometriesToAdd, listOfOffsetGeoIds);
        }
        catch (const Base::Exception& e) {
            Gui::NotifyUserError(
                sketchgui->getSketchObject(),
                QT_TRANSLATE_NOOP("Notifications", "Offset Error"),
                e.what()
            );
            return;
        }

        SketchObject* Obj = sketchgui->getSketchObject();

        if (listOfOffsetGeoIds.empty()) {
            Gui::NotifyUserError(
                Obj,
                QT_TRANSLATE_NOOP("Notifications", "Offset Error"),
                QT_TRANSLATE_NOOP("Notifications", "Offset could not be created.")
            );
            return;
        }

        openCommand(QT_TRANSLATE_NOOP("Command", "Offset"));

        // Create geos
        Obj->addGeometry(std::move(geometriesToAdd));

        constraintDedupKeys.clear();

        // Create coincident (& tangent) constraints
        jointOffsetCurves(listOfOffsetGeoIds);

        if (deleteOriginal && !isConstrainedClearanceMode()) {
            deleteOriginalGeometries();
        }
        else if (isConstrainedClearanceMode()) {
            makeConstrainedClearanceConstraints(listOfOffsetGeoIds);
            makeRoundedCornerRadiusConstraints(listOfOffsetGeoIds);
        }
        else if (offsetConstraint) {
            makeOffsetConstraint(listOfOffsetGeoIds);
        }

        QString validationMessage = validateOffsetProfile(listOfOffsetGeoIds);
        validateSketchConstraints();

        commitCommand();

        if (!validationMessage.isEmpty()) {
            Gui::TranslatedUserWarning(
                Obj,
                tr("Offset validation"),
                validationMessage
            );
        }
    }

    void jointOffsetCurves(std::vector<int>& listOfOffsetGeoIds)
    {
        if (listOfOffsetGeoIds.empty()) {
            return;
        }

        const std::vector<int> chain =
            buildConnectedOffsetChain(listOfOffsetGeoIds, offsetWireOrderedGeoIds);
        offsetConnectivityChain = chain;
        vCCO = chain.empty() ? std::vector<std::vector<int>> {} : std::vector<std::vector<int>> {chain};

        if (chain.size() < 2) {
            return;
        }

        std::stringstream stream;
        stream << "conList = []\n";

        for (size_t index = 0; index + 1 < chain.size(); ++index) {
            PointPos pos1 = PointPos::none;
            PointPos pos2 = PointPos::none;
            if (findDirectedChainJunction(chain[index], chain[index + 1], pos1, pos2)) {
                appendJunctionConstraint(stream, chain[index], chain[index + 1], pos1, pos2);
            }
        }

        PointPos closingPos1 = PointPos::none;
        PointPos closingPos2 = PointPos::none;
        if (chain.size() > 2
            && findDirectedChainJunction(chain.back(), chain.front(), closingPos1, closingPos2)) {
            appendJunctionConstraint(stream, chain.back(), chain.front(), closingPos1, closingPos2);
        }

        executeConstraintScript(stream);
    }

    bool needTangent(int geoId1, int geoId2, PointPos pos1, PointPos pos2)
    {
        // Todo: add cases for arcOfellipse parabolas hyperbolas bspline

        SketchObject* Obj = sketchgui->getSketchObject();
        const Part::Geometry* geo1 = Obj->getGeometry(geoId1);
        const Part::Geometry* geo2 = Obj->getGeometry(geoId2);

        if (!isArcOfCircle(*geo1) && !isArcOfCircle(*geo2)) {
            return false;
        }

        Base::Vector3d perpendicular1, perpendicular2, p1, p2;
        if (isArcOfCircle(*geo1)) {
            auto* arcOfCircle = static_cast<const Part::GeomArcOfCircle*>(geo1);
            p1 = pos1 == PointPos::start ? arcOfCircle->getStartPoint(true)
                                         : arcOfCircle->getEndPoint(true);

            perpendicular1.x = -(arcOfCircle->getCenter() - p1).y;
            perpendicular1.y = (arcOfCircle->getCenter() - p1).x;
        }
        else if (isLineSegment(*geo1)) {
            auto* line = static_cast<const Part::GeomLineSegment*>(geo1);
            perpendicular1 = line->getStartPoint() - line->getEndPoint();
        }
        else {
            return false;
        }

        if (isArcOfCircle(*geo2)) {
            auto* arcOfCircle = static_cast<const Part::GeomArcOfCircle*>(geo2);
            p2 = pos2 == PointPos::start ? arcOfCircle->getStartPoint(true)
                                         : arcOfCircle->getEndPoint(true);

            perpendicular2.x = -(arcOfCircle->getCenter() - p2).y;
            perpendicular2.y = (arcOfCircle->getCenter() - p2).x;
        }
        else if (isLineSegment(*geo2)) {
            auto* line = static_cast<const Part::GeomLineSegment*>(geo2);
            perpendicular2 = line->getStartPoint() - line->getEndPoint();
        }
        else {
            return false;
        }

        // if lines are parallel
        if ((perpendicular1 % perpendicular2).Length() < Precision::Confusion()) {
            return true;
        }

        return false;
    }

    void deleteOriginalGeometries()
    {
        std::stringstream stream;
        for (size_t j = 0; j < listOfGeoIds.size() - 1; j++) {
            stream << listOfGeoIds[j] << ",";
        }
        stream << listOfGeoIds[listOfGeoIds.size() - 1];
        try {
            Gui::cmdAppObjectArgs(sketchgui->getObject(), "delGeometries([%s])", stream.str().c_str());
        }
        catch (const Base::Exception& e) {
            Base::Console().error("%s\n", e.what());
        }
    }

    bool lineOffsetMatchesSourceLine(int offsetGeoId, int sourceGeoId)
    {
        SketchObject* Obj = sketchgui->getSketchObject();
        const Part::Geometry* offsetGeo = Obj->getGeometry(offsetGeoId);
        const Part::Geometry* sourceGeo = Obj->getGeometry(sourceGeoId);
        if (!offsetGeo || !sourceGeo || !isLineSegment(*offsetGeo) || !isLineSegment(*sourceGeo)) {
            return false;
        }

        const auto* offsetLine = static_cast<const Part::GeomLineSegment*>(offsetGeo);
        const auto* sourceLine = static_cast<const Part::GeomLineSegment*>(sourceGeo);

        const Base::Vector3d offsetStart = offsetLine->getStartPoint();
        const Base::Vector3d offsetEnd = offsetLine->getEndPoint();
        const Base::Vector3d sourceStart = sourceLine->getStartPoint();
        const Base::Vector3d sourceEnd = sourceLine->getEndPoint();

        const Base::Vector3d offsetDirection = offsetEnd - offsetStart;
        const Base::Vector3d sourceDirection = sourceEnd - sourceStart;
        const double directionScale =
            std::sqrt(dot2d(offsetDirection, offsetDirection) * dot2d(sourceDirection, sourceDirection));
        if (directionScale < Precision::Confusion()) {
            return false;
        }

        if (fabs(cross2d(offsetDirection, sourceDirection)) / directionScale
            > Precision::Confusion()) {
            return false;
        }

        Base::Vector3d projectedDistance;
        projectedDistance.ProjectToLine(offsetStart - sourceStart, sourceDirection);
        return fabs(projectedDistance.Length() - fabs(offsetLength)) < Precision::Confusion();
    }

    bool circleOffsetMatchesSourceCircle(int offsetGeoId, int sourceGeoId)
    {
        SketchObject* Obj = sketchgui->getSketchObject();
        const Part::Geometry* offsetGeo = Obj->getGeometry(offsetGeoId);
        const Part::Geometry* sourceGeo = Obj->getGeometry(sourceGeoId);

        if (!offsetGeo || !sourceGeo) {
            return false;
        }

        Base::Vector3d offsetCenter;
        Base::Vector3d sourceCenter;
        double offsetRadius = 0.0;
        double sourceRadius = 0.0;

        if (isCircle(*offsetGeo) && isCircle(*sourceGeo)) {
            offsetCenter = static_cast<const Part::GeomCircle*>(offsetGeo)->getCenter();
            sourceCenter = static_cast<const Part::GeomCircle*>(sourceGeo)->getCenter();
            offsetRadius = static_cast<const Part::GeomCircle*>(offsetGeo)->getRadius();
            sourceRadius = static_cast<const Part::GeomCircle*>(sourceGeo)->getRadius();
        }
        else if (isArcOfCircle(*offsetGeo) && isArcOfCircle(*sourceGeo)) {
            offsetCenter = static_cast<const Part::GeomArcOfCircle*>(offsetGeo)->getCenter();
            sourceCenter = static_cast<const Part::GeomArcOfCircle*>(sourceGeo)->getCenter();
            offsetRadius = static_cast<const Part::GeomArcOfCircle*>(offsetGeo)->getRadius();
            sourceRadius = static_cast<const Part::GeomArcOfCircle*>(sourceGeo)->getRadius();
        }
        else {
            return false;
        }

        if (!pointsCoincident(offsetCenter, sourceCenter)) {
            return false;
        }

        return fabs(offsetRadius - sourceRadius - fabs(offsetLength)) < Precision::Confusion();
    }

    bool isHorizontalLine(const Part::GeomLineSegment& line)
    {
        return fabs(line.getStartPoint().y - line.getEndPoint().y) < Precision::Confusion();
    }

    bool isVerticalLine(const Part::GeomLineSegment& line)
    {
        return fabs(line.getStartPoint().x - line.getEndPoint().x) < Precision::Confusion();
    }

    double clearanceMatchScore(int offsetGeoId, int sourceGeoId)
    {
        SketchObject* Obj = sketchgui->getSketchObject();
        const Part::Geometry* offsetGeo = Obj->getGeometry(offsetGeoId);
        const Part::Geometry* sourceGeo = Obj->getGeometry(sourceGeoId);
        if (!offsetGeo || !sourceGeo || !isLineSegment(*offsetGeo) || !isLineSegment(*sourceGeo)) {
            return std::numeric_limits<double>::max();
        }

        const auto* offsetLine = static_cast<const Part::GeomLineSegment*>(offsetGeo);
        const auto* sourceLine = static_cast<const Part::GeomLineSegment*>(sourceGeo);

        const Base::Vector3d offsetMid =
            (offsetLine->getStartPoint() + offsetLine->getEndPoint()) * 0.5;
        const Base::Vector3d sourceStart = sourceLine->getStartPoint();
        const Base::Vector3d sourceEnd = sourceLine->getEndPoint();
        const Base::Vector3d sourceDirection = sourceEnd - sourceStart;
        const double sourceLengthSquared = dot2d(sourceDirection, sourceDirection);
        if (sourceLengthSquared < Precision::Confusion()) {
            return std::numeric_limits<double>::max();
        }

        const Base::Vector3d toOffsetMid = offsetMid - sourceStart;
        const double projection =
            std::clamp(dot2d(toOffsetMid, sourceDirection) / sourceLengthSquared, 0.0, 1.0);
        const Base::Vector3d closestPointOnSource = Base::Vector3d(
            sourceStart.x + sourceDirection.x * projection,
            sourceStart.y + sourceDirection.y * projection,
            sourceStart.z + sourceDirection.z * projection
        );

        return (offsetMid - closestPointOnSource).Length();
    }

    bool appendClearanceDistanceConstraint(
        std::stringstream& stream,
        int offsetGeoId,
        int sourceGeoId
    )
    {
        SketchObject* Obj = sketchgui->getSketchObject();
        const Part::Geometry* offsetGeo = Obj->getGeometry(offsetGeoId);
        const Part::Geometry* sourceGeo = Obj->getGeometry(sourceGeoId);
        if (!offsetGeo || !sourceGeo || !isLineSegment(*offsetGeo) || !isLineSegment(*sourceGeo)) {
            return false;
        }

        const auto* offsetLine = static_cast<const Part::GeomLineSegment*>(offsetGeo);
        const auto* sourceLine = static_cast<const Part::GeomLineSegment*>(sourceGeo);

        // The clearance dimension must be measured *between* the generated offset edge and the
        // external/reference source edge, otherwise it is orphaned and does not drive the offset.
        // For axis-aligned edges we emit a point-to-point horizontal/vertical distance between the
        // two start endpoints; for any other orientation we emit a perpendicular edge-to-edge
        // distance. In every case the constraint references sourceGeoId.
        //
        // Note: the 3-argument forms Constraint('DistanceX'/'DistanceY', geo1, geo2, value) are NOT
        // line-to-line distances - ConstraintPyImp parses them as a point coordinate constraint
        // (FirstPos = geo2, Second = -1), which is exactly the orphaned-distance defect we must
        // avoid here. The 5-argument point-to-point form is required instead.
        const int startPos = static_cast<int>(PointPos::start);
        const Base::Vector3d offsetStart = offsetLine->getStartPoint();
        const Base::Vector3d sourceStart = sourceLine->getStartPoint();

        std::ostringstream expression;
        if (isHorizontalLine(*offsetLine) && isHorizontalLine(*sourceLine)) {
            // GCS stores DistanceY as (secondPoint.y - firstPoint.y); pass the current signed gap so
            // the solver keeps the offset on its existing side instead of flipping it.
            const double signedClearance = sourceStart.y - offsetStart.y;
            expression << "Sketcher.Constraint('DistanceY'," << offsetGeoId << ", " << startPos
                       << ", " << sourceGeoId << ", " << startPos << ", " << signedClearance << ")";
            return tryAddConstraint(stream, "DistanceY", offsetGeoId, startPos, sourceGeoId,
                                    startPos, expression.str());
        }
        if (isVerticalLine(*offsetLine) && isVerticalLine(*sourceLine)) {
            const double signedClearance = sourceStart.x - offsetStart.x;
            expression << "Sketcher.Constraint('DistanceX'," << offsetGeoId << ", " << startPos
                       << ", " << sourceGeoId << ", " << startPos << ", " << signedClearance << ")";
            return tryAddConstraint(stream, "DistanceX", offsetGeoId, startPos, sourceGeoId,
                                    startPos, expression.str());
        }

        expression << "Sketcher.Constraint('Distance'," << offsetGeoId << ", " << sourceGeoId
                   << ", " << fabs(offsetLength) << ")";
        return tryAddConstraint(stream, "Distance", offsetGeoId, -1, sourceGeoId, -1,
                                expression.str());
    }

    void makeConstrainedClearanceConstraints(std::vector<int>& listOfOffsetGeoIds)
    {
        struct LineClearanceMatch
        {
            int offsetGeoId;
            int sourceGeoId;
            bool horizontal;
            bool vertical;
        };

        std::vector<LineClearanceMatch> lineMatches;
        std::vector<std::pair<int, int>> circleMatches;
        const std::set<int> connectedGeos(
            offsetConnectivityChain.begin(),
            offsetConnectivityChain.end()
        );

        for (int offsetGeoId : listOfOffsetGeoIds) {
            if (!connectedGeos.empty() && !connectedGeos.contains(offsetGeoId)) {
                continue;
            }

            SketchObject* Obj = sketchgui->getSketchObject();
            const Part::Geometry* offsetGeo = Obj->getGeometry(offsetGeoId);
            if (!offsetGeo || !isLineSegment(*offsetGeo)) {
                continue;
            }

            const auto* offsetLine = static_cast<const Part::GeomLineSegment*>(offsetGeo);

            bool foundBestMatch = false;
            double bestScore = std::numeric_limits<double>::max();
            LineClearanceMatch bestMatch {};

            for (int sourceGeoId : listOfGeoIds) {
                if (!lineOffsetMatchesSourceLine(offsetGeoId, sourceGeoId)) {
                    continue;
                }

                const double score = clearanceMatchScore(offsetGeoId, sourceGeoId);
                if (!foundBestMatch || score < bestScore) {
                    bestMatch = {offsetGeoId,
                                 sourceGeoId,
                                 isHorizontalLine(*offsetLine),
                                 isVerticalLine(*offsetLine)};
                    bestScore = score;
                    foundBestMatch = true;
                }
            }

            if (foundBestMatch) {
                lineMatches.push_back(bestMatch);
            }
        }

        for (int offsetGeoId : listOfOffsetGeoIds) {
            for (int sourceGeoId : listOfGeoIds) {
                if (circleOffsetMatchesSourceCircle(offsetGeoId, sourceGeoId)) {
                    circleMatches.emplace_back(offsetGeoId, sourceGeoId);
                }
            }
        }

        std::ranges::stable_sort(lineMatches, [](const LineClearanceMatch& first,
                                                   const LineClearanceMatch& second) {
            const int firstScore = (first.horizontal || first.vertical) ? 0 : 1;
            const int secondScore = (second.horizontal || second.vertical) ? 0 : 1;
            return firstScore < secondScore;
        });

        std::stringstream stream;
        stream << "conList = []\n";

        for (const LineClearanceMatch& match : lineMatches) {
            std::ostringstream parallelExpression;
            parallelExpression << "Sketcher.Constraint('Parallel'," << match.offsetGeoId << ", "
                               << match.sourceGeoId << ")";
            tryAddConstraint(stream, "Parallel", match.offsetGeoId, -1, match.sourceGeoId, -1,
                             parallelExpression.str());

            appendClearanceDistanceConstraint(stream, match.offsetGeoId, match.sourceGeoId);
        }

        bool drivingRadiusAdded = false;
        std::set<int> radiusConstrainedOffsetGeos;
        SketchObject* Obj = sketchgui->getSketchObject();
        for (const auto& [offsetGeoId, sourceGeoId] : circleMatches) {
            if (!radiusConstrainedOffsetGeos.insert(offsetGeoId).second) {
                continue;
            }

            std::ostringstream coincidentExpression;
            coincidentExpression << "Sketcher.Constraint('Coincident'," << offsetGeoId
                                 << ", 3, " << sourceGeoId << ", 3)";
            tryAddConstraint(stream, "Coincident", offsetGeoId, 3, sourceGeoId, 3,
                             coincidentExpression.str());

            if (drivingRadiusAdded) {
                continue;
            }

            const Part::Geometry* sourceGeo = Obj->getGeometry(sourceGeoId);
            if (!sourceGeo) {
                continue;
            }

            double sourceRadius = 0.0;
            if (isCircle(*sourceGeo)) {
                sourceRadius = static_cast<const Part::GeomCircle*>(sourceGeo)->getRadius();
            }
            else if (isArcOfCircle(*sourceGeo)) {
                sourceRadius = static_cast<const Part::GeomArcOfCircle*>(sourceGeo)->getRadius();
            }
            else {
                continue;
            }

            std::ostringstream radiusExpression;
            radiusExpression << "Sketcher.Constraint('Radius'," << offsetGeoId << ", "
                             << sourceRadius + fabs(offsetLength) << ")";
            drivingRadiusAdded =
                tryAddConstraint(stream, "Radius", offsetGeoId, -1, -1, -1, radiusExpression.str());
        }

        executeConstraintScript(stream);
    }

    void makeOffsetConstraint(std::vector<int>& listOfOffsetGeoIds)
    {
        SketchObject* Obj = sketchgui->getSketchObject();

        std::stringstream stream;
        stream << "conList = []\n";
        // We separate the constraints of new lines in case the construction lines are not needed.
        std::stringstream newLinesStream;
        newLinesStream << "conList2 = []\n";

        vCCO = generatevCC(listOfOffsetGeoIds);

        int geoIdCandidate1 {};
        int geoIdCandidate2 {};

        int newCurveCounter = 0;
        int prevCurveCounter = 0;
        std::vector<Part::Geometry*> geometriesToAdd;
        for (auto& curve : vCCO) {
            // Check if curve is closed. Note as we use pipe it should always be closed but in case
            // we enable 'Skin' in the future.
            bool closed = isCurveClosed(curve);
            bool atLeastOneLine = false;
            bool rerunFirstAfterThis = false;
            bool rerunningFirst = false;
            bool inTangentGroup = false;

            for (size_t j = 0; j < curve.size(); j++) {

                // Tangent constraint is constraining the offset already. So if there are tangents
                // we should not create the construction lines. Hence the code below.
                bool createLine = true;
                bool forceCreate = false;
                if (!inTangentGroup && (!closed || j != 0 || rerunningFirst)) {
                    createLine = true;
                    atLeastOneLine = true;
                }
                else {
                    // include case of j == 0 and closed curve, because if required the line
                    // will be made after last.
                    createLine = false;
                }

                if (j + 1 < curve.size()) {
                    inTangentGroup = areTangentCoincident(curve[j], curve[j + 1]);
                }
                else if (j == curve.size() - 1 && closed) {
                    // Case of last geoId for closed curves.
                    inTangentGroup = areTangentCoincident(curve[j], curve[0]);
                    if (inTangentGroup) {
                        if (!atLeastOneLine) {  // We need at least one line
                            createLine = true;
                            forceCreate = true;
                        }
                    }
                    else {
                        // We rerun the for at j=0 after this run to create line for j = 0.
                        rerunFirstAfterThis = true;
                    }
                }

                const Part::Geometry* geo = Obj->getGeometry(curve[j]);
                for (auto geoId : listOfGeoIds) {
                    // Check if geoId is the offsetted curve giving curve[j].
                    const Part::Geometry* geo2 = Obj->getGeometry(geoId);

                    if (isCircle(*geo) && isCircle(*geo2)) {
                        auto* circle = static_cast<const Part::GeomCircle*>(geo);
                        auto* circle2 = static_cast<const Part::GeomCircle*>(geo2);
                        Base::Vector3d p1 = circle->getCenter();
                        Base::Vector3d p2 = circle2->getCenter();
                        if ((p1 - p2).Length() < Precision::Confusion()) {
                            // coincidence of center
                            stream << "conList.append(Sketcher.Constraint('Coincident'," << curve[j]
                                   << ",3, " << geoId << ",3))\n";

                            // Create line between both circles.
                            auto* line = new Part::GeomLineSegment();
                            p1.x = p1.x + circle->getRadius();
                            p2.x = p2.x + circle2->getRadius();
                            line->setPoints(p1, p2);
                            GeometryFacade::setConstruction(line, true);
                            geometriesToAdd.push_back(line);
                            newCurveCounter++;
                            newLinesStream << "conList2.append(Sketcher.Constraint('Perpendicular',"
                                           << getHighestCurveIndex() + newCurveCounter << ", "
                                           << curve[j] << "))\n";
                            newLinesStream << "conList2.append(Sketcher.Constraint('PointOnObject',"
                                           << getHighestCurveIndex() + newCurveCounter << ",1, "
                                           << curve[j] << "))\n";
                            newLinesStream << "conList2.append(Sketcher.Constraint('PointOnObject',"
                                           << getHighestCurveIndex() + newCurveCounter << ",2, "
                                           << geoId << "))\n";

                            geoIdCandidate1 = curve[j];
                            geoIdCandidate2 = geoId;

                            break;
                        }
                    }
                    else if (isEllipse(*geo) && isEllipse(*geo2)) {
                        // same as circle but 2 lines
                    }
                    else if (isLineSegment(*geo) && isLineSegment(*geo2)) {
                        auto* lineSeg1 = static_cast<const Part::GeomLineSegment*>(geo);
                        auto* lineSeg2 = static_cast<const Part::GeomLineSegment*>(geo2);
                        Base::Vector3d p1[2], p2[2];
                        p1[0] = lineSeg1->getStartPoint();
                        p1[1] = lineSeg1->getEndPoint();
                        p2[0] = lineSeg2->getStartPoint();
                        p2[1] = lineSeg2->getEndPoint();
                        // if lines are parallel
                        if (((p1[1] - p1[0]) % (p2[1] - p2[0])).Length() < Precision::Intersection()) {
                            // If the lines are space by offsetLength distance
                            Base::Vector3d projectedP;
                            projectedP.ProjectToLine(p1[0] - p2[0], p2[1] - p2[0]);

                            if ((projectedP).Length() - fabs(offsetLength) < Precision::Confusion()) {
                                if (!forceCreate && !rerunningFirst) {
                                    stream << "conList.append(Sketcher.Constraint('Parallel',"
                                           << curve[j] << ", " << geoId << "))\n";
                                }

                                // We don't need a construction line if the line has a tangent at
                                // one end. Unless it's the first line that we're making.
                                if (createLine) {
                                    auto* line = new Part::GeomLineSegment();
                                    line->setPoints(p1[0], p1[0] + projectedP);
                                    GeometryFacade::setConstruction(line, true);
                                    geometriesToAdd.push_back(line);
                                    newCurveCounter++;

                                    newLinesStream
                                        << "conList2.append(Sketcher.Constraint('Perpendicular',"
                                        << getHighestCurveIndex() + newCurveCounter << ", "
                                        << curve[j] << "))\n";
                                    newLinesStream
                                        << "conList2.append(Sketcher.Constraint('PointOnObject',"
                                        << getHighestCurveIndex() + newCurveCounter << ",1, "
                                        << curve[j] << "))\n";
                                    newLinesStream
                                        << "conList2.append(Sketcher.Constraint('PointOnObject',"
                                        << getHighestCurveIndex() + newCurveCounter << ",2, "
                                        << geoId << "))\n";

                                    geoIdCandidate1 = curve[j];
                                    geoIdCandidate2 = geoId;
                                }
                                break;
                            }
                        }
                    }
                    else if (isArcOfCircle(*geo)) {
                        // multiple cases because arc join mode creates arcs or circle.
                        auto* arcOfCircle = static_cast<const Part::GeomArcOfCircle*>(geo);
                        Base::Vector3d p1 = arcOfCircle->getCenter();

                        if (isArcOfCircle(*geo2)) {
                            auto* arcOfCircle2 = static_cast<const Part::GeomArcOfCircle*>(geo2);
                            Base::Vector3d p2 = arcOfCircle2->getCenter();
                            Base::Vector3d p3 = arcOfCircle2->getStartPoint(true);
                            Base::Vector3d p4 = arcOfCircle2->getEndPoint(true);

                            if ((p1 - p2).Length() < Precision::Confusion()) {
                                // coincidence of center. Offset arc is the offset of an arc
                                stream << "conList.append(Sketcher.Constraint('Coincident',"
                                       << curve[j] << ",3, " << geoId << ",3))\n";
                                if (createLine) {
                                    // Create line between both circles.
                                    auto* line = new Part::GeomLineSegment();
                                    p1.x = p1.x + arcOfCircle->getRadius();
                                    p2.x = p2.x + arcOfCircle2->getRadius();
                                    line->setPoints(p1, p2);
                                    GeometryFacade::setConstruction(line, true);
                                    geometriesToAdd.push_back(line);
                                    newCurveCounter++;
                                    newLinesStream
                                        << "conList2.append(Sketcher.Constraint('Perpendicular',"
                                        << getHighestCurveIndex() + newCurveCounter << ", "
                                        << curve[j] << "))\n";
                                    newLinesStream
                                        << "conList2.append(Sketcher.Constraint('PointOnObject',"
                                        << getHighestCurveIndex() + newCurveCounter << ",1, "
                                        << curve[j] << "))\n";
                                    newLinesStream
                                        << "conList2.append(Sketcher.Constraint('PointOnObject',"
                                        << getHighestCurveIndex() + newCurveCounter << ",2, "
                                        << geoId << "))\n";

                                    geoIdCandidate1 = curve[j];
                                    geoIdCandidate2 = geoId;
                                }
                                break;
                            }
                            else if ((p1 - p3).Length() < Precision::Confusion()) {
                                // coincidence of center to startpoint. offset arc is created arc
                                // join
                                stream << "conList.append(Sketcher.Constraint('Coincident',"
                                       << curve[j] << ",3, " << geoId << ", 1))\n";

                                if (forceCreate) {
                                    stream << "conList.append(Sketcher.Constraint('Radius',"
                                           << curve[j] << ", " << offsetLength << "))\n";
                                }
                                break;
                            }
                            else if ((p1 - p4).Length() < Precision::Confusion()) {
                                // coincidence of center to startpoint
                                stream << "conList.append(Sketcher.Constraint('Coincident',"
                                       << curve[j] << ",3, " << geoId << ", 2))\n";

                                if (forceCreate) {
                                    stream << "conList.append(Sketcher.Constraint('Radius',"
                                           << curve[j] << ", " << offsetLength << "))\n";
                                }
                                break;
                            }
                        }
                        else if (
                            isLineSegment(*geo2) || isBSplineCurve(*geo2)
                            || geo2->is<Part::GeomArcOfConic>()
                        ) {
                            // cases where arc is created by arc join mode.
                            Base::Vector3d p2, p3;

                            if (getFirstSecondPoints(geoId, p2, p3)) {
                                bool startCoincidence = (p1 - p2).Length() < Precision::Confusion();
                                bool endCoincidence = (p1 - p3).Length() < Precision::Confusion();

                                if (startCoincidence || endCoincidence) {
                                    // coincidence of center to startpoint
                                    stream << "conList.append(Sketcher.Constraint('Coincident',"
                                           << curve[j] << ", 3, " << geoId << ", "
                                           << (startCoincidence ? 1 : 2) << "))\n";

                                    geoIdCandidate1 = curve[j];
                                    geoIdCandidate2 = geoId;

                                    break;
                                }
                            }
                        }
                    }
                    else if (isArcOfEllipse(*geo) && isArcOfEllipse(*geo2)) {
                        // const Part::GeomArcOfEllipse* arcOfEllipse = static_cast<const
                        // Part::GeomArcOfEllipse*>(geo2);
                    }
                    else if (isArcOfHyperbola(*geo) && isArcOfHyperbola(*geo2)) {
                        // const Part::GeomArcOfHyperbola* arcOfHyperbola = static_cast<const
                        // Part::GeomArcOfHyperbola*>(geo2);
                    }
                    else if (isArcOfParabola(*geo) && isArcOfParabola(*geo2)) {
                        // const Part::GeomArcOfParabola* arcOfParabola = static_cast<const
                        // Part::GeomArcOfParabola*>(geo2);
                    }
                    else if (isBSplineCurve(*geo) && isBSplineCurve(*geo2)) {
                    }
                }
                if (newCurveCounter != prevCurveCounter) {
                    prevCurveCounter = newCurveCounter;
                    if (newCurveCounter != 1) {
                        stream << "conList.append(Sketcher.Constraint('Equal',"
                               << getHighestCurveIndex() + newCurveCounter << ", "
                               << getHighestCurveIndex() + 1 << "))\n";
                    }
                }


                if (rerunningFirst) {
                    break;
                }

                if (rerunFirstAfterThis) {
                    j = -1;  // j will be incremented to 0 after new loop
                    rerunningFirst = true;
                }
            }
        }

        if (newCurveCounter >= 2) {
            stream << "conList.append(Sketcher.Constraint('Distance'," << getHighestCurveIndex() + 1
                   << ", " << fabs(offsetLength) << "))\n";

            Obj->addGeometry(std::move(geometriesToAdd));

            newLinesStream << Gui::Command::getObjectCmd(sketchgui->getObject())
                           << ".addConstraint(conList2)\n";
            newLinesStream << "del conList2\n";
            Gui::Command::doCommand(Gui::Command::Doc, newLinesStream.str().c_str());
        }
        else {
            // If there is a single construction line, then its not needed.
            const Part::Geometry* geo = Obj->getGeometry(geoIdCandidate1);

            if (isCircle(*geo)) {
                stream << "conList.append(Sketcher.Constraint('Distance'," << geoIdCandidate1
                       << ", " << geoIdCandidate2 << ", " << fabs(offsetLength) << "))\n";
            }
            else if (isLineSegment(*geo)) {
                stream << "conList.append(Sketcher.Constraint('Distance'," << geoIdCandidate1
                       << ", 1," << geoIdCandidate2 << ", " << fabs(offsetLength) << "))\n";
            }
            else if (isArcOfCircle(*geo)) {
                const Part::Geometry* geo2 = Obj->getGeometry(geoIdCandidate2);
                if (isArcOfCircle(*geo2)) {
                    stream << "conList.append(Sketcher.Constraint('Distance'," << geoIdCandidate1
                           << ", 1," << geoIdCandidate2 << ", 1, " << fabs(offsetLength) << "))\n";
                }
                else if (isLineSegment(*geo2)) {
                    stream << "conList.append(Sketcher.Constraint('Distance'," << geoIdCandidate1
                           << ", 3," << geoIdCandidate1 << ", 1, " << fabs(offsetLength) << "))\n";
                }
            }
        }

        stream << Gui::Command::getObjectCmd(sketchgui->getObject()) << ".addConstraint(conList)\n";
        stream << "del conList\n";
        Gui::Command::doCommand(Gui::Command::Doc, stream.str().c_str());
    }

    bool hasRadiusConstraint(int geoId)
    {
        SketchObject* Obj = sketchgui->getSketchObject();
        const std::vector<Constraint*>& vals = Obj->Constraints.getValues();
        for (const auto* cstr : vals) {
            if (cstr->Type == Radius && cstr->First == geoId) {
                return true;
            }
        }

        return false;
    }

    bool isGeneratedRoundedCornerArc(int offsetGeoId)
    {
        SketchObject* Obj = sketchgui->getSketchObject();
        const Part::Geometry* offsetGeo = Obj->getGeometry(offsetGeoId);
        if (!offsetGeo || !isArcOfCircle(*offsetGeo)) {
            return false;
        }

        const auto* offsetArc = static_cast<const Part::GeomArcOfCircle*>(offsetGeo);
        const Base::Vector3d center = offsetArc->getCenter();

        for (int sourceGeoId : listOfGeoIds) {
            Base::Vector3d sourceStart;
            Base::Vector3d sourceEnd;
            if (!getFirstSecondPoints(sourceGeoId, sourceStart, sourceEnd)) {
                continue;
            }

            if (pointsCoincident(center, sourceStart) || pointsCoincident(center, sourceEnd)) {
                return true;
            }
        }

        return false;
    }

    void makeRoundedCornerRadiusConstraints(std::vector<int>& listOfOffsetGeoIds)
    {
        SketchObject* Obj = sketchgui->getSketchObject();
        std::vector<int> roundedArcGeoIds;
        for (int offsetGeoId : listOfOffsetGeoIds) {
            if (!offsetConnectivityChain.empty()) {
                if (std::ranges::find(offsetConnectivityChain, offsetGeoId)
                    == offsetConnectivityChain.end()) {
                    continue;
                }
            }

            const Part::Geometry* offsetGeo = Obj->getGeometry(offsetGeoId);
            if (!offsetGeo || !isArcOfCircle(*offsetGeo) || isGeometryDegenerate(offsetGeo)) {
                continue;
            }

            if (!isGeneratedRoundedCornerArc(offsetGeoId)) {
                continue;
            }

            roundedArcGeoIds.push_back(offsetGeoId);
        }

        if (roundedArcGeoIds.empty()) {
            return;
        }

        std::stringstream stream;
        stream << "conList = []\n";

        const int drivingArcGeoId = roundedArcGeoIds.front();
        if (!hasRadiusConstraint(drivingArcGeoId)) {
            const auto* firstArc =
                static_cast<const Part::GeomArcOfCircle*>(Obj->getGeometry(drivingArcGeoId));
            std::ostringstream radiusExpression;
            radiusExpression << "Sketcher.Constraint('Radius'," << drivingArcGeoId << ", "
                             << firstArc->getRadius() << ")";
            tryAddConstraint(stream, "Radius", drivingArcGeoId, -1, -1, -1,
                             radiusExpression.str());
        }

        for (size_t i = 1; i < roundedArcGeoIds.size(); ++i) {
            const Part::Geometry* arcGeo = Obj->getGeometry(roundedArcGeoIds[i]);
            const Part::Geometry* drivingArcGeo = Obj->getGeometry(drivingArcGeoId);
            if (!arcGeo || !drivingArcGeo || !isArcOfCircle(*arcGeo)
                || !isArcOfCircle(*drivingArcGeo)) {
                continue;
            }

            std::ostringstream equalExpression;
            equalExpression << "Sketcher.Constraint('Equal'," << roundedArcGeoIds[i] << ", "
                            << drivingArcGeoId << ")";
            tryAddConstraint(stream, "Equal", roundedArcGeoIds[i], -1, drivingArcGeoId, -1,
                             equalExpression.str());
        }

        executeConstraintScript(stream);
    }

    void validateSketchConstraints()
    {
        std::stringstream stream;
        stream << Gui::Command::getObjectCmd(sketchgui->getObject()) << ".validateConstraints()\n";
        Gui::Command::doCommand(Gui::Command::Doc, stream.str().c_str());
    }

    struct OffsetEndpoint
    {
        int geoId;
        PointPos pos;
        Base::Vector3d point;
    };

    std::vector<OffsetEndpoint> getOffsetEndpoints(const std::vector<int>& listOfOffsetGeoIds)
    {
        std::vector<OffsetEndpoint> endpoints;
        endpoints.reserve(listOfOffsetGeoIds.size() * 2);

        for (int geoId : listOfOffsetGeoIds) {
            Base::Vector3d startPoint;
            Base::Vector3d endPoint;
            if (!getFirstSecondPoints(geoId, startPoint, endPoint)) {
                continue;
            }

            endpoints.push_back({geoId, PointPos::start, startPoint});
            endpoints.push_back({geoId, PointPos::end, endPoint});
        }

        return endpoints;
    }

    bool hasEndpointConstraint(int geoId1, PointPos pos1, int geoId2, PointPos pos2)
    {
        SketchObject* Obj = sketchgui->getSketchObject();
        const std::vector<Constraint*>& vals = Obj->Constraints.getValues();
        for (const auto* cstr : vals) {
            if (cstr->Type != Coincident && cstr->Type != Tangent) {
                continue;
            }

            bool sameOrder = cstr->First == geoId1 && cstr->FirstPos == pos1
                && cstr->Second == geoId2 && cstr->SecondPos == pos2;
            bool reverseOrder = cstr->First == geoId2 && cstr->FirstPos == pos2
                && cstr->Second == geoId1 && cstr->SecondPos == pos1;

            if (sameOrder || reverseOrder) {
                return true;
            }
        }

        return false;
    }

    int countDuplicateGeometries(const std::vector<int>& listOfOffsetGeoIds)
    {
        SketchObject* Obj = sketchgui->getSketchObject();
        int duplicateCount = 0;

        for (size_t i = 0; i < listOfOffsetGeoIds.size(); ++i) {
            const Part::Geometry* first = Obj->getGeometry(listOfOffsetGeoIds[i]);
            if (!first) {
                continue;
            }

            for (size_t j = i + 1; j < listOfOffsetGeoIds.size(); ++j) {
                const Part::Geometry* second = Obj->getGeometry(listOfOffsetGeoIds[j]);
                if (second && areSameGeometries(*first, *second)) {
                    ++duplicateCount;
                }
            }
        }

        return duplicateCount;
    }

    int countOpenVertices(const std::vector<OffsetEndpoint>& endpoints)
    {
        int openVertexCount = 0;
        for (size_t i = 0; i < endpoints.size(); ++i) {
            bool matched = false;
            for (size_t j = 0; j < endpoints.size(); ++j) {
                if (i == j) {
                    continue;
                }
                if (pointsCoincident(endpoints[i].point, endpoints[j].point)) {
                    matched = true;
                    break;
                }
            }

            if (!matched) {
                ++openVertexCount;
            }
        }

        return openVertexCount;
    }

    int countMissingCoincidentConstraints(const std::vector<OffsetEndpoint>& endpoints)
    {
        int missingCount = 0;
        for (size_t i = 0; i < endpoints.size(); ++i) {
            for (size_t j = i + 1; j < endpoints.size(); ++j) {
                if (endpoints[i].geoId == endpoints[j].geoId
                    || !pointsCoincident(endpoints[i].point, endpoints[j].point)) {
                    continue;
                }

                if (!hasEndpointConstraint(
                        endpoints[i].geoId,
                        endpoints[i].pos,
                        endpoints[j].geoId,
                        endpoints[j].pos
                    )) {
                    ++missingCount;
                }
            }
        }

        return missingCount;
    }

    int countConstructionOffsetGeometry(const std::vector<int>& listOfOffsetGeoIds)
    {
        SketchObject* Obj = sketchgui->getSketchObject();
        int constructionCount = 0;
        for (int geoId : listOfOffsetGeoIds) {
            const Part::Geometry* geo = Obj->getGeometry(geoId);
            if (geo && GeometryFacade::getConstruction(geo)) {
                ++constructionCount;
            }
        }

        return constructionCount;
    }

    double cross2d(const Base::Vector3d& first, const Base::Vector3d& second) const
    {
        return first.x * second.y - first.y * second.x;
    }

    double dot2d(const Base::Vector3d& first, const Base::Vector3d& second) const
    {
        return first.x * second.x + first.y * second.y;
    }

    bool pointOnLineSegment(
        const Base::Vector3d& point,
        const Base::Vector3d& start,
        const Base::Vector3d& end
    )
    {
        const Base::Vector3d segment = end - start;
        const Base::Vector3d toPoint = point - start;
        if (fabs(cross2d(segment, toPoint)) >= Precision::Confusion()) {
            return false;
        }

        const double projection = dot2d(toPoint, segment);
        const double segmentLengthSquared = dot2d(segment, segment);
        return projection > Precision::Confusion()
            && projection < segmentLengthSquared - Precision::Confusion();
    }

    bool lineSegmentsShareEndpoint(
        const Base::Vector3d& firstStart,
        const Base::Vector3d& firstEnd,
        const Base::Vector3d& secondStart,
        const Base::Vector3d& secondEnd
    )
    {
        return pointsCoincident(firstStart, secondStart) || pointsCoincident(firstStart, secondEnd)
            || pointsCoincident(firstEnd, secondStart) || pointsCoincident(firstEnd, secondEnd);
    }

    bool lineSegmentsIntersect(
        const Base::Vector3d& firstStart,
        const Base::Vector3d& firstEnd,
        const Base::Vector3d& secondStart,
        const Base::Vector3d& secondEnd
    )
    {
        if (lineSegmentsShareEndpoint(firstStart, firstEnd, secondStart, secondEnd)) {
            return false;
        }

        const Base::Vector3d firstDirection = firstEnd - firstStart;
        const Base::Vector3d secondDirection = secondEnd - secondStart;
        const double denom = cross2d(firstDirection, secondDirection);

        if (fabs(denom) < Precision::Confusion()) {
            return false;
        }

        const Base::Vector3d delta = secondStart - firstStart;
        const double firstParam = cross2d(delta, secondDirection) / denom;
        const double secondParam = cross2d(delta, firstDirection) / denom;

        return firstParam > Precision::Confusion() && firstParam < 1.0 - Precision::Confusion()
            && secondParam > Precision::Confusion() && secondParam < 1.0 - Precision::Confusion();
    }

    bool lineSegmentsOverlap(
        const Base::Vector3d& firstStart,
        const Base::Vector3d& firstEnd,
        const Base::Vector3d& secondStart,
        const Base::Vector3d& secondEnd
    )
    {
        const Base::Vector3d firstDirection = firstEnd - firstStart;
        if (fabs(cross2d(firstDirection, secondStart - firstStart)) >= Precision::Confusion()
            || fabs(cross2d(firstDirection, secondEnd - firstStart)) >= Precision::Confusion()) {
            return false;
        }

        return pointOnLineSegment(secondStart, firstStart, firstEnd)
            || pointOnLineSegment(secondEnd, firstStart, firstEnd)
            || pointOnLineSegment(firstStart, secondStart, secondEnd)
            || pointOnLineSegment(firstEnd, secondStart, secondEnd);
    }

    bool getLineSegmentPoints(int geoId, Base::Vector3d& startPoint, Base::Vector3d& endPoint)
    {
        const Part::Geometry* geo = sketchgui->getSketchObject()->getGeometry(geoId);
        if (!geo || !isLineSegment(*geo)) {
            return false;
        }

        const auto* line = static_cast<const Part::GeomLineSegment*>(geo);
        startPoint = line->getStartPoint();
        endPoint = line->getEndPoint();
        return true;
    }

    int countSelfIntersections(const std::vector<int>& listOfOffsetGeoIds)
    {
        int intersectionCount = 0;
        for (size_t i = 0; i < listOfOffsetGeoIds.size(); ++i) {
            Base::Vector3d firstStart;
            Base::Vector3d firstEnd;
            if (!getLineSegmentPoints(listOfOffsetGeoIds[i], firstStart, firstEnd)) {
                continue;
            }

            for (size_t j = i + 1; j < listOfOffsetGeoIds.size(); ++j) {
                Base::Vector3d secondStart;
                Base::Vector3d secondEnd;
                if (!getLineSegmentPoints(listOfOffsetGeoIds[j], secondStart, secondEnd)) {
                    continue;
                }

                if (lineSegmentsIntersect(firstStart, firstEnd, secondStart, secondEnd)) {
                    ++intersectionCount;
                }
            }
        }

        return intersectionCount;
    }

    int countOverlappingOffsetEdges(const std::vector<int>& listOfOffsetGeoIds)
    {
        int overlapCount = 0;
        for (size_t i = 0; i < listOfOffsetGeoIds.size(); ++i) {
            Base::Vector3d firstStart;
            Base::Vector3d firstEnd;
            if (!getLineSegmentPoints(listOfOffsetGeoIds[i], firstStart, firstEnd)) {
                continue;
            }

            for (size_t j = i + 1; j < listOfOffsetGeoIds.size(); ++j) {
                Base::Vector3d secondStart;
                Base::Vector3d secondEnd;
                if (!getLineSegmentPoints(listOfOffsetGeoIds[j], secondStart, secondEnd)) {
                    continue;
                }

                if (lineSegmentsOverlap(firstStart, firstEnd, secondStart, secondEnd)) {
                    ++overlapCount;
                }
            }
        }

        return overlapCount;
    }

    bool sourceSelectionContainsClosedCurve()
    {
        for (auto& curve : vCC) {
            if (curve.size() == 1) {
                const Part::Geometry* geo = sketchgui->getSketchObject()->getGeometry(curve.front());
                if (geo && (isCircle(*geo) || isEllipse(*geo))) {
                    return true;
                }
            }

            if (isCurveClosed(curve)) {
                return true;
            }
        }

        return false;
    }

    QString validateOffsetProfile(const std::vector<int>& listOfOffsetGeoIds)
    {
        QStringList issues;
        const std::vector<OffsetEndpoint> endpoints = getOffsetEndpoints(listOfOffsetGeoIds);

        int duplicateCount = countDuplicateGeometries(listOfOffsetGeoIds);
        if (duplicateCount > 0) {
            issues << tr("%1 duplicate offset geometries were created.").arg(duplicateCount);
        }

        int openVertexCount = countOpenVertices(endpoints);
        if (openVertexCount > 0 && sourceSelectionContainsClosedCurve()) {
            issues << tr("%1 open vertices remain in the closed offset profile.").arg(openVertexCount);
        }

        int missingCoincidentCount = countMissingCoincidentConstraints(endpoints);
        if (missingCoincidentCount > 0) {
            issues << tr("%1 connected offset vertices are missing coincident or tangent constraints.")
                          .arg(missingCoincidentCount);
        }

        int selfIntersectionCount = countSelfIntersections(listOfOffsetGeoIds);
        if (selfIntersectionCount > 0) {
            issues << tr("%1 self-intersections were detected between offset line segments.")
                          .arg(selfIntersectionCount);
        }

        int overlappingEdgeCount = countOverlappingOffsetEdges(listOfOffsetGeoIds);
        if (overlappingEdgeCount > 0) {
            issues << tr("%1 overlapping offset line segment pairs were detected.")
                          .arg(overlappingEdgeCount);
        }

        int constructionCount = countConstructionOffsetGeometry(listOfOffsetGeoIds);
        if (constructionCount > 0) {
            issues << tr("%1 generated offset geometries are still marked as construction geometry.")
                          .arg(constructionCount);
        }

        return issues.join(QStringLiteral("\n"));
    }

    std::vector<std::vector<int>> generatevCC(std::vector<int>& listOfGeo)
    {
        // This function separates all the selected geometries into separate continuous curves.
        SketchObject* Obj = sketchgui->getSketchObject();
        std::vector<std::vector<int>> vcc;

        for (auto geoId : listOfGeo) {
            std::vector<int> vecOfGeoIds;
            const Part::Geometry* geo = Obj->getGeometry(geoId);
            if (isCircle(*geo) || isEllipse(*geo)) {
                vecOfGeoIds.push_back(geoId);
                vcc.push_back(vecOfGeoIds);
                continue;
            }

            bool inserted = false;
            int insertedIn = -1;
            for (size_t j = 0; j < vcc.size(); j++) {
                for (size_t k = 0; k < vcc[j].size(); k++) {
                    if (!areCoincident(geoId, vcc[j][k])) {
                        continue;
                    }

                    if (inserted && insertedIn != int(j)) {
                        // if it's already inserted in another continuous curve then we need
                        // to merge both curves together. There're 2 cases, it could have
                        // been inserted at the end or at the beginning.
                        if (vcc[insertedIn][0] == geoId) {
                            // Two cases. Either the coincident is at the beginning or at
                            // the end.
                            if (k == 0) {
                                std::reverse(vcc[j].begin(), vcc[j].end());
                            }
                            vcc[j].insert(vcc[j].end(), vcc[insertedIn].begin(), vcc[insertedIn].end());
                            vcc.erase(vcc.begin() + insertedIn);
                        }
                        else {
                            if (k != 0) {  // ie k is  vcc[j].size()-1
                                std::reverse(vcc[j].begin(), vcc[j].end());
                            }
                            vcc[insertedIn].insert(vcc[insertedIn].end(), vcc[j].begin(), vcc[j].end());
                            vcc.erase(vcc.begin() + j);
                        }
                        j--;
                    }
                    else {
                        // we need to get the curves in the correct order.
                        if (k == vcc[j].size() - 1) {
                            vcc[j].push_back(geoId);
                        }
                        else {
                            // in this case k should actually be 0.
                            vcc[j].insert(vcc[j].begin() + k, geoId);
                        }
                        insertedIn = j;
                        inserted = true;
                    }
                    // printCCeVec();
                    break;
                }
            }
            if (!inserted) {
                vecOfGeoIds.push_back(geoId);
                vcc.push_back(vecOfGeoIds);
            }
        }
        return vcc;
    }

    void generateSourceWires()
    {
        vCC = generatevCC(listOfGeoIds);

        SketchObject* Obj = sketchgui->getSketchObject();
        sourceWires.clear();
        onlySingleLines = true;

        for (auto& CC : vCC) {
            std::list<TopoDS_Edge> edgeList;
            for (auto& curveId : CC) {
                const Part::Geometry* pGeo = Obj->getGeometry(curveId);
                if (!pGeo) {
                    continue;
                }

                auto geoCopy = std::unique_ptr<Part::Geometry>(pGeo->copy());
                Part::Geometry* geo = geoCopy.get();
                geo->reverseIfReversed();

                edgeList.push_back(TopoDS::Edge(geo->toShape()));
            }

            if (edgeList.empty()) {
                continue;
            }

            BRepBuilderAPI_MakeWire mkWire;
            mkWire.Add(edgeList.front());
            edgeList.pop_front();
            TopoDS_Wire wire = mkWire.Wire();

            bool found = true;
            while (found && !edgeList.empty()) {
                found = false;
                for (auto it = edgeList.begin(); it != edgeList.end(); ++it) {
                    mkWire.Add(*it);
                    if (mkWire.Error() != BRepBuilderAPI_DisconnectedWire) {
                        found = true;
                        edgeList.erase(it);
                        wire = mkWire.Wire();
                        break;
                    }
                }
            }

            if (!edgeList.empty()) {
                Base::Console().warning(
                    "Sketcher offset: skipped a disconnected source chain with %zu edges.\n",
                    edgeList.size()
                );
                continue;
            }

            ShapeFix_Wire wireFixer;
            wireFixer.SetPrecision(Precision::Confusion());
            wireFixer.Load(wire);
            wireFixer.FixReorder();
            wireFixer.FixConnected(Precision::Confusion());
            if (wire.Closed() || isCurveClosed(CC)) {
                wireFixer.FixClosed(Precision::Confusion());
            }
            wire = wireFixer.Wire();

            // Fix orientation: ensure all closed wires are CCW relative to Sketch Plane (+Z)
            if (wire.Closed()) {
                BRepBuilderAPI_MakeFace mkFace(wire);
                if (mkFace.IsDone()) {
                    TopoDS_Face face = mkFace.Face();
                    BRepAdaptor_Surface surf(face);
                    if (surf.GetType() == GeomAbs_Plane) {
                        gp_Dir norm = surf.Plane().Axis().Direction();
                        if (norm.Z() < 0) {
                            wire.Reverse();
                        }
                    }
                }
            }

            if (CC.size() == 1 && isLineSegment(*Obj->getGeometry(CC[0]))) {
                sourceWires.push_back(wire);
            }
            else {
                sourceWires.insert(sourceWires.begin(), wire);
                onlySingleLines = false;
            }
        }
    }

    bool getNearestPointAndTangent(
        int geoId,
        const Base::Vector3d& cursor,
        Base::Vector3d& nearestPoint,
        Base::Vector3d& tangent
    )
    {
        const Part::Geometry* geo = sketchgui->getSketchObject()->getGeometry(geoId);
        if (!geo) {
            return false;
        }

        if (isLineSegment(*geo)) {
            const auto* line = static_cast<const Part::GeomLineSegment*>(geo);
            const Base::Vector3d startPoint = line->getStartPoint();
            const Base::Vector3d endPoint = line->getEndPoint();
            tangent = endPoint - startPoint;

            const double lengthSquared = dot2d(tangent, tangent);
            if (lengthSquared < Precision::Confusion()) {
                return false;
            }

            const double parameter =
                std::clamp(dot2d(cursor - startPoint, tangent) / lengthSquared, 0.0, 1.0);
            nearestPoint = Base::Vector3d(
                startPoint.x + tangent.x * parameter,
                startPoint.y + tangent.y * parameter,
                startPoint.z + tangent.z * parameter
            );
            return true;
        }

        if (isArcOfCircle(*geo)) {
            const auto* arc = static_cast<const Part::GeomArcOfCircle*>(geo);
            const Base::Vector3d center = arc->getCenter();
            Base::Vector3d radial = cursor - center;
            const double radialLength = std::sqrt(dot2d(radial, radial));
            if (radialLength < Precision::Confusion()) {
                radial = arc->getStartPoint(true) - center;
            }

            const double fallbackRadialLength = std::sqrt(dot2d(radial, radial));
            if (fallbackRadialLength < Precision::Confusion()) {
                return false;
            }

            nearestPoint = Base::Vector3d(
                center.x + radial.x * arc->getRadius() / fallbackRadialLength,
                center.y + radial.y * arc->getRadius() / fallbackRadialLength,
                center.z
            );

            Base::Vector3d ccwTangent {-radial.y, radial.x, 0.0};
            const Base::Vector3d chord = arc->getEndPoint(true) - arc->getStartPoint(true);
            if (dot2d(ccwTangent, chord) < 0.0) {
                ccwTangent.x = -ccwTangent.x;
                ccwTangent.y = -ccwTangent.y;
            }
            tangent = ccwTangent;
            return true;
        }

        return false;
    }

    double openCurveOffsetSign(const Base::Vector2d& cursorPoint)
    {
        const Base::Vector3d cursor {cursorPoint.x, cursorPoint.y, 0.0};

        bool foundNearest = false;
        double nearestDistanceSquared = std::numeric_limits<double>::max();
        double nearestCross = 0.0;

        for (int geoId : listOfGeoIds) {
            Base::Vector3d nearestPoint;
            Base::Vector3d tangent;
            if (!getNearestPointAndTangent(geoId, cursor, nearestPoint, tangent)) {
                continue;
            }

            const Base::Vector3d toCursor = cursor - nearestPoint;
            const double distanceSquared = dot2d(toCursor, toCursor);
            if (distanceSquared < nearestDistanceSquared) {
                nearestDistanceSquared = distanceSquared;
                nearestCross = cross2d(tangent, toCursor);
                foundNearest = true;
            }
        }

        if (!foundNearest || fabs(nearestCross) < Precision::Confusion()) {
            return 1.0;
        }

        // OCC positive open-wire offsets are on the right side of the ordered source chain.
        return nearestCross < 0.0 ? 1.0 : -1.0;
    }

    void findOffsetLength()
    {
        double newOffsetLength = std::numeric_limits<double>::max();
        double nearestDistance = std::numeric_limits<double>::max();

        BRepBuilderAPI_MakeVertex mkVertex({endpoint.x, endpoint.y, 0.0});
        TopoDS_Vertex vertex = mkVertex.Vertex();
        for (auto& wire : sourceWires) {
            BRepExtrema_DistShapeShape distTool(wire, vertex);
            if (distTool.IsDone()) {
                double distance = distTool.Value();
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    newOffsetLength = distance;

                    gp_Pnt pnt = distTool.PointOnShape1(1);
                    pointOnSourceWire = Base::Vector2d(pnt.X(), pnt.Y());

                    // find direction
                    if (BRep_Tool::IsClosed(wire)) {
                        TopoDS_Face aFace = BRepBuilderAPI_MakeFace(wire);
                        BRepClass_FaceClassifier checkPoint(
                            aFace,
                            {endpoint.x, endpoint.y, 0.0},
                            Precision::Confusion()
                        );
                        if (checkPoint.State() == TopAbs_IN) {
                            newOffsetLength = -newOffsetLength;
                        }
                    }
                    else {
                        newOffsetLength *= openCurveOffsetSign(endpoint);
                    }
                }
            }
        }

        if (newOffsetLength != std::numeric_limits<double>::max()) {
            offsetLength = newOffsetLength;
        }
    }

    bool getFirstSecondPoints(int geoId, Base::Vector3d& startPoint, Base::Vector3d& endPoint)
    {
        const Part::Geometry* geo = sketchgui->getSketchObject()->getGeometry(geoId);

        if (isLineSegment(*geo)) {
            const auto* line = static_cast<const Part::GeomLineSegment*>(geo);
            startPoint = line->getStartPoint();
            endPoint = line->getEndPoint();
            return true;
        }
        else if (
            isArcOfCircle(*geo) || isArcOfEllipse(*geo) || isArcOfHyperbola(*geo)
            || isArcOfParabola(*geo)
        ) {
            const auto* arcOfConic = static_cast<const Part::GeomArcOfConic*>(geo);
            startPoint = arcOfConic->getStartPoint(true);
            endPoint = arcOfConic->getEndPoint(true);
            return true;
        }
        else if (isBSplineCurve(*geo)) {
            const auto* bSpline = static_cast<const Part::GeomBSplineCurve*>(geo);
            startPoint = bSpline->getStartPoint();
            endPoint = bSpline->getEndPoint();
            return true;
        }
        return false;
    }

    CoincidencePointPos checkForCoincidence(int geoId1, int geoId2, bool tangentOnly = false)
    {
        // This function looks up for 2 coincidence between 2 edges (arc + line can have 2)
        SketchObject* Obj = sketchgui->getSketchObject();
        const std::vector<Constraint*>& vals = Obj->Constraints.getValues();
        CoincidencePointPos positions;
        positions.firstPos1 = PointPos::none;
        positions.secondPos1 = PointPos::none;
        positions.firstPos2 = PointPos::none;
        positions.secondPos2 = PointPos::none;
        bool firstCoincidenceFound = false;
        for (auto* cstr : vals) {
            if (((tangentOnly || cstr->Type != Coincident) && cstr->Type != Tangent)
                || cstr->FirstPos == PointPos::mid || cstr->FirstPos == PointPos::none
                || cstr->SecondPos == PointPos::mid || cstr->SecondPos == PointPos::none) {
                continue;
            }

            if ((cstr->First == geoId1 && cstr->Second == geoId2)
                || (cstr->First == geoId2 && cstr->Second == geoId1)) {
                if (!firstCoincidenceFound) {
                    positions.firstPos1 = cstr->First == geoId1 ? cstr->FirstPos : cstr->SecondPos;
                    positions.secondPos1 = cstr->First == geoId2 ? cstr->FirstPos : cstr->SecondPos;
                    firstCoincidenceFound = true;
                }
                else {
                    positions.firstPos2 = cstr->First == geoId1 ? cstr->FirstPos : cstr->SecondPos;
                    positions.secondPos2 = cstr->First == geoId2 ? cstr->FirstPos : cstr->SecondPos;
                    break;
                }
            }
        }
        return positions;
    }

    bool areCoincident(int geoId1, int geoId2)
    {
        // Instead of checking for constraints like so:
        // CoincidencePointPos ppc = checkForCoincidence(geoId1, geoId2);
        // return ppc.firstPos1 != PointPos::none;
        // we are going to check if the points are effectively coincident:

        Base::Vector3d p11, p12, p21, p22;
        if (!getFirstSecondPoints(geoId1, p11, p12) || !getFirstSecondPoints(geoId2, p21, p22)) {
            return false;
        }

        return (
            (p11 - p21).Length() < Precision::Confusion()
            || (p11 - p22).Length() < Precision::Confusion()
            || (p12 - p21).Length() < Precision::Confusion()
            || (p12 - p22).Length() < Precision::Confusion()
        );
    }

    bool areTangentCoincident(int geoId1, int geoId2)
    {
        CoincidencePointPos ppc = checkForCoincidence(geoId1, geoId2, true);
        return ppc.firstPos1 != PointPos::none;
    }

    bool isCurveClosed(std::vector<int>& curve)
    {
        bool closed = false;
        if (curve.size() > 2) {
            closed = areCoincident(curve[0], curve[curve.size() - 1]);
        }
        else if (curve.size() == 2) {
            // if only 2 elements, we need to check if they close end to end.
            CoincidencePointPos cpp = checkForCoincidence(curve[0], curve[curve.size() - 1]);
            closed = cpp.firstPos1 != PointPos::none && cpp.firstPos2 != PointPos::none;
        }
        return closed;
    }

    // debug only
    /*void printCCeVec()
    {
        for (size_t j = 0; j < vCC.size(); j++) {
            Base::Console().warning("curve %d{", j);
            for (size_t k = 0; k < vCC[j].size(); k++) {
                Base::Console().warning("%d, ", vCC[j][k]);
            }
            Base::Console().warning("}\n");
        }
    }*/
};

template<>
auto DSHOffsetControllerBase::getState(int labelindex) const
{
    switch (labelindex) {
        case OnViewParameter::First:
            return SelectMode::SeekFirst;
            break;
        default:
            THROWM(Base::ValueError, "Parameter index without an associated machine state")
    }
}

template<>
void DSHOffsetController::configureToolWidget()
{
    if (!init) {  // Code to be executed only upon initialisation
        QStringList names = {
            QApplication::translate("Sketcher_CreateOffset", "Arc"),
            QApplication::translate("Sketcher_CreateOffset", "Intersection"),
            QApplication::translate("Sketcher_CreateOffset", "Constrained Clearance")
        };
        toolWidget->setComboboxElements(WCombobox::FirstCombo, names);

        toolWidget->setComboboxItemIcon(
            WCombobox::FirstCombo,
            0,
            Gui::BitmapFactory().iconFromTheme("Sketcher_OffsetArc")
        );
        toolWidget->setComboboxItemIcon(
            WCombobox::FirstCombo,
            1,
            Gui::BitmapFactory().iconFromTheme("Sketcher_OffsetIntersection")
        );
        toolWidget->setComboboxItemIcon(
            WCombobox::FirstCombo,
            2,
            Gui::BitmapFactory().iconFromTheme("Sketcher_OffsetArc")
        );

        toolWidget->setCheckboxLabel(
            WCheckbox::FirstBox,
            QApplication::translate("TaskSketcherTool_c1_offset", "Delete original geometries (U)")
        );
        toolWidget->setCheckboxLabel(
            WCheckbox::SecondBox,
            QApplication::translate("TaskSketcherTool_c2_offset", "Add offset constraint (J)")
        );
        toolWidget->setCheckboxLabel(
            WCheckbox::ThirdBox,
            QApplication::translate("TaskSketcherTool_c3_offset", "Chain link")
        );
        toolWidget->setCheckboxToolTip(
            WCheckbox::FirstBox,
            QApplication::translate(
                "TaskSketcherTool_c1_offset",
                "Deletes the original geometry. If creating a single copy, this effectively "
                "performs a 'Move' operation."
            )
        );
        toolWidget->setCheckboxToolTip(
            WCheckbox::SecondBox,
            QApplication::translate(
                "TaskSketcherTool_c2_offset",
                "Adds a distance constraint with additional construction geometries that allows "
                "the distance to modify the entire offset geometry"

            )
        );
        toolWidget->setCheckboxToolTip(
            WCheckbox::ThirdBox,
            QApplication::translate(
                "TaskSketcherTool_c3_offset",
                "Automatically follows connected source edges and offsets the full connected chain"
            )
        );
    }

    if (handler->isConstrainedClearanceMode()) {
        handler->deleteOriginal = false;
    }

    syncCheckboxToHandler(WCheckbox::FirstBox, handler->deleteOriginal);
    syncCheckboxToHandler(
        WCheckbox::SecondBox,
        handler->offsetConstraint || handler->isConstrainedClearanceMode()
    );
    syncCheckboxToHandler(WCheckbox::ThirdBox, handler->chainLink);

    onViewParameters[OnViewParameter::First]->setLabelType(
        Gui::SoDatumLabel::DISTANCE,
        Gui::EditableDatumLabel::Function::Forced
    );
}

template<>
void DSHOffsetController::adaptDrawingToComboboxChange(int comboboxindex, int value)
{
    if (comboboxindex == WCombobox::FirstCombo && handler->ConstructionMethodsCount() > 1) {
        handler->setConstructionMethod(static_cast<ConstructionMethod>(value));

        if (handler->isConstrainedClearanceMode()) {
            handler->deleteOriginal = false;
        }

        handler->refreshSourceGeometry();
    }
}

template<>
void DSHOffsetControllerBase::adaptDrawingToOnViewParameterChange(int labelindex, double value)
{
    switch (labelindex) {
        case OnViewParameter::First: {
            if (value == 0. && onViewParameters[OnViewParameter::First]->hasFinishedEditing) {
                // Do not accept 0, but only if user has finished editing the OVP.
                unsetOnViewParameter(onViewParameters[OnViewParameter::First].get());

                // reset offsetLengthSet so mouse can control the offset again
                handler->offsetLengthSet = false;

                Gui::NotifyUserError(
                    handler->sketchgui->getSketchObject(),
                    QT_TRANSLATE_NOOP("Notifications", "Invalid Value"),
                    QT_TRANSLATE_NOOP("Notifications", "Offset value can't be 0.")
                );
            }
            else {
                handler->offsetLengthSet = true;
                handler->offsetLength = value;
            }
        } break;
        default:
            break;
    }
}

template<>
void DSHOffsetController::adaptDrawingToCheckboxChange(int checkboxindex, bool value)
{
    switch (checkboxindex) {
        case WCheckbox::FirstBox:
            if (handler->isConstrainedClearanceMode() && value) {
                handler->deleteOriginal = false;
                toolWidget->setCheckboxChecked(WCheckbox::FirstBox, false);
                break;
            }

            handler->deleteOriginal = value;

            // Both options cannot be enabled at the same time.
            if (value && toolWidget->getCheckboxChecked(WCheckbox::SecondBox)) {
                toolWidget->setCheckboxChecked(WCheckbox::SecondBox, false);
            }
            break;

        case WCheckbox::SecondBox:
            if (handler->isConstrainedClearanceMode() && !value) {
                toolWidget->setCheckboxChecked(WCheckbox::SecondBox, true);
                break;
            }

            handler->offsetConstraint = value;

            // Both options cannot be enabled at the same time.
            if (value && toolWidget->getCheckboxChecked(WCheckbox::FirstBox)) {
                toolWidget->setCheckboxChecked(WCheckbox::FirstBox, false);
            }
            break;

        case WCheckbox::ThirdBox:
            handler->chainLink = value;
            handler->refreshSourceGeometry();
            break;
    }
}

/* doEnforceControlParameters : The tool validates after offset length is set. So we don't need to
 * enforce it. Besides it is hard to override onsketchpos such that it is at offsetLength from the
 * curve. As we do not override the pos, we need to use offsetLengthSet to prevent rewrite of
 * offsetLength.*/

template<>
void DSHOffsetController::adaptParameters(Base::Vector2d onSketchPos)
{
    Q_UNUSED(onSketchPos)

    switch (handler->state()) {
        case SelectMode::SeekFirst: {
            auto& firstParam = onViewParameters[OnViewParameter::First];

            if (!firstParam->isSet) {
                setOnViewParameterValue(OnViewParameter::First, handler->offsetLength);
            }

            Base::Vector3d dimensionEndpoint;
            if (handler->offsetLengthSet && firstParam->isSet) {
                // if user has typed a value, calculate correct endpoint based on typed value
                Base::Vector2d direction = handler->endpoint - handler->pointOnSourceWire;
                if (direction.Length() > Precision::Confusion()) {
                    direction.Normalize();
                    Base::Vector2d correctedEndpoint = handler->pointOnSourceWire
                        + direction * handler->offsetLength;
                    dimensionEndpoint = Base::Vector3d(correctedEndpoint.x, correctedEndpoint.y, 0.);
                }
                else {
                    dimensionEndpoint = Base::Vector3d(handler->endpoint.x, handler->endpoint.y, 0.);
                }
            }
            else {
                // use mouse pos when user hasn't typed a value
                dimensionEndpoint = Base::Vector3d(handler->endpoint.x, handler->endpoint.y, 0.);
            }

            firstParam->setPoints(
                dimensionEndpoint,
                Base::Vector3d(handler->pointOnSourceWire.x, handler->pointOnSourceWire.y, 0.)
            );
        } break;
        default:
            break;
    }
}

template<>
void DSHOffsetController::computeNextDrawSketchHandlerMode()
{
    switch (handler->state()) {
        case SelectMode::SeekFirst: {
            auto& firstParam = onViewParameters[OnViewParameter::First];

            if (firstParam->hasFinishedEditing) {
                handler->setNextState(SelectMode::End);
            }
        } break;
        default:
            break;
    }
}


}  // namespace SketcherGui
