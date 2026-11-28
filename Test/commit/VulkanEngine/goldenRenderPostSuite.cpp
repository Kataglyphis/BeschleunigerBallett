// Goldens: the post chain's exposure, auto exposure, bloom and SSAO, each graded against the frame's own radiance.

#include <map>

#include "GoldenRenderHarness.hpp"

import kataglyphis.vulkan.renderer;

using namespace Kataglyphis::TestSupport::GoldenRender;
using Kataglyphis::Test::GoldenMetrics::post_display_level;
using Kataglyphis::Test::GoldenMetrics::post_linear_level;

namespace {

// The app's clock advances a tenth of a second per frame, so the adaptation moves as it would at 10 Hz.
constexpr double kFrameSeconds = 0.1;
// histogram.slang's luminance range and key (OxidANT's exposure.slang).
constexpr double kMinLogLuminance = -10.0;
constexpr double kMaxLogLuminance = 4.0;
constexpr double kExposureKey = 0.18;

// One frame with the exposure state copied out: [adapted EV, target EV].
std::array<float, 2> render_frame_reading_exposure(EngineHarness &harness, double advanceSeconds)
{
    harness.renderer->requestExposureCapture();
    harness.settings.time_seconds += advanceSeconds;
    harness.render_frame();
    const std::optional<std::array<float, 2>> state = harness.renderer->takeCapturedExposure();
    EXPECT_TRUE(state.has_value()) << "the frame copied no exposure state out";
    return state.value_or(std::array{ std::nanf(""), std::nanf("") });
}

// The reduce's target without its bins: log2(key) less the non-black pixels' mean log2 luminance, clamped to its range.
double log_average_target_ev(const std::vector<float> &hdr)
{
    double log_sum = 0.0;
    std::size_t counted = 0;
    for (std::size_t i = 0; i + 3U < hdr.size(); i += 4U) {
        const double luminance = 0.2126 * static_cast<double>(hdr[i]) + 0.7152 * static_cast<double>(hdr[i + 1U])
                                 + 0.0722 * static_cast<double>(hdr[i + 2U]);
        if (!(luminance > 1e-6)) { continue; }
        log_sum += std::clamp(std::log2(luminance), kMinLogLuminance, kMaxLogLuminance);
        ++counted;
    }
    return counted == 0U ? 0.0 : std::log2(kExposureKey) - log_sum / static_cast<double>(counted);
}

// Sampled pixels whose largest channel misses post's encoding of hdr * 2^ev by more than `levels`.
struct Prediction
{
    std::size_t sampled = 0;
    std::size_t missed = 0;
    double worst = 0.0;
};

Prediction
  compare_with_exposed_hdr(const std::vector<uint8_t> &frame, const std::vector<float> &hdr, double ev, double levels)
{
    Prediction result;
    const double scale = std::exp2(ev);
    const std::size_t pixels = std::min(frame.size(), hdr.size()) / 4U;
    for (std::size_t pixel = 0; pixel < pixels; pixel += 3U) {
        double miss = 0.0;
        for (std::size_t c = 0; c < 3U; ++c) {
            const double predicted = post_display_level(static_cast<double>(hdr[pixel * 4U + c]) * scale);
            miss = std::max(miss, std::abs(static_cast<double>(frame[pixel * 4U + c]) - predicted));
        }
        ++result.sampled;
        result.worst = std::max(result.worst, miss);
        if (miss > levels) { ++result.missed; }
    }
    return result;
}

// Mean luminance of each `block` x `block` square, row by row: the picture two frames of one view share at any scale.
std::vector<double> block_luminance(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height, uint32_t block)
{
    std::vector<double> means;
    for (uint32_t by = 0; by + block <= height; by += block) {
        for (uint32_t bx = 0; bx + block <= width; bx += block) {
            double sum = 0.0;
            for (uint32_t y = by; y < by + block; ++y) {
                for (uint32_t x = bx; x < bx + block; ++x) {
                    sum += luminance_of(rgba, static_cast<size_t>(y) * width + x);
                }
            }
            means.push_back(sum / static_cast<double>(block * block));
        }
    }
    return means;
}

// Mean largest-channel change of each `block` x `block` square between two captures of one view, row by row.
std::vector<double> block_change(const std::vector<uint8_t> &a,
  const std::vector<uint8_t> &b,
  uint32_t width,
  uint32_t height,
  uint32_t block)
{
    std::vector<double> means;
    for (uint32_t by = 0; by + block <= height; by += block) {
        for (uint32_t bx = 0; bx + block <= width; bx += block) {
            double sum = 0.0;
            for (uint32_t y = by; y < by + block; ++y) {
                for (uint32_t x = bx; x < bx + block; ++x) {
                    sum += channel_delta(a, b, static_cast<size_t>(y) * width + x);
                }
            }
            means.push_back(sum / static_cast<double>(block * block));
        }
    }
    return means;
}

// Mean absolute difference of two equally long series.
double mean_difference(const std::vector<double> &a, const std::vector<double> &b)
{
    double difference = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) { difference += std::abs(a[i] - b[i]); }
    return a.empty() ? 0.0 : difference / static_cast<double>(a.size());
}

