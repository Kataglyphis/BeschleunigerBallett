// GoldenMetrics.hpp is the oracle every golden trusts, so it gets its own fast CPU tests.

#include <gtest/gtest.h>

#include "GoldenMetrics.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using Kataglyphis::Test::GoldenMetrics::compare_shadow_masks;
using Kataglyphis::Test::GoldenMetrics::Crop;
using Kataglyphis::Test::GoldenMetrics::GroundPixel;
using Kataglyphis::Test::GoldenMetrics::GroundRect;
using Kataglyphis::Test::GoldenMetrics::ggx_distribution;
using Kataglyphis::Test::GoldenMetrics::GroundShadowPrediction;
using Kataglyphis::Test::GoldenMetrics::kBrdfMinRoughness;
using Kataglyphis::Test::GoldenMetrics::post_display_level;
using Kataglyphis::Test::GoldenMetrics::post_linear_level;
using Kataglyphis::Test::GoldenMetrics::predict_ground_shadow;
using Kataglyphis::Test::GoldenMetrics::ShadowAgreement;
using Kataglyphis::Test::GoldenMetrics::WorldBox;
using Kataglyphis::Test::GoldenMetrics::detail_fraction;
using Kataglyphis::Test::GoldenMetrics::distinct_luminance_buckets;
using Kataglyphis::Test::GoldenMetrics::fraction_above;
using Kataglyphis::Test::GoldenMetrics::frame_structure;
using Kataglyphis::Test::GoldenMetrics::FrameStructure;
using Kataglyphis::Test::GoldenMetrics::kFrameLitLevel;
using Kataglyphis::Test::GoldenMetrics::luminance_bucket;
using Kataglyphis::Test::GoldenMetrics::luminance_of;
using Kataglyphis::Test::GoldenMetrics::luminance_stddev;
using Kataglyphis::Test::GoldenMetrics::mean_luminance;
using Kataglyphis::Test::GoldenMetrics::mean_luminance_in_crop;
using Kataglyphis::Test::GoldenMetrics::renderer_mean_ratio;
using Kataglyphis::Test::GoldenMetrics::renderer_mean_ratio_holds;
using Kataglyphis::Test::GoldenMetrics::sky_silhouette;
using Kataglyphis::Test::GoldenMetrics::SkySilhouette;
using Kataglyphis::Test::GoldenMetrics::swung_fraction;
using Kataglyphis::Test::GoldenMetrics::white_single_scatter_albedo;

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

// One opaque pixel per entry, in a single row.
std::vector<uint8_t> row_frame(const std::vector<std::array<uint8_t, 3>> &colours)
{
    std::vector<uint8_t> rgba;
    rgba.reserve(colours.size() * 4U);
    for (const auto &[r, g, b] : colours) { rgba.insert(rgba.end(), { r, g, b, 255U }); }
    return rgba;
}

// Every grey level once: 256 pixels.
std::vector<uint8_t> grey_ramp_frame()
{
    std::vector<std::array<uint8_t, 3>> colours;
    for (int level = 0; level < 256; ++level) {
        const auto grey = static_cast<uint8_t>(level);
        colours.push_back({ grey, grey, grey });
    }
    return row_frame(colours);
}

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

// A 64x64 top-down orthographic view of a 20x20 ground, +x to the right, one world unit per 3.2 pixels.
constexpr uint32_t TOP_DOWN_SIZE = 64U;
constexpr GroundRect TOP_DOWN_GROUND{ 0.0F, -10.0F, 10.0F, -10.0F, 10.0F };
constexpr WorldBox TOP_DOWN_CASTER{ glm::vec3(-2.0F, 4.0F, -2.0F), glm::vec3(2.0F, 5.0F, 2.0F) };

GroundShadowPrediction top_down_prediction(glm::vec3 light_travel)
{
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0F, 50.0F, 0.0F), glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, -1.0F));
    const glm::mat4 projection = glm::ortho(-10.0F, 10.0F, -10.0F, 10.0F, 1.0F, 100.0F);
    return predict_ground_shadow(
      glm::inverse(projection * view), TOP_DOWN_SIZE, TOP_DOWN_SIZE, TOP_DOWN_GROUND, TOP_DOWN_CASTER, light_travel);
}

// The middle row's pixel over world x.
GroundPixel top_down_pixel_at(const GroundShadowPrediction &prediction, float world_x)
{
    const auto column = static_cast<uint32_t>((world_x + 10.0F) / 20.0F * static_cast<float>(TOP_DOWN_SIZE));
    return prediction.pixels[static_cast<size_t>(TOP_DOWN_SIZE / 2U) * TOP_DOWN_SIZE + column];
}

