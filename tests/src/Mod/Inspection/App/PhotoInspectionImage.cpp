// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <Mod/Inspection/App/OpenCVPhotoInspectionCompat.h>
#include <Mod/Inspection/App/PhotoInspectionImage.h>

namespace
{

using namespace Inspection::Photo;

std::vector<std::uint8_t> onePixelPng()
{
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x04, 0x00, 0x00,
        0x00, 0xb5, 0x1c, 0x0c, 0x02, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54, 0x78,
        0xda, 0x63, 0x64, 0xf8, 0x0f, 0x00, 0x01, 0x05, 0x01, 0x01, 0x27, 0x18, 0xe3, 0x66,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

GrayRaster whiteRaster(const int width, const int height)
{
    GrayRaster result;
    result.width = width;
    result.height = height;
    result.pixels.assign(static_cast<std::size_t>(width * height), 255);
    return result;
}

GrayRaster renderedMarker(const int markerId)
{
    BinaryGrid grid;
    std::string reason;
    if (!OpenCVPhotoInspectionCompat::markerGrid(7, markerId, grid, reason)) {
        return {};
    }
    constexpr int cellPixels = 12;
    constexpr int quietCells = 2;
    const int size = (grid.rows + quietCells * 2) * cellPixels;
    GrayRaster raster = whiteRaster(size, size);
    for (int row = 0; row < grid.rows; ++row) {
        for (int column = 0; column < grid.columns; ++column) {
            if (!grid.black(row, column)) {
                continue;
            }
            for (int y = 0; y < cellPixels; ++y) {
                for (int x = 0; x < cellPixels; ++x) {
                    const int pixelX = (column + quietCells) * cellPixels + x;
                    const int pixelY = (row + quietCells) * cellPixels + y;
                    raster.pixels[static_cast<std::size_t>(pixelY * size + pixelX)] = 0;
                }
            }
        }
    }
    return raster;
}

GrayRaster renderedQr(const std::string& payload)
{
    BinaryGrid grid;
    std::string reason;
    if (!OpenCVPhotoInspectionCompat::qrGrid(payload, grid, reason)) {
        return {};
    }
    constexpr int cellPixels = 8;
    constexpr int quietCells = 4;
    const int size = (grid.rows + quietCells * 2) * cellPixels;
    GrayRaster raster = whiteRaster(size, size);
    for (int row = 0; row < grid.rows; ++row) {
        for (int column = 0; column < grid.columns; ++column) {
            if (!grid.black(row, column)) {
                continue;
            }
            for (int y = 0; y < cellPixels; ++y) {
                for (int x = 0; x < cellPixels; ++x) {
                    const int pixelX = (column + quietCells) * cellPixels + x;
                    const int pixelY = (row + quietCells) * cellPixels + y;
                    raster.pixels[static_cast<std::size_t>(pixelY * size + pixelX)] = 0;
                }
            }
        }
    }
    return raster;
}

TEST(PhotoInspectionImageTest, pngHeaderPreflightIsAllocationBounded)
{
    EncodedImageInfo info;
    const auto valid = preflightEncodedImage(onePixelPng(), {}, info);
    ASSERT_TRUE(valid.valid) << valid.diagnostic.message;
    EXPECT_EQ(info.format, EncodedImageFormat::Png);
    EXPECT_EQ(info.width, 1);
    EXPECT_EQ(info.height, 1);

    auto bomb = onePixelPng();
    std::fill(bomb.begin() + 16, bomb.begin() + 24, 0xff);
    const auto rejected = preflightEncodedImage(bomb, {}, info);
    EXPECT_FALSE(rejected.valid);
    EXPECT_EQ(rejected.diagnostic.code, DiagnosticCode::ResourceLimit);
}

TEST(PhotoInspectionImageTest, unsupportedBytesRejectBeforeDecoder)
{
    EncodedImageInfo info;
    const auto result = preflightEncodedImage({1, 2, 3, 4}, {}, info);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::InvalidSchema);
}

TEST(PhotoInspectionImageTest, decodeIsTruthfulInOpenCvOnAndOffBuilds)
{
    const auto result = decodePhotoImage(onePixelPng());
    if (OpenCVPhotoInspectionCompat::capability().available) {
        ASSERT_EQ(result.status, OperationStatus::Complete) << result.diagnostic.message;
        EXPECT_TRUE(result.raster.valid());
        EXPECT_EQ(result.raster.sourceSha256.size(), 64);
    }
    else {
        EXPECT_EQ(result.status, OperationStatus::Unavailable);
        EXPECT_EQ(result.diagnostic.code, DiagnosticCode::OpenCVUnavailable);
    }
}

TEST(PhotoInspectionImageTest, exactCornerCorrespondencesRectifyWithoutScaleFit)
{
    const GrayRaster source = whiteRaster(101, 101);
    const std::vector<PointCorrespondence> points {
        {{0.0, 0.0}, {0.0, 0.0}},
        {{100.0, 0.0}, {10.0, 0.0}},
        {{100.0, 100.0}, {10.0, 10.0}},
        {{0.0, 100.0}, {0.0, 10.0}},
    };
    RectificationOptions options;
    options.pixelsPerMm = 10.0;
    options.sheetWidthMm = 10.0;
    options.sheetHeightMm = 10.0;

    const auto result = rectifyPhotoImage(source, points, options);
    if (OpenCVPhotoInspectionCompat::capability().available) {
        ASSERT_EQ(result.status, OperationStatus::Complete) << result.diagnostic.message;
        EXPECT_EQ(result.raster.width, 100);
        EXPECT_EQ(result.raster.height, 100);
        EXPECT_NEAR(result.imageToSheet[0], 0.1, 1.0e-9);
        EXPECT_NEAR(result.imageToSheet[4], 0.1, 1.0e-9);
        EXPECT_NEAR(result.rmsResidualMm, 0.0, 1.0e-9);
    }
    else {
        EXPECT_EQ(result.status, OperationStatus::Unavailable);
    }
}

TEST(PhotoInspectionImageTest, generatedMarkerIsDetectedWithPhysicalSideEvidence)
{
    const GrayRaster raster = renderedMarker(920);
    if (!OpenCVPhotoInspectionCompat::capability().available) {
        EXPECT_FALSE(raster.valid());
        const auto result = detectPhotoMarkers(whiteRaster(20, 20), 7, {920});
        EXPECT_EQ(result.status, OperationStatus::Unavailable);
        return;
    }
    ASSERT_TRUE(raster.valid());
    const auto result = detectPhotoMarkers(raster, 7, {920});
    ASSERT_EQ(result.status, OperationStatus::Complete) << result.diagnostic.message;
    ASSERT_EQ(result.accepted.size(), 1);
    EXPECT_EQ(result.accepted.front().id, 920);
    EXPECT_GE(result.accepted.front().minimumSidePixels, 80.0);
}

TEST(PhotoInspectionImageTest, markerAllowListIsStrict)
{
    if (!OpenCVPhotoInspectionCompat::capability().available) {
        const auto result = detectPhotoMarkers(whiteRaster(20, 20), 7, {921});
        EXPECT_EQ(result.status, OperationStatus::Unavailable);
        return;
    }
    const auto result = detectPhotoMarkers(renderedMarker(920), 7, {921});
    EXPECT_EQ(result.status, OperationStatus::Inconclusive);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::LowImageQuality);
    EXPECT_TRUE(result.accepted.empty());
}