// Pearson's correlation of two equally long series; 0 when either is flat.
double correlation(const std::vector<double> &a, const std::vector<double> &b)
{
    const double n = static_cast<double>(a.size());
    const double mean_a = std::accumulate(a.begin(), a.end(), 0.0) / n;
    const double mean_b = std::accumulate(b.begin(), b.end(), 0.0) / n;
    double cross = 0.0;
    double var_a = 0.0;
    double var_b = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        cross += (a[i] - mean_a) * (b[i] - mean_b);
        var_a += (a[i] - mean_a) * (a[i] - mean_a);
        var_b += (b[i] - mean_b) * (b[i] - mean_b);
    }
    return var_a > 0.0 && var_b > 0.0 ? cross / std::sqrt(var_a * var_b) : 0.0;
}

double median(std::vector<double> values)
{
    if (values.empty()) { return std::nan(""); }
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2U);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

// The radiance ratio between two frames read off their levels, over pixels both show below the clip.
double median_radiance_ratio(const std::vector<uint8_t> &frame, const std::vector<uint8_t> &reference)
{
    std::vector<double> ratios;
    const std::size_t pixels = std::min(frame.size(), reference.size()) / 4U;
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        bool usable = true;
        double frame_luminance = 0.0;
        double reference_luminance = 0.0;
        for (std::size_t c = 0; c < 3U; ++c) {
            const double level = frame[pixel * 4U + c];
            const double reference_level = reference[pixel * 4U + c];
            usable = usable && level >= 24.0 && level <= 240.0 && reference_level >= 24.0 && reference_level <= 240.0;
            const double weight = c == 0U ? 0.2126 : (c == 1U ? 0.7152 : 0.0722);
            frame_luminance += weight * post_linear_level(level);
            reference_luminance += weight * post_linear_level(reference_level);
        }
        if (usable && reference_luminance > 0.0) { ratios.push_back(frame_luminance / reference_luminance); }
    }
    EXPECT_GT(ratios.size(), pixels / 20U) << "too few pixels sit below the clip in both frames to compare";
    return median(std::move(ratios));
}

}// namespace

