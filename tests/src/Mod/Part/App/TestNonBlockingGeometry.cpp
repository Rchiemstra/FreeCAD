// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <Mod/Part/App/TopoShapeArchive.h>
#include <Mod/Part/App/BooleanGeometryOperation.h>
#include <Mod/Part/App/FilletGeometryOperation.h>
#include <Mod/Part/App/SweepGeometryOperation.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <gp_Pnt.hxx>
#include <QDir>
#include <QTemporaryDir>

class NonBlockingGeometryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(_tempDir->isValid());
    }

    std::unique_ptr<QTemporaryDir> _tempDir;
};

TEST_F(NonBlockingGeometryTest, TopoShapeArchiveWriteReadRoundTrip)
{
    BRepPrimAPI_MakeBox mkBox(10.0, 20.0, 30.0);
    Part::TopoShape boxShape(mkBox.Shape());
    boxShape.Tag = 12345;

    Part::FrozenTopoShapeBundle inBundle = Part::TopoShapeArchive::createBundle(boxShape);

    std::string archivePath = (_tempDir->path() + "/test_box.fcg").toStdString();

    bool writeSuccess = Part::TopoShapeArchive::writeArchive(inBundle, archivePath);
    EXPECT_TRUE(writeSuccess);

    Part::FrozenTopoShapeBundle outBundle;
    bool readSuccess = Part::TopoShapeArchive::readArchive(archivePath, outBundle);
    EXPECT_TRUE(readSuccess);

    EXPECT_EQ(outBundle.shapeTag, 12345);
    EXPECT_FALSE(outBundle.shape.isNull());
    EXPECT_EQ(outBundle.shape.getShape().ShapeType(), TopAbs_SOLID);
}

TEST_F(NonBlockingGeometryTest, ChecksumMismatchRejection)
{
    BRepPrimAPI_MakeCylinder mkCyl(5.0, 15.0);
    Part::TopoShape cylShape(mkCyl.Shape());

    Part::FrozenTopoShapeBundle inBundle = Part::TopoShapeArchive::createBundle(cylShape);
    std::string archivePath = (_tempDir->path() + "/corrupt.fcg").toStdString();

    ASSERT_TRUE(Part::TopoShapeArchive::writeArchive(inBundle, archivePath));

    // Corrupt one byte in the middle of the archive
    std::fstream fs(archivePath, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(fs.is_open());
    fs.seekp(20);
    char corruptByte = 0xFF;
    fs.write(&corruptByte, 1);
    fs.close();

    Part::FrozenTopoShapeBundle outBundle;
    bool readSuccess = Part::TopoShapeArchive::readArchive(archivePath, outBundle);
    // Checksum verification must reject corrupted archive payload
    EXPECT_FALSE(readSuccess);
}

class TestWorkerContext : public App::GeometryWorkerContext
{
public:
    explicit TestWorkerContext(const std::string& tempDir)
        : _tempDir(tempDir)
    {
    }

    void reportProgress(double fraction, const std::string& phase = "") override
    {
        _lastFraction = fraction;
        _lastPhase = phase;
    }

    bool isCancelled() const override
    {
        return false;
    }

    std::chrono::steady_clock::time_point deadline() const override
    {
        return std::chrono::steady_clock::now() + std::chrono::minutes(1);
    }

    std::string tempDir() const override
    {
        return _tempDir;
    }

    double _lastFraction {0.0};
    std::string _lastPhase;
    std::string _tempDir;
};

TEST_F(NonBlockingGeometryTest, BooleanFuseOperation)
{
    BRepPrimAPI_MakeBox box1(10.0, 10.0, 10.0);
    BRepPrimAPI_MakeBox box2(gp_Pnt(5, 5, 5), 10.0, 10.0, 10.0);

    Part::TopoShape shape1(box1.Shape());
    Part::TopoShape shape2(box2.Shape());

    Part::FrozenTopoShapeBundle b1 = Part::TopoShapeArchive::createBundle(shape1);
    Part::FrozenTopoShapeBundle b2 = Part::TopoShapeArchive::createBundle(shape2);

    Part::BooleanGeometryOperation fuseOp(Part::BooleanType::Fuse, b1, b2);

    TestWorkerContext ctx(_tempDir->path().toStdString());
    App::DetachedGeometryResult result = fuseOp.run(ctx);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.resultArchivePath.empty());

    Part::FrozenTopoShapeBundle outBundle;
    EXPECT_TRUE(Part::TopoShapeArchive::readArchive(result.resultArchivePath, outBundle));
    EXPECT_FALSE(outBundle.shape.isNull());
}

TEST_F(NonBlockingGeometryTest, FilletOperation)
{
    BRepPrimAPI_MakeBox box(10.0, 10.0, 10.0);
    Part::TopoShape shape(box.Shape());
    Part::FrozenTopoShapeBundle b = Part::TopoShapeArchive::createBundle(shape);

    std::vector<Part::FilletEdgeSpec> edges;
    edges.push_back({0, 1.0, 1.0}); // Fillet edge 0 with radius 1.0

    Part::FilletGeometryOperation filletOp(b, edges);

    TestWorkerContext ctx(_tempDir->path().toStdString());
    App::DetachedGeometryResult result = filletOp.run(ctx);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.resultArchivePath.empty());

    Part::FrozenTopoShapeBundle outBundle;
    EXPECT_TRUE(Part::TopoShapeArchive::readArchive(result.resultArchivePath, outBundle));
    EXPECT_FALSE(outBundle.shape.isNull());
}