// Left half lit, right half shadowed, in a frame of WIDTH x HEIGHT.
std::vector<GroundPixel> half_shadowed_prediction()
{
    std::vector<GroundPixel> pixels(static_cast<size_t>(WIDTH) * HEIGHT, GroundPixel::Lit);
    for (uint32_t y = 0; y < HEIGHT; ++y) {
        for (uint32_t x = WIDTH / 2U; x < WIDTH; ++x) {
            pixels[static_cast<size_t>(y) * WIDTH + x] = GroundPixel::Shadowed;
        }
    }
    return pixels;
}

std::vector<bool> shadowed_from_column(uint32_t first_column)
{
    std::vector<bool> mask(static_cast<size_t>(WIDTH) * HEIGHT, false);
    for (uint32_t y = 0; y < HEIGHT; ++y) {
        for (uint32_t x = first_column; x < WIDTH; ++x) { mask[static_cast<size_t>(y) * WIDTH + x] = true; }
    }
    return mask;
}

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

TEST(GoldenMetrics, PredictedShadowFallsWhereTheLightTravels)
{
    const GroundShadowPrediction prediction = top_down_prediction(glm::vec3(1.0F, -1.0F, 0.0F));
    EXPECT_EQ(top_down_pixel_at(prediction, 5.0F), GroundPixel::Shadowed);
    EXPECT_EQ(top_down_pixel_at(prediction, -5.0F), GroundPixel::Lit);
    // The caster itself hides the ground beneath it from a camera above.
    EXPECT_EQ(top_down_pixel_at(prediction, 0.0F), GroundPixel::NotGround);
}

TEST(GoldenMetrics, PredictedShadowFlipsSideWithTheLight)
{
    const GroundShadowPrediction prediction = top_down_prediction(glm::vec3(-1.0F, -1.0F, 0.0F));
    EXPECT_EQ(top_down_pixel_at(prediction, 5.0F), GroundPixel::Lit);
    EXPECT_EQ(top_down_pixel_at(prediction, -5.0F), GroundPixel::Shadowed);
}

TEST(GoldenMetrics, PredictedShadowEndsAtTheCastersShadowEdge)
{
    // Heights 4..5 under a 45 degree light: the shadow spans x from 2 to 7.
    const GroundShadowPrediction prediction = top_down_prediction(glm::vec3(1.0F, -1.0F, 0.0F));
    EXPECT_EQ(top_down_pixel_at(prediction, 6.5F), GroundPixel::Shadowed);
    EXPECT_EQ(top_down_pixel_at(prediction, 7.6F), GroundPixel::Lit);
}

TEST(GoldenMetrics, ShadowMasksAgreeWhenTheMeasurementMatches)
{
    const ShadowAgreement agreement =
      compare_shadow_masks(half_shadowed_prediction(), shadowed_from_column(WIDTH / 2U), WIDTH, HEIGHT, 1U);
    EXPECT_GT(agreement.compared, 0U);
    EXPECT_DOUBLE_EQ(agreement.agreement(), 1.0);
    EXPECT_EQ(agreement.predicted_shadowed, agreement.measured_shadowed);
    EXPECT_DOUBLE_EQ(agreement.predicted_centroid.x, agreement.measured_centroid.x);
}

TEST(GoldenMetrics, ShadowMasksIgnoreAnEdgeOffByLessThanTheBand)
{
    const ShadowAgreement agreement =
      compare_shadow_masks(half_shadowed_prediction(), shadowed_from_column(WIDTH / 2U + 1U), WIDTH, HEIGHT, 1U);
    EXPECT_DOUBLE_EQ(agreement.agreement(), 1.0);
}

TEST(GoldenMetrics, ShadowMasksDisagreeWhenTheShadowIsOnTheOtherSide)
{
    std::vector<bool> mirrored = shadowed_from_column(WIDTH / 2U);
    mirrored.flip();
    const ShadowAgreement agreement = compare_shadow_masks(half_shadowed_prediction(), mirrored, WIDTH, HEIGHT, 1U);
    EXPECT_DOUBLE_EQ(agreement.agreement(), 0.0);
    EXPECT_LT(agreement.measured_centroid.x, agreement.predicted_centroid.x);
}

TEST(GoldenMetrics, PostDisplayLevelMapsMidGreyAndClipsOnlyAtTheTop)
{
    EXPECT_DOUBLE_EQ(post_display_level(0.0), 0.0);
    EXPECT_NEAR(post_display_level(0.18), 141.1, 0.5);
    EXPECT_NEAR(post_display_level(1.0e4), 255.0, 1e-6);
    double previous = -1.0;
    for (int step = 0; step <= 200; ++step) {
        const double level = post_display_level(0.05 * static_cast<double>(step));
        EXPECT_GE(level, previous) << "at linear " << 0.05 * step;
        previous = level;
    }
    EXPECT_LT(post_display_level(4.0), 255.0) << "ACES compresses a highlight before clipping it.";
}