// Manual exposure multiplies the HDR radiance by 2^EV before ACES: each frame is post's encoding of its own radiance.
TEST(GoldenRender, ExposureScalesTheRadianceBeforeTheTonemap)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    harness.useForwardRaster();
    auto &renderer_vars = harness.settings.renderer;
    // The cascades cost most of an llvmpipe frame (and a QEMU shard's), and nothing here grades them.
    harness.settings.scene.shadows_enabled = false;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t hdr_width = 0;
    uint32_t hdr_height = 0;
    const std::vector<float> hdr = harness.capture_hdr_frame(hdr_width, hdr_height);
    ASSERT_FALSE(hdr.empty()) << "HDR capture returned no texels.";

    std::map<int, std::vector<uint8_t>> frames;
    for (const int ev : { 0, 1, -1 }) {
        renderer_vars.exposure_ev = static_cast<float>(ev);
        harness.render_frames(SETTLE_FRAMES);
        const std::array<float, 2> state = render_frame_reading_exposure(harness, 0.0);
        EXPECT_EQ(state[0], static_cast<float>(ev)) << "manual exposure wrote another adapted EV";
        EXPECT_EQ(state[1], static_cast<float>(ev)) << "manual exposure wrote another target EV";

        uint32_t width = 0;
        uint32_t height = 0;
        frames[ev] = harness.capture_frame(width, height);
        ASSERT_EQ(width, hdr_width);
        ASSERT_EQ(height, hdr_height);

        const Prediction prediction = compare_with_exposed_hdr(frames[ev], hdr, ev, 1.5);
        EXPECT_LE(prediction.missed * 1000U, prediction.sampled)
          << "EV " << ev << ": " << prediction.missed << " of " << prediction.sampled
          << " sampled pixels miss post's encoding of the exposed HDR radiance by more than 1.5 levels (worst "
          << prediction.worst << ")";
    }
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost changing the exposure.";

    const double doubled = median_radiance_ratio(frames[1], frames[0]);
    const double halved = median_radiance_ratio(frames[-1], frames[0]);
    EXPECT_NEAR(doubled, 2.0, 0.1) << "EV +1 should double the radiance the frame shows";
    EXPECT_NEAR(halved, 0.5, 0.025) << "EV -1 should halve the radiance the frame shows";
}

// Auto exposure moves the EV towards the frame's log-average from a dark and a bright scene, then holds it.
TEST(GoldenRender, AutoExposureConvergesOnTheFramesLogAverageAndHolds)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    auto &renderer_vars = harness.settings.renderer;
    // Looking down onto the lit ground: from the golden view the photo sky's fixed radiance outweighs any Sun.
    harness.camera->set_camera_position(glm::vec3(0.0F, 8.0F, 12.0F));
    harness.camera->set_orientation(-90.0F, -45.0F);
    scene_vars.shadows_enabled = false;
    harness.render_frames(WARMUP_FRAMES);

    constexpr double kSpeed = 5.0;
    constexpr int kAdaptFrames = 25;
    // One frame of 0.1 s at rate 5 covers 1 - e^-0.5 of the distance, the reduce's framerate-independent step.
    const double first_step = 1.0 - std::exp(-kFrameSeconds * kSpeed);

    struct Lighting
    {
        const char *name;
        float radiance;
        float ambient;
        bool exposesUp;
    };
    for (const Lighting &lighting :
      { Lighting{ "dark", 0.05F, 0.02F, true }, Lighting{ "bright", 40.0F, 2.0F, false } }) {
        SCOPED_TRACE(lighting.name);
        scene_vars.directional_light_radiance = lighting.radiance;
        scene_vars.directional_light_ambient = lighting.ambient;
        // Manual writes [0, 0], which the adaptation then starts from.
        renderer_vars.auto_exposure = false;
        renderer_vars.exposure_ev = 0.0F;
        harness.render_frames(SETTLE_FRAMES);

        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<float> hdr = harness.capture_hdr_frame(width, height);
        ASSERT_FALSE(hdr.empty());
        const double expected_target = log_average_target_ev(hdr);

        renderer_vars.auto_exposure = true;
        renderer_vars.auto_exposure_speed = static_cast<float>(kSpeed);
        std::vector<std::array<float, 2>> states;
        for (int frame = 0; frame < kAdaptFrames; ++frame) {
            states.push_back(render_frame_reading_exposure(harness, kFrameSeconds));
        }
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost adapting the exposure.";

        const auto target = static_cast<double>(states.back()[1]);
        GTEST_LOG_(INFO) << lighting.name << ": target EV " << target << ", log-average " << expected_target;
        // The bins quantise each pixel's log2 luminance to its bin's centre, 14/62 EV wide.
        EXPECT_NEAR(target, expected_target, 0.25) << "the target is not the frame's log-average";
        if (lighting.exposesUp) {
            EXPECT_GT(target, 1.0) << "a dark scene must expose up";
        } else {
            EXPECT_LT(target, -1.0) << "a bright scene must expose down";
        }
        EXPECT_NEAR(static_cast<double>(states.front()[0]), first_step * target, 0.02 + 0.01 * std::abs(target))
          << "the first frame did not move 1 - e^(-dt speed) of the way from 0";

        double previous = 0.0;
        for (std::size_t frame = 0; frame < states.size(); ++frame) {
            const auto adapted = static_cast<double>(states[frame][0]);
            const double moved = (adapted - previous) * (target > 0.0 ? 1.0 : -1.0);
            EXPECT_GE(moved, -1e-4) << "frame " << frame << " moved away from the target";
            EXPECT_LE(std::abs(adapted), std::abs(target) + 1e-3) << "frame " << frame << " overshot the target";
            previous = adapted;
        }
        EXPECT_NEAR(static_cast<double>(states.back()[0]), target, 0.02)
          << "did not converge in " << kAdaptFrames << " frames";

        // A static scene holds what it converged on, and post shows its HDR radiance at that EV.
        const std::array<float, 2> held = render_frame_reading_exposure(harness, kFrameSeconds);
        EXPECT_NEAR(static_cast<double>(held[0]), static_cast<double>(states.back()[0]), 2e-3)
          << "the converged exposure kept moving";
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        const Prediction prediction = compare_with_exposed_hdr(frame, hdr, static_cast<double>(held[0]), 1.5);
        EXPECT_LE(prediction.missed * 1000U, prediction.sampled)
          << prediction.missed << " of " << prediction.sampled
          << " sampled pixels miss post's encoding of the HDR radiance at the adapted EV " << held[0] << " (worst "
          << prediction.worst << ")";
    }
}

