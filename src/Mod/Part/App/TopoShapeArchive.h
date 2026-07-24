// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Part/PartGlobal.h>
#include <App/GeometryJob.h>
#include <App/ElementMap.h>
#include <App/StringHasher.h>
#include <Mod/Part/App/TopoShape.h>

#include <memory>
#include <string>
#include <vector>
#include <map>

namespace Part
{

struct PartExport StringHasherSnapshot
{
    uint64_t highWaterId {0};
    uint64_t revision {0};
    std::map<long, App::StringIDRef> idMap;
};

struct PartExport FrozenTopoShapeBundle
{
    TopoShape shape;
    Data::ElementMapPtr elementMap;
    StringHasherSnapshot hasherSnapshot;
    long shapeTag {0};
    std::vector<std::string> mappedElements;
};

class PartExport TopoShapeArchive
{
public:
    TopoShapeArchive();
    ~TopoShapeArchive();

    static FrozenTopoShapeBundle createBundle(const TopoShape& shape);

    static bool writeArchive(const FrozenTopoShapeBundle& bundle, const std::string& filePath);
    static bool readArchive(const std::string& filePath, FrozenTopoShapeBundle& outBundle);

    static std::string calculateSha256(const std::vector<uint8_t>& data);
};

} // namespace Part