// The exposure goldens read radiance back off 8-bit levels; below the clip the two maps undo each other.
TEST(GoldenMetrics, PostLinearLevelUndoesTheDisplayEncoding)
{
    EXPECT_DOUBLE_EQ(post_linear_level(0.0), 0.0);
    for (const double linear : { 0.001, 0.02, 0.18, 0.5, 1.0, 3.0 }) {
        EXPECT_NEAR(post_linear_level(post_display_level(linear)), linear, 1e-9 + 1e-9 * linear) << "at " << linear;
    }
    EXPECT_GT(post_linear_level(254.0), post_linear_level(200.0));
}

// The furnace goldens' oracle once guarded PI d^2 at 1e-6, which clipped every GGX peak below a roughness of 0.154.
TEST(GoldenMetrics, FurnaceOracleKeepsTheGgxPeakBelowTheOldGuardsReach)
{
    // A white metal at normal incidence returns nearly all its light; the clipped peak gave 0.60 at roughness 0.12.
    EXPECT_NEAR(white_single_scatter_albedo(1.0, 0.12, 1.0), 1.0, 0.02);
    EXPECT_NEAR(white_single_scatter_albedo(0.7, 0.12, 1.0), 1.0, 0.02);
}

TEST(GoldenMetrics, FurnaceOracleFloorsTheNdfRoughnessAsBrdfSlangDoes)
{
    // The peak is the floor's 1 / (PI r^4) at and below MIN_ROUGHNESS; a narrower lobe barely moves the albedo.
    constexpr double PI = 3.14159265358979323846;
    EXPECT_NEAR(ggx_distribution(1.0, kBrdfMinRoughness) * PI * std::pow(kBrdfMinRoughness, 4.0), 1.0, 1e-9);
    EXPECT_DOUBLE_EQ(ggx_distribution(1.0, 0.01), ggx_distribution(1.0, kBrdfMinRoughness));
    const double floor = white_single_scatter_albedo(0.7, kBrdfMinRoughness, 1.0);
    ASSERT_GT(floor, 0.0);
    EXPECT_NEAR(white_single_scatter_albedo(0.7, 0.01, 1.0) / floor, 1.0, 0.01);
    // The Smith remap floors too: its raw roughness 0 masked a grazing lobe less, 1.0005 against the floor's 0.824.
    EXPECT_DOUBLE_EQ(
      white_single_scatter_albedo(0.01, 0.0, 1.0), white_single_scatter_albedo(0.01, kBrdfMinRoughness, 1.0));
}

// Its grid uniform in angle could not resolve a lobe this narrow: a white metal at the floor gave 0.03 to 1.16.
TEST(GoldenMetrics, FurnaceOracleResolvesTheNarrowestLobeTheFloorAllows)
{
    for (const double mu : { 0.3, 0.7, 1.0 }) {
        EXPECT_NEAR(white_single_scatter_albedo(mu, kBrdfMinRoughness, 1.0), 1.0, 0.03) << "mu " << mu;
    }
    // A 4000-step angle grid and a fine half-vector grid agree on 0.9667 here; the 320-step grid gave 0.730.
    EXPECT_NEAR(white_single_scatter_albedo(0.3, 0.12, 1.0), 0.9667, 0.002);
}

// The PowerShell copy used Rec. 601, which reads pure green 149.685; both now weigh the stored bytes by Rec. 709.
TEST(GoldenMetrics, LuminanceWeighsTheChannelsByRec709)
{
    const std::vector<uint8_t> primaries = row_frame({ { 255U, 0U, 0U }, { 0U, 255U, 0U }, { 0U, 0U, 255U } });
    EXPECT_NEAR(luminance_of(primaries, 0U), 54.213, 1e-9);
    EXPECT_NEAR(luminance_of(primaries, 1U), 182.376, 1e-9);
    EXPECT_NEAR(luminance_of(primaries, 2U), 18.411, 1e-9);
}

TEST(GoldenMetrics, LuminanceBucketIsTheNearestLevelHalvesUp)
{
    EXPECT_EQ(luminance_bucket(8.49), 8U);
    EXPECT_EQ(luminance_bucket(8.5), 9U);
    EXPECT_EQ(luminance_bucket(-3.0), 0U);
    EXPECT_EQ(luminance_bucket(300.0), 255U);
    // White's luma sums to 254.99999999999997, which a floor put a bucket low.
    EXPECT_EQ(luminance_bucket(luminance_of(row_frame({ { 255U, 255U, 255U } }), 0U)), 255U);
}