namespace {

// The bright pass's input as bloom.slang reads it: the half-resolution texel's 2x2 footprint, exposed, less 1.
std::vector<double> cpu_bright_pass(const std::vector<float> &hdr, uint32_t width, uint32_t height, double ev)
{
    const uint32_t half_width = std::max(width / 2U, 1U);
    const uint32_t half_height = std::max(height / 2U, 1U);
    const double scale = std::exp2(ev);
    std::vector<double> bright(static_cast<std::size_t>(half_width) * half_height * 3U, 0.0);
    for (uint32_t y = 0; y < half_height; ++y) {
        for (uint32_t x = 0; x < half_width; ++x) {
            for (std::size_t c = 0; c < 3U; ++c) {
                double sum = 0.0;
                for (uint32_t dy = 0; dy < 2U; ++dy) {
                    for (uint32_t dx = 0; dx < 2U; ++dx) {
                        const std::size_t pixel = static_cast<std::size_t>(2U * y + dy) * width + (2U * x + dx);
                        sum += static_cast<double>(hdr[pixel * 4U + c]);
                    }
                }
                bright[(static_cast<std::size_t>(y) * half_width + x) * 3U + c] =
                  std::max(0.25 * sum * scale - 1.0, 0.0);
            }
        }
    }
    return bright;
}

// bloom.slang's separable 9-tap Gaussian of the bright pass, horizontal then vertical, the edges clamped.
std::vector<double> cpu_blur(const std::vector<double> &bright, uint32_t width, uint32_t height)
{
    constexpr std::array<double, 5> kWeights = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };
    const auto pass = [&](const std::vector<double> &source, int dx, int dy) {
        std::vector<double> result(source.size(), 0.0);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                for (int tap = -4; tap <= 4; ++tap) {
                    const auto sx = static_cast<uint32_t>(
                      std::clamp(static_cast<int64_t>(x) + tap * dx, int64_t{ 0 }, static_cast<int64_t>(width) - 1));
                    const auto sy = static_cast<uint32_t>(
                      std::clamp(static_cast<int64_t>(y) + tap * dy, int64_t{ 0 }, static_cast<int64_t>(height) - 1));
                    const double weight = kWeights[static_cast<std::size_t>(std::abs(tap))];
                    for (std::size_t c = 0; c < 3U; ++c) {
                        result[(static_cast<std::size_t>(y) * width + x) * 3U + c] +=
                          weight * source[(static_cast<std::size_t>(sy) * width + sx) * 3U + c];
                    }
                }
            }
        }
        return result;
    };
    return pass(pass(bright, 1, 0), 0, 1);
}

