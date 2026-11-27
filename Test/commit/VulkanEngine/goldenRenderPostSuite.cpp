// Goldens: the post chain's exposure and auto exposure, each graded against the frame's own HDR radiance.

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
    harness.render_frames(WARMUP_FRAMES);

    constexpr double kSpeed = 3.0;
    constexpr int kAdaptFrames = 40;
    // One frame of 0.1 s at rate 3 covers 1 - e^-0.3 of the distance, the reduce's framerate-independent step.
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
