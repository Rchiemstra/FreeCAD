// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <limits>
#include <regex>
#include <string>
#include <utility>

#include <Mod/Inspection/Gui/PhotoInspectionRenderer.h>

namespace
{

using Inspection::Photo::ScenePrimitive;
using Inspection::Photo::ScenePrimitiveKind;
using Inspection::Photo::VectorScene;
using InspectionGui::renderPhotoInspectionPdf;
using InspectionGui::writePhotoInspectionFileAtomically;

std::array<double, 4> mediaBox(const QByteArray& pdf)
{
    const std::string bytes(pdf.constData(), static_cast<std::size_t>(pdf.size()));
    const std::regex pattern(
        R"(/MediaBox\s*\[\s*([-+0-9.eE]+)\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)\s*\])"
    );
    std::smatch match;
    if (!std::regex_search(bytes, match, pattern)) {
        return {
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN(),
        };
    }
    return {
        std::stod(match[1].str()),
        std::stod(match[2].str()),
        std::stod(match[3].str()),
        std::stod(match[4].str()),
    };
}

VectorScene scene(const double widthMm, const double heightMm)
{
    VectorScene value;
    value.widthMm = widthMm;
    value.heightMm = heightMm;
    ScenePrimitive ruler;
    ruler.id = "physical-ruler";
    ruler.layer = "verification";
    ruler.kind = ScenePrimitiveKind::Polyline;
    ruler.points = {{10.0, 10.0}, {110.0, 10.0}, {110.0, 210.0}};
    ruler.strokeWidthMm = 0.1;
    value.primitives.push_back(std::move(ruler));
    return value;
}

void expectMediaSize(const VectorScene& input, const double widthMm, const double heightMm)
{
    const auto result = renderPhotoInspectionPdf(input);
    ASSERT_TRUE(result.valid) << result.error.toStdString();
    ASSERT_TRUE(result.bytes.startsWith("%PDF-"));

    const auto box = mediaBox(result.bytes);
    ASSERT_TRUE(std::isfinite(box[2])) << result.bytes.left(1000).toStdString();
    constexpr double pointsToMm = 25.4 / 72.0;
    // QPdfWriter serializes the MediaBox in whole PostScript points. Requiring
    // at most half a point proves the closest representable physical page
    // size while retaining the quantization as an uncertainty contribution.
    constexpr double halfPointMm = pointsToMm / 2.0 + 1.0e-6;
    EXPECT_NEAR((box[2] - box[0]) * pointsToMm, widthMm, halfPointMm);
    EXPECT_NEAR((box[3] - box[1]) * pointsToMm, heightMm, halfPointMm);
}

TEST(PhotoInspectionPdfTest, a4PortraitHasExactPhysicalMediaBox)
{
    expectMediaSize(scene(210.0, 297.0), 210.0, 297.0);
}

TEST(PhotoInspectionPdfTest, a3LandscapeHasExactPhysicalMediaBox)
{
    expectMediaSize(scene(420.0, 297.0), 420.0, 297.0);
}

TEST(PhotoInspectionPdfTest, invalidOrOversizedScenesFailClosed)
{
    EXPECT_FALSE(renderPhotoInspectionPdf(scene(0.0, 297.0)).valid);

    VectorScene oversized = scene(210.0, 297.0);
    oversized.primitives.resize(250001);
    EXPECT_FALSE(renderPhotoInspectionPdf(oversized).valid);
}

TEST(PhotoInspectionPdfTest, atomicWriterProducesCompletePdf)
{
    const auto result = renderPhotoInspectionPdf(scene(210.0, 297.0));
    ASSERT_TRUE(result.valid);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString target = directory.filePath(QStringLiteral("inspection.pdf"));
    QString error;
    ASSERT_TRUE(writePhotoInspectionFileAtomically(target, result.bytes, error))
        << error.toStdString();

    QFile file(target);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), result.bytes);
}

}  // namespace
