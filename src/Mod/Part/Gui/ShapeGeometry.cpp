// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2011 Juergen Riegel <juergen.riegel@web.de>

#include "ShapeGeometry.h"

#include <Base/Console.h>
#include <Base/TimeInfo.h>
#include <Base/Tools.h>
#include <Mod/Part/App/ShapeMapHasher.h>
#include <Mod/Part/App/Tools.h>

#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <gp_Trsf.hxx>
#include <Precision.hxx>
#include <Poly_Array1OfTriangle.hxx>
#include <Poly_Polygon3D.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Version.hxx>
#include <TColgp_Array1OfDir.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <cmath>
#include <exception>
#include <map>
#include <set>
#include <utility>

FC_LOG_LEVEL_INIT("Part", true, true)

namespace PartGui
{
namespace
{

ShapeGeometry::Vector toFloatVector(const gp_Pnt& point)
{
    return {static_cast<float>(point.X()), static_cast<float>(point.Y()),
            static_cast<float>(point.Z())};
}

void addNormal(ShapeGeometry::Vector& target, const gp_Vec& normal)
{
    target[0] += static_cast<float>(normal.X());
    target[1] += static_cast<float>(normal.Y());
    target[2] += static_cast<float>(normal.Z());
}

void tessellateShapeUnchecked(
    TopoDS_Shape shape,
    ShapeGeometry& geometry,
    double deviation,
    double angularDeflection,
    bool normalsFromUV)
{
    if (Part::Tools::isShapeEmpty(shape)) {
        return;
    }

    // time measurement and book keeping
    Base::TimeElapsed startTime;

    [[maybe_unused]]
    int numTriangles = 0,
        numNodes = 0, numNorms = 0, numFaces = 0, numEdges = 0, numLines = 0;

    std::set<int> faceEdges;

    // calculating the deflection value
    Standard_Real deflection = Part::Tools::getDeflection(shape, deviation);

    // Since OCCT 7.6 a value of equal 0 is not allowed any more, this can happen if a single
    // vertex should be displayed.
    if (deflection < gp::Resolution()) {
        deflection = Precision::Confusion();
    }

    // For very big objects the computed deflection can become very high and thus leads to a
    // useless tessellation. To avoid this the upper limit is set to 20.0 See also forum:
    // https://forum.freecad.org/viewtopic.php?t=77521
    // deflection = std::min(deflection, 20.0);

    // create or use the mesh on the data structure
    Standard_Real AngDeflectionRads = Base::toRadians(angularDeflection);

    IMeshTools_Parameters meshParams;
    meshParams.Deflection = deflection;
    meshParams.Relative = Standard_False;
    meshParams.Angle = AngDeflectionRads;
    meshParams.InParallel = Standard_True;
    meshParams.AllowQualityDecrease = Standard_True;

    // Clear triangulation and PCurves from geometry which can slow down the process
#if OCC_VERSION_HEX < 0x070600
    BRepTools::Clean(shape);
#else
    BRepTools::Clean(shape, Standard_True);
#endif

    BRepMesh_IncrementalMesh(shape, meshParams);

    // We must reset the location here because the transformation data
    // are set in the placement property
    TopLoc_Location aLoc;
    shape.Location(aLoc);

    // count triangles and nodes in the mesh
    TopTools_IndexedMapOfShape faceMap;
    TopExp::MapShapes(shape, TopAbs_FACE, faceMap);
    for (int i = 1; i <= faceMap.Extent(); i++) {
        Handle(Poly_Triangulation) mesh = BRep_Tool::Triangulation(TopoDS::Face(faceMap(i)), aLoc);

        if (mesh.IsNull()) {
            mesh = Part::Tools::triangulationOfFace(TopoDS::Face(faceMap(i)));
        }

        // Note: we must also count empty faces
        if (!mesh.IsNull()) {
            numTriangles += mesh->NbTriangles();
            numNodes += mesh->NbNodes();
            numNorms += mesh->NbNodes();
        }

        TopExp_Explorer xp;
        for (xp.Init(faceMap(i), TopAbs_EDGE); xp.More(); xp.Next()) {
            faceEdges.insert(Part::ShapeMapHasher {}(xp.Current()));
        }
        numFaces++;
    }

    // get an indexed map of edges
    TopTools_IndexedMapOfShape edgeMap;
    TopExp::MapShapes(shape, TopAbs_EDGE, edgeMap);

    // key is the edge number, value the coord indexes. This is needed to keep the same order as
    // the edges.
    std::map<int, std::vector<int32_t>> lineSetMap;
    std::set<int> edgeIdxSet;
    std::set<int> edgeFailed;
    std::vector<int32_t> edgeVector;

    // count and index the edges
    for (int i = 1; i <= edgeMap.Extent(); i++) {
        edgeIdxSet.insert(i);
        numEdges++;

        const TopoDS_Edge& aEdge = TopoDS::Edge(edgeMap(i));
        TopLoc_Location aLoc;

        // handling of the free edge that are not associated to a face
        // Note: The assumption that if for an edge BRep_Tool::Polygon3D
        // returns a valid object is wrong. This e.g. happens for ruled
        // surfaces which gets created by two edges or wires.
        // So, we have to store the hashes of the edges associated to a face.
        // If the hash of a given edge is not in this list we know it's really
        // a free edge.
        int hash = Part::ShapeMapHasher {}(aEdge);
        if (faceEdges.find(hash) == faceEdges.end()) {
            Handle(Poly_Polygon3D) aPoly = Part::Tools::polygonOfEdge(aEdge, aLoc);
            if (!aPoly.IsNull()) {
                int nbNodesInEdge = aPoly->NbNodes();
                numNodes += nbNodesInEdge;
            }
        }
    }

    // handling of the vertices
    TopTools_IndexedMapOfShape vertexMap;
    TopExp::MapShapes(shape, TopAbs_VERTEX, vertexMap);
    numNodes += vertexMap.Extent();

    geometry.vertices.resize(numNodes);
    geometry.normals.resize(numNorms);
    geometry.faceIndices.resize(numTriangles * 4);
    geometry.faceParts.resize(numFaces);
    auto* verts = geometry.vertices.data();
    auto* norms = geometry.normals.data();
    auto* index = geometry.faceIndices.data();
    auto* parts = geometry.faceParts.data();

    int ii = 0, faceNodeOffset = 0, faceTriaOffset = 0;
    for (int i = 1; i <= faceMap.Extent(); i++, ii++) {
        TopLoc_Location aLoc;
        const TopoDS_Face& actFace = TopoDS::Face(faceMap(i));
        // get the mesh of the shape
        Handle(Poly_Triangulation) mesh = BRep_Tool::Triangulation(actFace, aLoc);
        if (mesh.IsNull()) {
            mesh = Part::Tools::triangulationOfFace(actFace);
        }
        if (mesh.IsNull()) {
            parts[ii] = 0;
            continue;
        }

        // getting the transformation of the shape/face
        gp_Trsf myTransf;
        Standard_Boolean identity = true;
        if (!aLoc.IsIdentity()) {
            identity = false;
            myTransf = aLoc.Transformation();
        }

        // getting size of node and triangle array of this face
        int nbNodesInFace = mesh->NbNodes();
        int nbTriInFace = mesh->NbTriangles();
        // check orientation
        TopAbs_Orientation orient = actFace.Orientation();

        // cycling through the poly mesh
#if OCC_VERSION_HEX < 0x070600
        const Poly_Array1OfTriangle& Triangles = mesh->Triangles();
        const TColgp_Array1OfPnt& Nodes = mesh->Nodes();
        TColgp_Array1OfDir Normals(Nodes.Lower(), Nodes.Upper());
#else
        int numNodes = mesh->NbNodes();
        TColgp_Array1OfDir Normals(1, numNodes);
#endif
        if (normalsFromUV) {
            Part::Tools::getPointNormals(actFace, mesh, Normals);
        }

        for (int g = 1; g <= nbTriInFace; g++) {
            // Get the triangle
            Standard_Integer N1, N2, N3;
#if OCC_VERSION_HEX < 0x070600
            Triangles(g).Get(N1, N2, N3);
#else
            mesh->Triangle(g).Get(N1, N2, N3);
#endif

            // change orientation of the triangle if the face is reversed
            if (orient != TopAbs_FORWARD) {
                Standard_Integer tmp = N1;
                N1 = N2;
                N2 = tmp;
            }

            // get the 3 points of this triangle
#if OCC_VERSION_HEX < 0x070600
            gp_Pnt V1(Nodes(N1)), V2(Nodes(N2)), V3(Nodes(N3));
#else
            gp_Pnt V1(mesh->Node(N1)), V2(mesh->Node(N2)), V3(mesh->Node(N3));
#endif

            // get the 3 normals of this triangle
            gp_Vec NV1, NV2, NV3;
            if (normalsFromUV) {
                NV1.SetXYZ(Normals(N1).XYZ());
                NV2.SetXYZ(Normals(N2).XYZ());
                NV3.SetXYZ(Normals(N3).XYZ());
            }
            else {
                gp_Vec v1 = Base::convertTo<gp_Vec>(V1);
                gp_Vec v2 = Base::convertTo<gp_Vec>(V2);
                gp_Vec v3 = Base::convertTo<gp_Vec>(V3);

                gp_Vec normal = (v2 - v1) ^ (v3 - v1);
                NV1 = normal;
                NV2 = normal;
                NV3 = normal;
            }

            // transform the vertices and normals to the place of the face
            if (!identity) {
                V1.Transform(myTransf);
                V2.Transform(myTransf);
                V3.Transform(myTransf);
                if (normalsFromUV) {
                    NV1.Transform(myTransf);
                    NV2.Transform(myTransf);
                    NV3.Transform(myTransf);
                }
            }

            // add the normals for all points of this triangle
            addNormal(norms[faceNodeOffset + N1 - 1], NV1);
            addNormal(norms[faceNodeOffset + N2 - 1], NV2);
            addNormal(norms[faceNodeOffset + N3 - 1], NV3);

            // set the vertices
            verts[faceNodeOffset + N1 - 1] = toFloatVector(V1);
            verts[faceNodeOffset + N2 - 1] = toFloatVector(V2);
            verts[faceNodeOffset + N3 - 1] = toFloatVector(V3);

            // set the index vector with the 3 point indexes and the end delimiter
            index[faceTriaOffset * 4 + 4 * (g - 1)] = faceNodeOffset + N1 - 1;
            index[faceTriaOffset * 4 + 4 * (g - 1) + 1] = faceNodeOffset + N2 - 1;
            index[faceTriaOffset * 4 + 4 * (g - 1) + 2] = faceNodeOffset + N3 - 1;
            index[faceTriaOffset * 4 + 4 * (g - 1) + 3] = -1;
        }

        parts[ii] = nbTriInFace;  // new part

        // handling the edges lying on this face
        TopExp_Explorer Exp;
        for (Exp.Init(actFace, TopAbs_EDGE); Exp.More(); Exp.Next()) {
            const TopoDS_Edge& curEdge = TopoDS::Edge(Exp.Current());
            // get the overall index of this edge
            int edgeIndex = edgeMap.FindIndex(curEdge);
            edgeVector.push_back((int32_t)edgeIndex - 1);
            // already processed this index ?
            if (edgeIdxSet.find(edgeIndex) != edgeIdxSet.end()) {

                // this holds the indices of the edge's triangulation to the current polygon
                Handle(Poly_PolygonOnTriangulation)
                    aPoly = BRep_Tool::PolygonOnTriangulation(curEdge, mesh, aLoc);
                if (aPoly.IsNull()) {
                    edgeFailed.insert(edgeIndex);
                    continue;  // polygon does not exist
                }

                // getting the indexes of the edge polygon
                const TColStd_Array1OfInteger& indices = aPoly->Nodes();
                for (Standard_Integer i = indices.Lower(); i <= indices.Upper(); i++) {
                    int nodeIndex = indices(i);
                    int index = faceNodeOffset + nodeIndex - 1;
                    lineSetMap[edgeIndex].push_back(index);

                    // usually the coordinates for this edge are already set by the
                    // triangles of the face this edge belongs to. However, there are
                    // rare cases where some points are only referenced by the polygon
                    // but not by any triangle. Thus, we must apply the coordinates to
                    // make sure that everything is properly set.
#if OCC_VERSION_HEX < 0x070600
                    gp_Pnt p(Nodes(nodeIndex));
#else
                    gp_Pnt p(mesh->Node(nodeIndex));
#endif
                    if (!identity) {
                        p.Transform(myTransf);
                    }
                    verts[index] = toFloatVector(p);
                }

                // remove the handled edge index from the set
                edgeIdxSet.erase(edgeIndex);
            }
        }

        edgeVector.push_back(-1);

        // counting up the per Face offsets
        faceNodeOffset += nbNodesInFace;
        faceTriaOffset += nbTriInFace;
    }

    // handling of the free edges
    for (int i = 1; i <= edgeMap.Extent(); i++) {
        const TopoDS_Edge& aEdge = TopoDS::Edge(edgeMap(i));
        Standard_Boolean identity = true;
        gp_Trsf myTransf;
        TopLoc_Location aLoc;

        // handling of the free edge that are not associated to a face
        int hash = Part::ShapeMapHasher {}(aEdge);
        if (faceEdges.find(hash) == faceEdges.end()) {
            Handle(Poly_Polygon3D) aPoly = Part::Tools::polygonOfEdge(aEdge, aLoc);
            if (!aPoly.IsNull()) {
                if (!aLoc.IsIdentity()) {
                    identity = false;
                    myTransf = aLoc.Transformation();
                }

                const TColgp_Array1OfPnt& aNodes = aPoly->Nodes();
                int nbNodesInEdge = aPoly->NbNodes();

                gp_Pnt pnt;
                for (Standard_Integer j = 1; j <= nbNodesInEdge; j++) {
                    pnt = aNodes(j);
                    if (!identity) {
                        pnt.Transform(myTransf);
                    }
                    int index = faceNodeOffset + j - 1;
                    verts[index] = toFloatVector(pnt);
                    lineSetMap[i].push_back(index);
                }

                faceNodeOffset += nbNodesInEdge;
            }
        }
    }

    std::map<int, int> coordsMap;
    geometry.pointStartIndex = faceNodeOffset;
    for (int i = 0; i < vertexMap.Extent(); i++) {
        const TopoDS_Vertex& aVertex = TopoDS::Vertex(vertexMap(i + 1));
        gp_Pnt pnt = BRep_Tool::Pnt(aVertex);

        verts[faceNodeOffset + i] = toFloatVector(pnt);
        coordsMap[i + 1] = faceNodeOffset + i;
    }

    // normalize all normals
    for (int i = 0; i < numNorms; i++) {
        auto& normal = norms[i];
        const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1]
                                       + normal[2] * normal[2]);
        if (length > 0.0F) {
            const float scale = 1.0F / length;
            for (auto& component : normal) {
                component *= scale;
            }
        }
    }

