// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <Mod/Part/PartGlobal.h>

#include <array>
#include <cstdint>
#include <vector>

class TopoDS_Shape;

namespace PartGui
{

struct ShapeGeometry
{
    using Vector = std::array<float, 3>;
    std::vector<Vector> vertices;
    std::vector<Vector> normals;
    std::vector<std::int32_t> faceIndices;
    std::vector<std::int32_t> faceParts;
    std::vector<std::int32_t> edgeIndices;
    std::vector<int> lineToEdge;
    int pointStartIndex {0};
};

// Mutates OCC triangulations; callers sharing TShapes must supply a private copy.
// Failure clears the output and never publishes partially tessellated geometry.
[[nodiscard]] PartGuiExport bool tessellateShape(
    TopoDS_Shape shape, ShapeGeometry& geometry,
    double deviation, double angularDeflection, bool normalsFromUV = false);

}  // namespace PartGui