TEST(PhotoInspectionImageTest, duplicatedAllowListRejectsBeforeDetection)
{
    const auto result = detectPhotoMarkers(whiteRaster(20, 20), 7, {920, 920});
    EXPECT_EQ(result.status, OperationStatus::InvalidInput);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::InvalidSchema);
}

TEST(PhotoInspectionImageTest, generatedQrRoundTripsLocally)
{
    const std::string payload = "{\"v\":[1,0],\"sid\":\"fixture\"}";
    const GrayRaster raster = renderedQr(payload);
    if (!OpenCVPhotoInspectionCompat::capability().available) {
        EXPECT_FALSE(raster.valid());
        const auto result = detectPhotoQr(whiteRaster(20, 20));
        EXPECT_EQ(result.status, OperationStatus::Unavailable);
        return;
    }
    ASSERT_TRUE(raster.valid());
    const auto result = detectPhotoQr(raster);
    ASSERT_EQ(result.status, OperationStatus::Complete) << result.diagnostic.message;
    EXPECT_EQ(result.payload, payload);
}

TEST(PhotoInspectionImageTest, blankImageCannotInventQrIdentity)
{
    const auto result = detectPhotoQr(whiteRaster(100, 100));
    if (OpenCVPhotoInspectionCompat::capability().available) {
        EXPECT_EQ(result.status, OperationStatus::Inconclusive);
        EXPECT_TRUE(result.payload.empty());
    }
    else {
        EXPECT_EQ(result.status, OperationStatus::Unavailable);
    }
}

TEST(PhotoInspectionImageTest, rectificationCapRejectsBeforeAllocation)
{
    const GrayRaster source = whiteRaster(2, 2);
    const std::vector<PointCorrespondence> points {
        {{0.0, 0.0}, {0.0, 0.0}},
        {{1.0, 0.0}, {1.0, 0.0}},
        {{1.0, 1.0}, {1.0, 1.0}},
        {{0.0, 1.0}, {0.0, 1.0}},
    };
    RectificationOptions options;
    options.pixelsPerMm = 25.0;
    options.sheetWidthMm = 297.0;
    options.sheetHeightMm = 420.0;
    const auto result = rectifyPhotoImage(source, points, options);
    EXPECT_EQ(result.status, OperationStatus::ResourceLimit);
}

TEST(PhotoInspectionImageTest, emptyReferenceSegmentationExtractsPhysicalContour)
{
    GrayRaster empty = whiteRaster(200, 200);
    GrayRaster part = empty;
    for (int y = 50; y < 150; ++y) {
        for (int x = 40; x < 160; ++x) {
            part.pixels[static_cast<std::size_t>(y * part.width + x)] = 20;
        }
    }
    std::vector<std::uint8_t> ink(part.pixels.size(), 0);
    SegmentationOptions options;
    options.pixelsPerMm = 10.0;
    options.morphologyRadiusPixels = 0;
    const auto result = segmentPhotoContour(part, ink, &empty, options);
    if (OpenCVPhotoInspectionCompat::capability().available) {
        ASSERT_EQ(result.status, OperationStatus::Complete) << result.diagnostic.message;
        ASSERT_FALSE(result.cycles.empty());
        EXPECT_FALSE(result.cycles.front().hole);
        EXPECT_GT(result.cycles.front().points.size(), 100);
    }
    else {
        EXPECT_EQ(result.status, OperationStatus::Unavailable);
    }
}

}  // namespace
