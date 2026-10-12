// GoldenMetrics.hpp is the oracle every golden trusts, so it gets its own fast CPU tests.

#include <gtest/gtest.h>

#include "GoldenMetrics.hpp"

#include <cstdint>
#include <vector>

namespace {

using Kataglyphis::Test::GoldenMetrics::Crop;
using Kataglyphis::Test::GoldenMetrics::detail_fraction;
using Kataglyphis::Test::GoldenMetrics::mean_luminance_in_crop;
using Kataglyphis::Test::GoldenMetrics::sky_silhouette;
using Kataglyphis::Test::GoldenMetrics::SkySilhouette;
using Kataglyphis::Test::GoldenMetrics::swung_fraction;

constexpr uint32_t WIDTH = 8;
constexpr uint32_t HEIGHT = 8;

std::vector<uint8_t> flat_frame(uint8_t level)
{ return std::vector<uint8_t>(static_cast<size_t>(WIDTH) * HEIGHT * 4U, level); }

// Alternating columns of black/white, opaque alpha.
std::vector<uint8_t> checkerboard_frame()
{
    std::vector<uint8_t> rgba(static_cast<size_t>(WIDTH) * HEIGHT * 4U, 0U);
    for (uint32_t y = 0; y < HEIGHT; ++y) {
        for (uint32_t x = 0; x < WIDTH; ++x) {
            const uint8_t level = (x % 2U == 0U) ? 0U : 255U;
            const size_t base = (static_cast<size_t>(y) * WIDTH + x) * 4U;
            rgba[base] = level;
            rgba[base + 1U] = level;
            rgba[base + 2U] = level;
            rgba[base + 3U] = 255U;
        }
    }
    return rgba;
}

Crop full_frame_crop() { return Crop{ 0U, WIDTH, 0U, HEIGHT }; }

// Grey levels from PathTracingAntiAliasesGeometricEdges on lavapipe: sky, slab, the edge row with and without jitter.
constexpr uint8_t SKY_LEVEL = 195U;
constexpr uint8_t SLAB_LEVEL = 238U;
constexpr uint8_t JITTERED_EDGE_LEVEL = 229U;
constexpr uint8_t CENTRED_EDGE_LEVEL = 237U;
constexpr uint32_t EDGE_ROW = 3U;

// Sky above EDGE_ROW, the slab below it; grey, so the luminance is the level itself.
std::vector<uint8_t> silhouette_frame(uint8_t sky, uint8_t edge, uint8_t object)
{
    std::vector<uint8_t> rgba(static_cast<size_t>(WIDTH) * HEIGHT * 4U, 255U);
    for (uint32_t y = 0; y < HEIGHT; ++y) {
        const uint8_t level = y < EDGE_ROW ? sky : (y == EDGE_ROW ? edge : object);
        for (uint32_t x = 0; x < WIDTH; ++x) {
            const size_t base = (static_cast<size_t>(y) * WIDTH + x) * 4U;
            rgba[base] = level;
            rgba[base + 1U] = level;
            rgba[base + 2U] = level;
        }
    }
    return rgba;
}

SkySilhouette golden_sky_silhouette(const std::vector<uint8_t> &frame)
{ return sky_silhouette(frame, WIDTH, full_frame_crop()); }

}// namespace

TEST(GoldenMetrics, SwungFractionIsZeroForIdenticalFrames)
{
    const std::vector<uint8_t> frame = checkerboard_frame();
    EXPECT_DOUBLE_EQ(swung_fraction(frame, frame, WIDTH, HEIGHT, full_frame_crop()), 0.0);
}

TEST(GoldenMetrics, SwungFractionIsOneForFullyDifferentFrames)
{
    const std::vector<uint8_t> black = flat_frame(0U);
    const std::vector<uint8_t> white = flat_frame(255U);
    EXPECT_DOUBLE_EQ(swung_fraction(black, white, WIDTH, HEIGHT, full_frame_crop()), 1.0);
}

TEST(GoldenMetrics, DetailFractionIsZeroForFlatImage)
{
    const std::vector<uint8_t> frame = flat_frame(128U);
    EXPECT_DOUBLE_EQ(detail_fraction(frame, WIDTH, HEIGHT, full_frame_crop()), 0.0);
}