// post's bilinear read of the half-resolution bloom at a full-resolution pixel's centre, edges clamped.
double upsampled_bloom(const std::vector<float> &bloom,
  uint32_t half_width,
  uint32_t half_height,
  uint32_t x,
  uint32_t y,
  std::size_t c)
{
    const auto clamped = [](double v, uint32_t size) {
        return static_cast<uint32_t>(std::clamp(v, 0.0, static_cast<double>(size - 1U)));
    };
    const double u = (static_cast<double>(x) + 0.5) * 0.5 - 0.5;
    const double v = (static_cast<double>(y) + 0.5) * 0.5 - 0.5;
    const double u0 = std::floor(u);
    const double v0 = std::floor(v);
    const double fu = u - u0;
    const double fv = v - v0;
    const auto at = [&](double tu, double tv) {
        const std::size_t texel =
          static_cast<std::size_t>(clamped(tv, half_height)) * half_width + clamped(tu, half_width);
        return static_cast<double>(bloom[texel * 4U + c]);
    };
    return (1.0 - fv) * ((1.0 - fu) * at(u0, v0) + fu * at(u0 + 1.0, v0))
           + fv * ((1.0 - fu) * at(u0, v0 + 1.0) + fu * at(u0 + 1.0, v0 + 1.0));
}

}// namespace