// A floor of the luma counted 203: 62 greys sum to just below their level.
TEST(GoldenMetrics, EveryGreyLevelIsItsOwnBucket) { EXPECT_EQ(distinct_luminance_buckets(grey_ramp_frame()), 256U); }

TEST(GoldenMetrics, LuminanceStddevDividesByThePixelCount)
{
    const std::vector<uint8_t> frame = row_frame({ { 0U, 0U, 0U }, { 255U, 255U, 255U } });
    EXPECT_NEAR(mean_luminance(frame), 127.5, 1e-9);
    EXPECT_NEAR(luminance_stddev(frame), 127.5, 1e-9);
}

TEST(GoldenMetrics, FractionAboveCountsOnlyBrighterPixels)
{
    const std::vector<uint8_t> frame = row_frame({ { 7U, 7U, 7U }, { 9U, 9U, 9U }, { 9U, 9U, 9U }, { 0U, 0U, 0U } });
    EXPECT_DOUBLE_EQ(fraction_above(frame, 8.0), 0.5);
}

// The lit level is strict; a pixel's own luma as the threshold holds whatever a fused multiply-add rounds.
TEST(GoldenMetrics, FractionAboveLeavesAPixelAtTheThresholdOut)
{
    EXPECT_DOUBLE_EQ(kFrameLitLevel, 8.0);
    const std::vector<uint8_t> frame = row_frame({ { 8U, 8U, 8U }, { 9U, 9U, 9U } });
    EXPECT_DOUBLE_EQ(fraction_above(frame, luminance_of(frame, 0U)), 0.5);
}

TEST(GoldenMetrics, FrameStructureOfAFlatGreyFrameIsLitButUniform)
{
    const FrameStructure structure = frame_structure(flat_frame(128U));
    EXPECT_DOUBLE_EQ(structure.stddev, 0.0);
    EXPECT_EQ(structure.buckets, 1U);
    EXPECT_TRUE(structure.is_lit());
    EXPECT_FALSE(structure.has_structure());
    EXPECT_FALSE(structure.has_variety());
    EXPECT_FALSE(structure.passes());
}

TEST(GoldenMetrics, FrameStructureOfABlackFrameIsDark)
{
    const FrameStructure structure = frame_structure(flat_frame(0U));
    EXPECT_DOUBLE_EQ(structure.lit_fraction, 0.0);
    EXPECT_FALSE(structure.is_lit());
}

TEST(GoldenMetrics, FrameStructureOfAGreyRampPassesEveryCheck)
{
    const FrameStructure structure = frame_structure(grey_ramp_frame());
    EXPECT_NEAR(structure.mean, 127.5, 1e-9);
    EXPECT_NEAR(structure.stddev, 73.90027, 1e-4);
    // Levels 9 to 255, and 8 when a fused multiply-add rounds its luma a hair above 8.
    EXPECT_GE(structure.lit_fraction, 247.0 / 256.0);
    EXPECT_LE(structure.lit_fraction, 248.0 / 256.0);
    EXPECT_TRUE(structure.passes());
}

// The thresholds are strict, as RendersNonBlankFrame's EXPECT_GT and the PowerShell -le have them.
TEST(GoldenMetrics, FrameStructureFailsAtEachThresholdItself)
{
    FrameStructure at_threshold{ 100.0, 2.0, 0.05, 8U };
    EXPECT_FALSE(at_threshold.has_structure());
    EXPECT_FALSE(at_threshold.is_lit());
    EXPECT_FALSE(at_threshold.has_variety());
    at_threshold = FrameStructure{ 100.0, 2.001, 0.0501, 9U };
    EXPECT_TRUE(at_threshold.passes());
}

TEST(GoldenMetrics, RendererMeanRatioHoldsInsideItsClosedRange)
{
    EXPECT_TRUE(renderer_mean_ratio_holds(0.5));
    EXPECT_TRUE(renderer_mean_ratio_holds(4.0));
    EXPECT_FALSE(renderer_mean_ratio_holds(0.499));
    EXPECT_FALSE(renderer_mean_ratio_holds(4.001));
    // A black C++ frame cannot hide a lit Rust one behind a division by zero.
    const FrameStructure black{};
    const FrameStructure lit{ 100.0, 40.0, 0.9, 200U };
    EXPECT_DOUBLE_EQ(renderer_mean_ratio(black, lit), 10.0);
    EXPECT_FALSE(renderer_mean_ratio_holds(renderer_mean_ratio(black, lit)));
    EXPECT_DOUBLE_EQ(renderer_mean_ratio(lit, black), 0.0);
}