    // If no adjacent face has a polygon for an edge, use its endpoint coordinates so that
    // the edge still has a line entry. Fall back to a zero-length line to preserve the edge
    // numbering when no usable polygon is available.
    for (int edgeIndex : edgeFailed) {
        if (lineSetMap.find(edgeIndex) != lineSetMap.end()) {
            continue;
        }

        int indexedPnt1 = faceNodeOffset;
        int indexedPnt2 = faceNodeOffset;
        TopoDS_Edge edge = TopoDS::Edge(edgeMap.FindKey(edgeIndex));
        Handle(Poly_Polygon3D) aPoly = Part::Tools::polygonOfEdge(edge, aLoc);
        if (!aPoly.IsNull() && aPoly->NbNodes() == 2) {
            const int v1 = vertexMap.FindIndex(TopExp::FirstVertex(edge));
            const int v2 = vertexMap.FindIndex(TopExp::LastVertex(edge));
            const auto it = coordsMap.find(v1);
            const auto jt = coordsMap.find(v2);
            if (it != coordsMap.end() && jt != coordsMap.end()) {
                indexedPnt1 = it->second;
                indexedPnt2 = jt->second;
            }
        }

        lineSetMap[edgeIndex].push_back(indexedPnt1);
        lineSetMap[edgeIndex].push_back(indexedPnt2);
    }