// Bloom keeps the exposed radiance above 1, spreads it four half-resolution texels and loses none; post adds it.
TEST(GoldenRender, BloomSpreadsABrightCardsEnergyAndConservesIt)
{
    SKIP_WITHOUT_GPU();

    nlohmann::json glowing;
    glowing["pbrMetallicRoughness"] = { { "baseColorFactor", { 0.0, 0.0, 0.0, 1.0 } } };
    glowing["emissiveFactor"] = { 1.0, 1.0, 1.0 };
    glowing["extensions"]["KHR_materials_emissive_strength"] = { { "emissiveStrength", 6.0 } };
    glowing["doubleSided"] = true;
    const auto scene = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_bloom_card.gltf", [&](auto &gltf) {
        make_card_scene(gltf, { glowing }, { { 0, nlohmann::json::object() } });
        gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_materials_emissive_strength" });
    });
    ASSERT_TRUE(scene.has_value()) << "could not write the glowing card";

    ScopedModelOverride card(scene->string().c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    // Only the card's emission lights the frame, on the navy at 0.04, which no exposure here lifts above 1.
    scene_vars.shadows_enabled = false;
    scene_vars.skybox_enabled = false;
    scene_vars.directional_light_radiance = 0.0F;
    scene_vars.directional_light_ambient = 0.0F;
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, 15.0F));
    harness.camera->set_orientation(-90.0F, 0.0F);
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> unbloomed = harness.capture_frame(width, height);
    const std::vector<float> hdr = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(hdr.empty());

    constexpr float kStrength = 0.6F;
    renderer_vars.bloom_strength = kStrength;
    harness.render_frames(SETTLE_FRAMES);
    uint32_t half_width = 0;
    uint32_t half_height = 0;
    const std::vector<float> bloom = harness.capture_bloom_frame(half_width, half_height);
    const std::vector<uint8_t> bloomed = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost blooming the card.";
    ASSERT_EQ(half_width, width / 2U);
    ASSERT_EQ(half_height, height / 2U);

    const std::vector<double> bright = cpu_bright_pass(hdr, width, height, 0.0);
    const std::vector<double> blurred = cpu_blur(bright, half_width, half_height);
    std::size_t blur_misses = 0;
    double kept = 0.0;
    double spread = 0.0;
    double outside = 0.0;
    double beyond = 0.0;
    std::size_t lit_texels = 0;
    constexpr int kRadius = 4;
    for (uint32_t y = 0; y < half_height; ++y) {
        for (uint32_t x = 0; x < half_width; ++x) {
            const std::size_t texel = static_cast<std::size_t>(y) * half_width + x;
            double near_source = 0.0;
            for (int dy = -kRadius; dy <= kRadius; ++dy) {
                for (int dx = -kRadius; dx <= kRadius; ++dx) {
                    const auto sx = static_cast<int64_t>(x) + dx;
                    const auto sy = static_cast<int64_t>(y) + dy;
                    if (sx < 0 || sy < 0 || sx >= half_width || sy >= half_height) { continue; }
                    near_source = std::max(near_source,
                      bright[(static_cast<std::size_t>(sy) * half_width + static_cast<std::size_t>(sx)) * 3U]);
                }
            }
            for (std::size_t c = 0; c < 3U; ++c) {
                const double source = bright[texel * 3U + c];
                const auto glow = static_cast<double>(bloom[texel * 4U + c]);
                const double expected = blurred[texel * 3U + c];
                // The half-float targets round each of the passes' sums to 11 bits.
                if (std::abs(glow - expected) > 0.01 + 0.004 * expected) { ++blur_misses; }
                kept += source;
                spread += glow;
                if (source <= 0.0) { outside += glow; }
                if (near_source <= 0.0) { beyond += glow; }
            }
            if (bright[texel * 3U] > 0.0) { ++lit_texels; }
        }
    }
    GTEST_LOG_(INFO) << "bright pass " << kept << ", bloom " << spread << ", outside the card " << outside << " over "
                     << lit_texels << " lit texels";
    ASSERT_GT(lit_texels, 200U) << "the card left too few texels above the threshold to measure";
    EXPECT_NEAR(spread / kept, 1.0, 0.02) << "the blur lost or added energy";
    EXPECT_EQ(blur_misses, 0U) << "bloom texels that are not the 9-tap Gaussian of the CPU bright pass";
    EXPECT_GT(outside / spread, 0.03) << "the blur did not spread energy beyond the card";
    EXPECT_EQ(beyond, 0.0) << "energy reached past the 9-tap kernel's four texels";

    // Post adds strength times the upsampled bloom to the exposed radiance; far from the card the frame is unchanged.
    std::size_t sampled = 0;
    std::size_t missed = 0;
    std::size_t far_changed = 0;
    double worst = 0.0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
            double miss = 0.0;
            double glow_here = 0.0;
            for (std::size_t c = 0; c < 3U; ++c) {
                const double glow = upsampled_bloom(bloom, half_width, half_height, x, y, c);
                glow_here = std::max(glow_here, glow);
                const double predicted =
                  post_display_level(static_cast<double>(hdr[pixel * 4U + c]) + static_cast<double>(kStrength) * glow);
                miss = std::max(miss, std::abs(static_cast<double>(bloomed[pixel * 4U + c]) - predicted));
            }
            ++sampled;
            worst = std::max(worst, miss);
            if (miss > 1.5) { ++missed; }
            if (glow_here == 0.0 && channel_delta(bloomed, unbloomed, pixel) != 0) { ++far_changed; }
        }
    }
    EXPECT_LE(missed * 1000U, sampled) << missed << " of " << sampled
                                       << " pixels miss post's encoding of the radiance plus the bloom (worst " << worst
                                       << ")";
    EXPECT_EQ(far_changed, 0U) << "pixels the bloom does not reach changed with it";

    // The clouds composite before the threshold, and a traced frame blooms too; sync validation watches both.
    scene_vars.clouds_enabled = true;
    harness.render_frames(SETTLE_FRAMES);
    if (harness.renderer->supportsHardwareRaytracing()) {
        renderer_vars.pathTracing = true;
        renderer_vars.pathTracingSamplesPerPixel = 1;
        renderer_vars.pathTracingMaxBounces = 1;
        harness.render_frames(SETTLE_FRAMES);
    }
    EXPECT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost blooming clouds or a traced frame.";
}

