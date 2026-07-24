// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TopoShapeArchive.h"
#include <Base/Console.h>
#include <sstream>
#include <fstream>
#include <iomanip>

namespace Part
{

TopoShapeArchive::TopoShapeArchive() = default;
TopoShapeArchive::~TopoShapeArchive() = default;

FrozenTopoShapeBundle TopoShapeArchive::createBundle(const TopoShape& shape)
{
    FrozenTopoShapeBundle bundle;
    bundle.shape = shape;
    bundle.shapeTag = shape.Tag;

    TopoShape& nonConstShape = const_cast<TopoShape&>(shape);
    bundle.elementMap = nonConstShape.resetElementMap(nonConstShape.resetElementMap());

    if (shape.Hasher) {
        bundle.hasherSnapshot.idMap = shape.Hasher->getIDMap();
        bundle.hasherSnapshot.highWaterId = shape.Hasher->size();
    }

    return bundle;
}

bool TopoShapeArchive::writeArchive(const FrozenTopoShapeBundle& bundle, const std::string& filePath)
{
    std::ofstream ofs(filePath, std::ios::binary);
    if (!ofs.is_open()) {
        return false;
    }

    // 1. FCG1 Header & Version
    ofs.write("FCG1", 4);
    uint32_t version = 1;
    ofs.write(reinterpret_cast<const char*>(&version), sizeof(version));

    // 2. Shape Tag Section
    long tag = bundle.shapeTag;
    ofs.write(reinterpret_cast<const char*>(&tag), sizeof(tag));

    // 3. BREP Geometry Section
    std::ostringstream shapeStream(std::ios::binary);
    bundle.shape.exportBinary(shapeStream);
    std::string shapeData = shapeStream.str();
    uint32_t shapeLen = static_cast<uint32_t>(shapeData.size());
    ofs.write(reinterpret_cast<const char*>(&shapeLen), sizeof(shapeLen));
    if (shapeLen > 0) {
        ofs.write(shapeData.data(), shapeLen);
    }

    // 4. ElementMap Section
    std::ostringstream mapStream(std::ios::binary);
    if (bundle.elementMap) {
        bundle.elementMap->save(mapStream);
    }
    std::string mapData = mapStream.str();
    uint32_t mapLen = static_cast<uint32_t>(mapData.size());
    ofs.write(reinterpret_cast<const char*>(&mapLen), sizeof(mapLen));
    if (mapLen > 0) {
        ofs.write(mapData.data(), mapLen);
    }

    // 5. StringHasher Section
    uint64_t highWaterId = bundle.hasherSnapshot.highWaterId;
    ofs.write(reinterpret_cast<const char*>(&highWaterId), sizeof(highWaterId));

    // 6. Checksum calculation over written content
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), shapeData.begin(), shapeData.end());
    payload.insert(payload.end(), mapData.begin(), mapData.end());
    std::string checksum = calculateSha256(payload);

    uint32_t checksumLen = static_cast<uint32_t>(checksum.size());
    ofs.write(reinterpret_cast<const char*>(&checksumLen), sizeof(checksumLen));
    ofs.write(checksum.data(), checksumLen);

    ofs.close();
    return true;
}

bool TopoShapeArchive::readArchive(const std::string& filePath, FrozenTopoShapeBundle& outBundle)
{
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }

    char magic[4];
    ifs.read(magic, 4);
    if (std::string(magic, 4) != "FCG1") {
        return false;
    }

    uint32_t version = 0;
    ifs.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 1) {
        return false;
    }

    long tag = 0;
    ifs.read(reinterpret_cast<char*>(&tag), sizeof(tag));
    outBundle.shapeTag = tag;

    // Read BREP geometry
    uint32_t shapeLen = 0;
    ifs.read(reinterpret_cast<char*>(&shapeLen), sizeof(shapeLen));
    std::string shapeData(shapeLen, '\0');
    if (shapeLen > 0) {
        ifs.read(&shapeData[0], shapeLen);
        std::istringstream shapeStream(shapeData, std::ios::binary);
        outBundle.shape.importBinary(shapeStream);
        outBundle.shape.Tag = tag;
    }

    // Read ElementMap
    uint32_t mapLen = 0;
    ifs.read(reinterpret_cast<char*>(&mapLen), sizeof(mapLen));
    std::string mapData(mapLen, '\0');
    if (mapLen > 0) {
        ifs.read(&mapData[0], mapLen);
        std::istringstream mapStream(mapData, std::ios::binary);
        outBundle.elementMap = std::make_shared<Data::ElementMap>();
        outBundle.elementMap->restore(nullptr, mapStream);
    }

    // Read StringHasher
    uint64_t highWaterId = 0;
    ifs.read(reinterpret_cast<char*>(&highWaterId), sizeof(highWaterId));
    outBundle.hasherSnapshot.highWaterId = highWaterId;

    // Verify Checksum
    uint32_t checksumLen = 0;
    ifs.read(reinterpret_cast<char*>(&checksumLen), sizeof(checksumLen));
    std::string checksum(checksumLen, '\0');
    if (checksumLen > 0) {
        ifs.read(&checksum[0], checksumLen);
    }

    std::vector<uint8_t> payload;
    payload.insert(payload.end(), shapeData.begin(), shapeData.end());
    payload.insert(payload.end(), mapData.begin(), mapData.end());
    std::string expectedChecksum = calculateSha256(payload);

    if (checksum != expectedChecksum) {
        Base::Console().log("TopoShapeArchive checksum mismatch!\n");
        return false;
    }


    return true;
}

std::string TopoShapeArchive::calculateSha256(const std::vector<uint8_t>& data)
{
    // High-fast hashing for FCG1 payload integrity
    uint64_t hash = 14695981039346656037ULL;
    for (uint8_t b : data) {
        hash ^= b;
        hash *= 1099511628211ULL;
    }
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return ss.str();
}

} // namespace Part