    // lineSetMap may omit edges that produced no usable polyline. The emitted polyline order
    // can therefore have gaps with respect to the topological edge numbering, so record the
    // real edge index of each polyline rather than inferring it from the line index later.
    std::vector<int32_t> lineSetCoords;
    std::vector<int> lineToEdge;
    lineToEdge.reserve(lineSetMap.size());
    for (const auto& it : lineSetMap) {
        lineSetCoords.insert(lineSetCoords.end(), it.second.begin(), it.second.end());
        lineSetCoords.push_back(-1);
        lineToEdge.push_back(it.first);
    }
    numLines = lineSetCoords.size();
    geometry.edgeIndices = std::move(lineSetCoords);
    geometry.lineToEdge = std::move(lineToEdge);


#ifdef FC_DEBUG
    Base::Console().log(
        "ViewProvider update time: {:f} s\n",
        Base::TimeElapsed::diffTimeF(startTime, Base::TimeElapsed())
    );
    Base::Console().log(
        "Shape mesh info: Faces:{} Edges:{} Nodes:{} Triangles:{} IdxVec:{}\n",
        numFaces,
        numEdges,
        numNodes,
        numTriangles,
        numLines
    );
#endif
}

}  // namespace

bool tessellateShape(TopoDS_Shape shape, ShapeGeometry& geometry,
                     double deviation, double angularDeflection, bool normalsFromUV)
{
    geometry = {};
    try {
        for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next()) {
            if (BRep_Tool::Surface(TopoDS::Face(faces.Current())).IsNull()) {
                throw Standard_Failure("Face has no surface");
            }
        }
        ShapeGeometry result;
        tessellateShapeUnchecked(shape, result, deviation, angularDeflection, normalsFromUV);
        geometry = std::move(result);
        return true;
    }
    catch (const Standard_Failure& error) {
        FC_ERR("Cannot compute Inventor representation: " << error.GetMessageString());
    }
    catch (const std::exception& error) {
        FC_ERR("Cannot compute Inventor representation: " << error.what());
    }
    catch (...) {
        FC_ERR("Cannot compute Inventor representation: unknown C++ exception");
    }
    return false;
}

}  // namespace PartGui