// SSAO scales the ambient light only: the Sun alone renders the same with it, the sky alone darkens, on both paths.
TEST(GoldenRender, SsaoDarkensTheAmbientLightAndNeverTheDirectLight)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture = [&](float ssao) {
        renderer_vars.ssao_strength = ssao;
        harness.render_frames(SETTLE_FRAMES);
        return harness.capture_frame(width, height);
    };
    for (const RasterizationMode path : { RasterizationMode::Forward, RasterizationMode::Deferred }) {
        const char *name = path == RasterizationMode::Forward ? "forward" : "deferred";
        renderer_vars.rasterizationMode = path;

        // The Sun alone: no ambient term, so SSAO has nothing it may darken.
        scene_vars.directional_light_radiance = 5.0F;
        scene_vars.directional_light_ambient = 0.0F;
        const std::vector<uint8_t> sun_without = capture(0.0F);
        const std::vector<uint8_t> sun_with = capture(1.0F);
        ASSERT_FALSE(sun_without.empty());
        EXPECT_EQ(sun_with, sun_without) << name << ": SSAO changed a frame the Sun alone lights";

        // The sky alone, through the baked environment.
        scene_vars.directional_light_radiance = 0.0F;
        scene_vars.directional_light_ambient = 1.0F;
        const std::vector<uint8_t> sky_without = capture(0.0F);
        const std::vector<uint8_t> sky_with = capture(1.0F);
        ASSERT_EQ(sky_with.size(), sky_without.size());
        std::size_t darkened = 0;
        std::size_t brightened = 0;
        double removed = 0.0;
        const std::size_t pixels = sky_with.size() / 4U;
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            const double delta = luminance_of(sky_without, pixel) - luminance_of(sky_with, pixel);
            removed += delta;
            if (delta >= 3.0) { ++darkened; }
            if (delta <= -1.0) { ++brightened; }
        }
        GTEST_LOG_(INFO) << name << ": SSAO darkened " << darkened << " of " << pixels
                         << " pixels by 3 levels or more, " << removed / static_cast<double>(pixels)
                         << " levels a pixel";
        // Measured on llvmpipe: 1.3 % on both paths, at the dinosaurs' feet and folds.
        EXPECT_GT(darkened * 200U, pixels) << name << ": SSAO darkened under 0.5 % of the frame's ambient light";
        EXPECT_EQ(brightened, 0U) << name << ": SSAO brightened pixels, which only darkening may do";
        EXPECT_FALSE(harness.renderer->hasDeviceLost()) << name;
    }
}