TEST(GoldenMetrics, DetailFractionIsHighForCheckerboard)
{
    const std::vector<uint8_t> frame = checkerboard_frame();
    // Every column boundary in the crop is an edge, so the fraction is near but not exactly 1.0.
    EXPECT_GT(detail_fraction(frame, WIDTH, HEIGHT, full_frame_crop()), 0.4);
}

TEST(GoldenMetrics, DetailFractionOfAFullWidthCropStaysInBounds)
{
    // x1 at the frame width is where detail_fraction can read one pixel past the end; ASan catches it.
    const std::vector<uint8_t> frame = checkerboard_frame();
    EXPECT_GT(detail_fraction(frame, WIDTH, HEIGHT, full_frame_crop()), 0.4);
}

TEST(GoldenMetrics, DetailFractionOfASingleColumnCropIsZero)
{
    // A single-column crop has no right-hand-neighbour pair to compare.
    const std::vector<uint8_t> frame = checkerboard_frame();
    const Crop single_column{ 0U, 1U, 0U, HEIGHT };
    EXPECT_DOUBLE_EQ(detail_fraction(frame, WIDTH, HEIGHT, single_column), 0.0);
}

TEST(GoldenMetrics, MeanLuminanceInCropMatchesAFlatImage)
{
    const std::vector<uint8_t> frame = flat_frame(128U);
    EXPECT_NEAR(mean_luminance_in_crop(frame, WIDTH, HEIGHT, full_frame_crop()), 128.0, 1e-9);
}

TEST(GoldenMetrics, MeanLuminanceInCropIgnoresPixelsOutsideTheCrop)
{
    // The one bright pixel sits outside the crop, which must read 0.
    std::vector<uint8_t> frame = flat_frame(0U);
    const size_t outside_pixel_base = 0U;// column 0, row 0
    frame[outside_pixel_base] = 255U;
    frame[outside_pixel_base + 1U] = 255U;
    frame[outside_pixel_base + 2U] = 255U;
    frame[outside_pixel_base + 3U] = 255U;

    const Crop crop_excluding_column_zero{ 1U, WIDTH, 0U, HEIGHT };
    EXPECT_DOUBLE_EQ(mean_luminance_in_crop(frame, WIDTH, HEIGHT, crop_excluding_column_zero), 0.0);
}

TEST(GoldenMetrics, SkySilhouetteCountsAJitteredEdgeAsBlended)
{
    const SkySilhouette silhouette =
      golden_sky_silhouette(silhouette_frame(SKY_LEVEL, JITTERED_EDGE_LEVEL, SLAB_LEVEL));
    EXPECT_EQ(silhouette.columns, WIDTH);
    EXPECT_EQ(silhouette.blended, WIDTH);
    EXPECT_EQ(silhouette.first_row, EDGE_ROW);
    EXPECT_EQ(silhouette.last_row, EDGE_ROW);
}

// The no-jitter control, kept: a ray through the pixel centre leaves the edge pixel a few levels off the slab.
TEST(GoldenMetrics, SkySilhouetteRejectsAPixelCentredEdge)
{
    const SkySilhouette silhouette = golden_sky_silhouette(silhouette_frame(SKY_LEVEL, CENTRED_EDGE_LEVEL, SLAB_LEVEL));
    EXPECT_EQ(silhouette.columns, WIDTH);
    EXPECT_EQ(silhouette.blended, 0U);
}

TEST(GoldenMetrics, SkySilhouetteIgnoresAStepSmallerThanTheThreshold)
{
    const SkySilhouette silhouette = golden_sky_silhouette(silhouette_frame(SKY_LEVEL, 205U, 205U));
    EXPECT_EQ(silhouette.columns, 0U);
    EXPECT_EQ(silhouette.blended, 0U);
}

// The walk down must stop at the crop's bottom; ASan catches a read past it.
TEST(GoldenMetrics, SkySilhouetteOfAnAllSkyFrameIsEmpty)
{
    const SkySilhouette silhouette = golden_sky_silhouette(flat_frame(SKY_LEVEL));
    EXPECT_EQ(silhouette.columns, 0U);
    EXPECT_EQ(silhouette.blended, 0U);
}