// Post filters a half-scale scene and its clouds up to the output: the full-scale picture, softer, in every mode.
TEST(GoldenRender, HalfRenderScaleDrawsTheFullScalePictureInEveryMode)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    const ScopedValidationErrorCounter validation;
    auto &renderer_vars = harness.useForwardRaster();
    // Both draw at half the scene extent, so a target left at the output's shows too.
    renderer_vars.bloom_strength = 0.6F;
    renderer_vars.ssao_strength = 0.7F;
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture_at = [&](float scale) {
        renderer_vars.render_scale = scale;
        harness.render_frames(SETTLE_FRAMES);
        return harness.capture_frame(width, height);
    };
    struct Mode
    {
        const char *name;
        RasterizationMode path;
        bool traced;
    };
    // 24-pixel squares: the half-scale frame's pixels are filtered, its picture is not moved or cropped.
    constexpr uint32_t kBlock = 24U;
    std::vector<Mode> modes = { { "forward", RasterizationMode::Forward, false },
        { "deferred", RasterizationMode::Deferred, false } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back({ "ray traced", RasterizationMode::Forward, true });
    }
    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.path;
        renderer_vars.raytracing = mode.traced;
        const std::vector<uint8_t> full = capture_at(1.0F);
        ASSERT_FALSE(full.empty()) << mode.name;
        const uint32_t full_width = width;
        const uint32_t full_height = height;
        const std::vector<uint8_t> half = capture_at(0.5F);
        ASSERT_EQ(harness.renderer->sceneExtent(), (vk::Extent2D{ (full_width + 1U) / 2U, (full_height + 1U) / 2U }))
          << mode.name;
        ASSERT_EQ(width, full_width) << mode.name << ": the frame is the output's, not the scene's";
        ASSERT_EQ(height, full_height) << mode.name;
        ASSERT_EQ(half.size(), full.size()) << mode.name;

        EXPECT_GT(luminance_stddev(half), 5.0) << mode.name << ": the half-scale frame is flat";
        EXPECT_GT(distinct_luminance_buckets(half), 32U) << mode.name;
        EXPECT_NE(half, full) << mode.name << ": the scale changed nothing";

        const std::vector<double> full_blocks = block_luminance(full, width, height, kBlock);
        const std::vector<double> half_blocks = block_luminance(half, width, height, kBlock);
        const double difference = mean_difference(full_blocks, half_blocks);
        const double structure = correlation(full_blocks, half_blocks);
        GTEST_LOG_(INFO) << mode.name << ": squares differ by " << difference << " levels, correlation " << structure;
        EXPECT_LT(difference, 4.0) << mode.name << ": the half-scale picture is not the full-scale one";
        EXPECT_GT(structure, 0.97) << mode.name << ": the half-scale picture is moved, cropped or missing";
        EXPECT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
    }

    // The HDR target post reads is the scene's own size.
    renderer_vars.raytracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    renderer_vars.render_scale = 0.5F;
    harness.render_frames(SETTLE_FRAMES);
    uint32_t hdr_width = 0;
    uint32_t hdr_height = 0;
    const std::vector<float> hdr = harness.capture_hdr_frame(hdr_width, hdr_height);
    EXPECT_EQ((vk::Extent2D{ hdr_width, hdr_height }), harness.renderer->sceneExtent());
    EXPECT_EQ(hdr.size(), static_cast<std::size_t>(hdr_width) * hdr_height * 4U);

    // The clouds reach post through its sampler too: at half scale they change the frame where they do at full.
    auto &scene_vars = harness.settings.scene;
    scene_vars.skybox_enabled = false;
    scene_vars.cloud_coverage_threshold = 0.3F;
    // Behind the slab, where the half-resolution cloud golden puts them: cloud in the sky all around it.
    place_cloud_box(scene_vars, glm::vec3(0.0F, 9.0F, -20.0F), glm::vec3(40.0F, 6.0F, 4.0F), 1.0F);
    const auto cloud_change = [&](float scale) {
        scene_vars.clouds_enabled = false;
        const std::vector<uint8_t> off = capture_at(scale);
        scene_vars.clouds_enabled = true;
        const std::vector<uint8_t> on = capture_at(scale);
        EXPECT_EQ(on.size(), off.size());
        return on.size() == off.size() ? block_change(off, on, width, height, kBlock) : std::vector<double>{};
    };
    const std::vector<double> full_clouds = cloud_change(1.0F);
    const std::vector<double> half_clouds = cloud_change(0.5F);
    ASSERT_EQ(half_clouds.size(), full_clouds.size());
    const auto clouded = static_cast<std::size_t>(
      std::count_if(full_clouds.begin(), full_clouds.end(), [](double change) { return change > 5.0; }));
    const double cloud_difference = mean_difference(full_clouds, half_clouds);
    const double cloud_structure = correlation(full_clouds, half_clouds);
    GTEST_LOG_(INFO) << "clouds: " << clouded << " of " << full_clouds.size()
                     << " squares clouded, the change differs by " << cloud_difference << " levels, correlation "
                     << cloud_structure;
    ASSERT_GT(clouded, full_clouds.size() / 10U) << "the clouds change too little of the frame to judge";
    EXPECT_LT(cloud_difference, 4.0) << "the half-scale clouds are not the full-scale ones";
    EXPECT_GT(cloud_structure, 0.9) << "the half-scale clouds are moved, cropped or missing";
    EXPECT_FALSE(harness.renderer->hasDeviceLost());
    EXPECT_EQ(validation.errorCount(), 0) << "the scale changes raised validation errors";
}
