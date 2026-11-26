// Goldens: cascaded and punctual shadows, punctual and directional lights, and the deferred path matching forward.

#include "GoldenRenderHarness.hpp"

import kataglyphis.vulkan.renderer;

using namespace Kataglyphis::TestSupport::GoldenRender;

namespace {

using Kataglyphis::Test::GoldenMetrics::compare_shadow_masks;
using Kataglyphis::Test::GoldenMetrics::ShadowAgreement;

using Kataglyphis::TestSupport::frameCaptureAvailable;

// A grey plane under a red point light (x -1.2) and a green spot light (x 1.5) pointing down; OxidANT's fixture.
std::string punctual_lights_model() { return Kataglyphis::TestSupport::oxidantGltfFixture("point_light.gltf"); }
// That plane under a 4098 lx directional light, the Colosseum's sun, on a turned and scaled node, and the red light.
constexpr const char *DIRECTIONAL_LIGHT_MODEL = "Models/GltfTest/directional_light.gltf";

}// namespace

// Disabled: measures what each PCF radius changes on the rig (MAX_PCF_RADIUS, docs/source/gpu-golden-testing.md).
TEST(GoldenRender, DISABLED_PcfRadiusSweepMeasuresTheShadowRig)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    const auto &timings = harness.feedback.gpuTimings;
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;
    harness.render_frames(WARMUP_FRAMES);

    // Past GpuPassAverage's window, so each radius's pass times are its own.
    constexpr int TIMED_FRAMES = static_cast<int>(Kataglyphis::GpuTimingSubsystem::GpuPassAverage::WINDOW) + 3;
    using Frame = std::vector<uint8_t>;
    struct Sample
    {
        int radius;
        Frame on;
        Frame off;
        double main_ms;
        double shadow_ms;
        double wall_ms;
    };
    std::vector<Sample> samples;
    uint32_t width = 0;
    uint32_t height = 0;
    for (int radius = 0; radius <= MAX_PCF_RADIUS; ++radius) {
        scene_vars.pcf_radius = radius;
        scene_vars.cascaded_shadow_intensity = 1.0F;
        const auto start = std::chrono::steady_clock::now();
        harness.render_frames(TIMED_FRAMES);
        const double wall_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / TIMED_FRAMES;
        const double main_ms = static_cast<double>(timings.pass_ms[static_cast<size_t>(GpuTimedPass::Main)]);
        const double shadow_ms =
          static_cast<double>(timings.pass_ms[static_cast<size_t>(GpuTimedPass::ShadowCascades)]);
        std::vector<uint8_t> on = harness.capture_frame(width, height);
        // The off frame holds this radius's GUI text, so overlay pixels that move with the slider can be masked.
        scene_vars.cascaded_shadow_intensity = 0.0F;
        harness.render_frames(SETTLE_FRAMES);
        std::vector<uint8_t> off = harness.capture_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost at radius " << radius;
        samples.push_back({ radius, std::move(on), std::move(off), main_ms, shadow_ms, wall_ms });
    }
    ASSERT_GE(samples.size(), 2U);

    // A kernel without a receiver-plane bias darkens lit receivers too, so penumbra and lit area are measured apart.
    const size_t pixel_count = static_cast<size_t>(width) * height;
    const std::vector<uint8_t> &unshadowed = samples.front().off;
    const std::vector<uint8_t> &hard = samples.front().on;
    std::vector<int> shadowed_prefix((static_cast<size_t>(width) + 1U) * (height + 1U), 0);
    std::vector<bool> noisy(pixel_count, false);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * width + x;
            for (const Sample &sample : samples) {
                noisy[pixel] =
                  noisy[pixel] || std::abs(luminance_of(sample.off, pixel) - luminance_of(unshadowed, pixel)) >= 1.0;
            }
            const int shadowed = luminance_of(unshadowed, pixel) - luminance_of(hard, pixel) >= 2.0 ? 1 : 0;
            const size_t at = (static_cast<size_t>(y) + 1U) * (width + 1U) + x + 1U;
            shadowed_prefix[at] = shadowed + shadowed_prefix[at - 1U] + shadowed_prefix[at - (width + 1U)]
                                  - shadowed_prefix[at - (width + 1U) - 1U];
        }
    }
    // Near: within PENUMBRA_PX of the smallest radius's shadow, where softening shows; far: lit pixels beyond it.
    constexpr uint32_t PENUMBRA_PX = 16U;
    const auto shadow_within = [&](uint32_t x, uint32_t y) {
        const size_t x0 = x > PENUMBRA_PX ? x - PENUMBRA_PX : 0U;
        const size_t y0 = y > PENUMBRA_PX ? y - PENUMBRA_PX : 0U;
        const size_t x1 = std::min<size_t>(width, x + PENUMBRA_PX + 1U);
        const size_t y1 = std::min<size_t>(height, y + PENUMBRA_PX + 1U);
        const auto row = [&](size_t yy) { return yy * (width + 1U); };
        return shadowed_prefix[row(y1) + x1] - shadowed_prefix[row(y0) + x1] - shadowed_prefix[row(y1) + x0]
                 + shadowed_prefix[row(y0) + x0]
               > 0;
    };
    std::vector<int> region(pixel_count, 0);
    size_t near_pixels = 0;
    size_t far_pixels = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * width + x;
            if (noisy[pixel]) { continue; }
            region[pixel] = shadow_within(x, y) ? 1 : 2;
            ++(region[pixel] == 1 ? near_pixels : far_pixels);
        }
    }
    ASSERT_GT(near_pixels, 0U) << "No shadow at the smallest radius; the sweep measured nothing.";
    ASSERT_GT(far_pixels, 0U);

    // Mean change and share moving >= 3 levels (0..255 luma); signed_delta: how much darker a is than b.
    const auto compare = [&](const Frame &a, const Frame &b, int which, bool signed_delta) {
        double sum = 0.0;
        size_t moved = 0;
        size_t count = 0;
        for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
            if (region[pixel] != which) { continue; }
            const double raw = luminance_of(b, pixel) - luminance_of(a, pixel);
            const double delta = signed_delta ? raw : std::abs(raw);
            sum += delta;
            if (delta >= 3.0) { ++moved; }
            ++count;
        }
        return std::pair{ sum / static_cast<double>(count), static_cast<double>(moved) / static_cast<double>(count) };
    };

    std::cout << "[  INFO ] PCF sweep on " << SHADOW_RIG_MODEL << ", " << width << "x" << height << "; near "
              << near_pixels << " px, far-lit " << far_pixels << " px\n"
              << "[  INFO ] radius taps | near vs prev: MAD >=3lvl | far-lit darkening: mean >=3lvl | main_ms "
                 "shadow_ms wall_ms\n";
    for (size_t i = 0; i < samples.size(); ++i) {
        const Sample &sample = samples[i];
        const auto [near_mad, near_moved] =
          i == 0 ? std::pair{ 0.0, 0.0 } : compare(sample.on, samples[i - 1].on, 1, false);
        const auto [far_dark, far_moved] = compare(sample.on, unshadowed, 2, true);
        const int taps = (2 * sample.radius + 1) * (2 * sample.radius + 1);
        std::cout << "[  INFO ] " << sample.radius << " " << taps << " | " << near_mad << " " << near_moved << " | "
                  << far_dark << " " << far_moved << " | " << sample.main_ms << " " << sample.shadow_ms << " "
                  << sample.wall_ms << "\n";
    }

    if (const auto out_path = Kataglyphis::environmentVariable("KATAGLYPHIS_FRAME_DUMP")) {
        for (const Sample &sample : samples) {
            const std::string path = *out_path + "-pcf" + std::to_string(sample.radius) + ".png";
            EXPECT_NE(stbi_write_png(path.c_str(),
                        static_cast<int>(width),
                        static_cast<int>(height),
                        4,
                        sample.on.data(),
                        static_cast<int>(width) * 4),
              0)
              << "Failed to write " << path;
        }
    }
}

// Guards against a shadow map nothing samples; the rig, not the debug scene, is what makes it assertable.
TEST(GoldenRender, ShadowsDarkenSomePixels)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    harness.useForwardRaster();

    // Only intensity varies, isolating "the shadow map is sampled" from "the shadow pass runs".
    scene_vars.shadows_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;

    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> without_shadows = harness.capture_frame(width, height);

    // The overlay's FPS readout flips hundreds of pixels, so mask out whatever moves between identical states.
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> noise_reference = harness.capture_frame(width, height);
    ASSERT_FALSE(without_shadows.empty()) << "Capture without shadows returned no pixels.";

    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> with_shadows = harness.capture_frame(width, height);
    ASSERT_FALSE(with_shadows.empty()) << "Capture with shadows returned no pixels.";

    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_EQ(without_shadows.size(), with_shadows.size());

    // Skybox and overlay swamp a whole-frame mean; per pixel, shadowing can only darken.
    constexpr double CHANGE_THRESHOLD = 4.0;

    const size_t pixel_count = with_shadows.size() / 4U;
    size_t darkened = 0;
    size_t brightened = 0;
    size_t unstable = 0;
    double shadowed_region_sum_with = 0.0;
    double shadowed_region_sum_without = 0.0;

    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        // Moved between identical captures: overlay or dither, not shadow.
        if (std::abs(luminance_of(without_shadows, pixel) - luminance_of(noise_reference, pixel)) > CHANGE_THRESHOLD) {
            ++unstable;
            continue;
        }

        const double lum_without = luminance_of(without_shadows, pixel);
        const double lum_with = luminance_of(with_shadows, pixel);
        const double delta = lum_without - lum_with;

        if (delta > CHANGE_THRESHOLD) {
            ++darkened;
            shadowed_region_sum_with += lum_with;
            shadowed_region_sum_without += lum_without;
        } else if (delta < -CHANGE_THRESHOLD) {
            ++brightened;
        }
    }

    const double darkened_fraction =
      pixel_count == 0U ? 0.0 : static_cast<double>(darkened) / static_cast<double>(pixel_count);

    GTEST_LOG_(INFO) << "whole-frame mean luminance: intensity 0.0 = " << mean_luminance(without_shadows)
                     << ", intensity 1.0 = " << mean_luminance(with_shadows) << "; pixels darkened = " << darkened
                     << " (" << (darkened_fraction * 100.0) << "%), brightened = " << brightened
                     << ", unstable/overlay skipped = " << unstable << " of " << pixel_count;

    // 1. A shadowed region must exist at all.
    ASSERT_GT(darkened, 0U) << "Not a single pixel changed when the shadow intensity went from 0.0 to 1.0. "
                               "The cascaded shadow map is rendered but its result never reaches the lighting "
                               "shaders.";
    // Measured, not tuned: shadow-pass culling only halves the signal, so never lower this to pass.
    EXPECT_GT(darkened_fraction, 0.04)
      << "Only " << (darkened_fraction * 100.0)
      << "% of pixels were meaningfully darkened by shadows; expected a visible shadowed region.";

    // 2. Over that region the mean luminance with shadows must be lower.
    const double mean_shadowed = shadowed_region_sum_with / static_cast<double>(darkened);
    const double mean_unshadowed = shadowed_region_sum_without / static_cast<double>(darkened);
    EXPECT_LT(mean_shadowed, mean_unshadowed)
      << "Shadows did not darken the shadowed region: mean luminance " << mean_shadowed
      << " at shadow intensity 1.0 vs " << mean_unshadowed << " at intensity 0.0.";

    // 3. Shadowing only attenuates, so brightened pixels are overlay noise and must stay a minority.
    EXPECT_GT(darkened, brightened * 5U) << darkened << " pixels darkened but " << brightened
                                         << " brightened - raising the shadow intensity must not brighten the image.";
}

// A counter assertion, not a pixel oracle, so no classifier or tonemap hazard.
TEST(GoldenRender, ShadowCasterStatsAreReportedAndZeroWhenShadowsAreOff)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    auto &scene_vars = harness.settings.scene;
    harness.useForwardRaster();

    scene_vars.shadows_enabled = true;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    ASSERT_GT(harness.feedback.visibility.shadow_casters_total, 0U)
      << "the renderer reported considering no shadow casters at all; "
         "the shadow visibility counters are not being written";
    EXPECT_LE(harness.feedback.visibility.shadow_casters_drawn, harness.feedback.visibility.shadow_casters_total);

    scene_vars.shadows_enabled = false;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_EQ(harness.feedback.visibility.shadow_casters_total, 0U)
      << "shadow caster counters must be zeroed when the shadow pass does not run";
    EXPECT_EQ(harness.feedback.visibility.shadow_casters_drawn, 0U)
      << "shadow caster counters must be zeroed when the shadow pass does not run";
}

// The raster pass samples the cascade array with shadows off too; a pass that never ran left it UNDEFINED.
TEST(GoldenRender, ShadowsOffFromTheFirstFrameLeaveTheCascadeArraySampleable)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    harness.useForwardRaster();
    harness.settings.scene.shadows_enabled = false;
    // After the harness: the arm64 runner's PowerVR ICD logs loader errors while devices are enumerated.
    ScopedValidationErrorCounter validation_errors;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost with shadows off.";

    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "a frame with the shadow pass skipped logged a validation error - VUID-vkCmdDraw-None-09600 "
         "if the cascade depth array was sampled before anything moved it out of UNDEFINED";
}

// The default scene's casters all sit inside every cascade, so only a far-off caster makes the flag observable.
TEST(GoldenRender, DisablingFrustumCullingAlsoDisablesShadowCasterCulling)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    auto &scene_vars = harness.settings.scene;
    auto &renderer_vars = harness.useForwardRaster();

    scene_vars.shadows_enabled = true;

    const glm::mat4 far_placement = glm::translate(glm::mat4(1.0F), glm::vec3(5000.0F, 0.0F, 0.0F));
    const std::optional<uint32_t> far_model = harness.renderer->addModel(SHADOW_RIG_MODEL, far_placement);
    ASSERT_TRUE(far_model.has_value()) << "the far-away caster model failed to load";

    renderer_vars.frustum_culling_enabled = false;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    ASSERT_GT(harness.feedback.visibility.shadow_casters_total, 0U)
      << "the renderer reported considering no shadow casters at all; "
         "the shadow visibility counters are not being written";
    EXPECT_EQ(harness.feedback.visibility.shadow_casters_drawn, harness.feedback.visibility.shadow_casters_total)
      << "with frustum culling disabled, every shadow caster must be drawn regardless of the cascade frusta";

    renderer_vars.frustum_culling_enabled = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_LE(harness.feedback.visibility.shadow_casters_drawn, harness.feedback.visibility.shadow_casters_total);
}

// A shadow that darkens but never moves passes ShadowsDarkenSomePixels and fails here.
TEST(GoldenRender, ShadowsMoveWhenTheLightRotates)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // -51 degrees about Y keeps both shadows on the ground; the band mostly slides in depth, so the masks overlap.
    const glm::vec3 direction_a(scene_vars.directional_light_direction[0],
      scene_vars.directional_light_direction[1],
      scene_vars.directional_light_direction[2]);
    const float rotation_radians = glm::radians(-51.0F);
    const glm::vec3 direction_b(
      glm::rotate(glm::mat4(1.0F), rotation_radians, glm::vec3(0.0F, 1.0F, 0.0F)) * glm::vec4(direction_a, 0.0F));

    const auto set_direction = [&scene_vars](const glm::vec3 &direction) {
        scene_vars.directional_light_direction[0] = direction.x;
        scene_vars.directional_light_direction[1] = direction.y;
        scene_vars.directional_light_direction[2] = direction.z;
    };

    uint32_t width = 0;
    uint32_t height = 0;

    // Direction A: unshadowed, a same-state noise reference, then shadowed.
    set_direction(direction_a);
    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> a_unshadowed = harness.capture_frame(width, height);
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> a_noise_reference = harness.capture_frame(width, height);
    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> a_shadowed = harness.capture_frame(width, height);

    // Direction B: same three-capture pattern.
    set_direction(direction_b);
    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> b_unshadowed = harness.capture_frame(width, height);
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> b_noise_reference = harness.capture_frame(width, height);
    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> b_shadowed = harness.capture_frame(width, height);

    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_FALSE(a_unshadowed.empty()) << "Capture under direction A returned no pixels.";
    ASSERT_EQ(a_unshadowed.size(), a_shadowed.size());
    ASSERT_EQ(a_unshadowed.size(), b_unshadowed.size());
    ASSERT_EQ(a_unshadowed.size(), b_shadowed.size());

    constexpr double CHANGE_THRESHOLD = 4.0;
    const size_t pixel_count = a_unshadowed.size() / 4U;

    // ShadowsDarkenSomePixels' mask, for one light direction.
    const auto build_mask = [pixel_count](const std::vector<uint8_t> &unshadowed,
                              const std::vector<uint8_t> &noise_reference,
                              const std::vector<uint8_t> &shadowed) {
        std::vector<bool> mask(pixel_count, false);
        size_t darkened = 0;
        for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
            if (std::abs(luminance_of(unshadowed, pixel) - luminance_of(noise_reference, pixel)) > CHANGE_THRESHOLD) {
                continue;
            }
            if (luminance_of(unshadowed, pixel) - luminance_of(shadowed, pixel) > CHANGE_THRESHOLD) {
                mask[pixel] = true;
                ++darkened;
            }
        }
        return std::make_pair(mask, darkened);
    };

    const auto [mask_a, darkened_a] = build_mask(a_unshadowed, a_noise_reference, a_shadowed);
    const auto [mask_b, darkened_b] = build_mask(b_unshadowed, b_noise_reference, b_shadowed);

    const double area_a = static_cast<double>(darkened_a) / static_cast<double>(pixel_count);
    const double area_b = static_cast<double>(darkened_b) / static_cast<double>(pixel_count);

    // Two empty masks would otherwise pass by trivially agreeing.
    constexpr double AREA_FLOOR = 0.04;
    EXPECT_GT(area_a, AREA_FLOOR) << "Direction A cast a shadow over only " << (area_a * 100.0)
                                  << "% of pixels; expected a visible shadowed region.";
    EXPECT_GT(area_b, AREA_FLOOR) << "Direction B cast a shadow over only " << (area_b * 100.0)
                                  << "% of pixels; expected a visible shadowed region.";

    size_t intersection = 0;
    size_t mask_union = 0;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        if (mask_a[pixel] || mask_b[pixel]) {
            ++mask_union;
            if (mask_a[pixel] && mask_b[pixel]) { ++intersection; }
        }
    }
    const size_t symmetric_difference = mask_union - intersection;
    const double moved_fraction =
      mask_union == 0U ? 0.0 : static_cast<double>(symmetric_difference) / static_cast<double>(mask_union);

    GTEST_LOG_(INFO) << "direction A shadowed " << darkened_a << " px (" << (area_a * 100.0)
                     << "%), direction B shadowed " << darkened_b << " px (" << (area_b * 100.0) << "%); mask union "
                     << mask_union << ", symmetric difference " << symmetric_difference << " ("
                     << (moved_fraction * 100.0) << "% of union)";

    // 2026-10-01: RTX 2080 0.370, llvmpipe 0.339; 0.011 with the shadow pass pinned to direction A.
    constexpr double MOVED_FRACTION_THRESHOLD = 0.15;
    EXPECT_GT(moved_fraction, MOVED_FRACTION_THRESHOLD)
      << "Only " << (moved_fraction * 100.0)
      << "% of the shadowed-pixel union differs between the two light directions; the shadow does not appear to "
         "move.";
}

// What one shadow-rig placement is expected to show; the cascade is the one its shadow sits in, -1 for any.
struct ShadowRigCase
{
    const char *name;
    glm::vec3 camera_position;
    glm::vec3 light_travel;
    glm::vec3 model_position;
    float model_yaw_degrees;
    int expected_cascade;
};

// Bounds of the box's corners under `transform`, exact for the quarter-turn yaws the cases use.
WorldBox transformed_box(const WorldBox &box, const glm::mat4 &transform)
{
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 local((corner & 1) != 0 ? box.hi.x : box.lo.x,
          (corner & 2) != 0 ? box.hi.y : box.lo.y,
          (corner & 4) != 0 ? box.hi.z : box.lo.z);
        const glm::vec3 world(transform * glm::vec4(local, 1.0F));
        lo = glm::min(lo, world);
        hi = glm::max(hi, world);
    }
    return WorldBox{ lo, hi };
}

// Pixels the shadow darkens by more than 4 levels, minus any that move between two identical unshadowed frames.
std::vector<bool> measured_shadow_mask(EngineHarness &harness, uint32_t &width, uint32_t &height)
{
    auto &scene_vars = harness.settings.scene;
    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> unshadowed = harness.capture_frame(width, height);
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> noise_reference = harness.capture_frame(width, height);
    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> shadowed = harness.capture_frame(width, height);
    const size_t pixel_count = static_cast<size_t>(width) * height;
    std::vector<bool> mask(pixel_count, false);
    if (unshadowed.size() != pixel_count * 4U || shadowed.size() != unshadowed.size()
        || noise_reference.size() != unshadowed.size()) {
        return mask;
    }
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        if (std::abs(luminance_of(unshadowed, pixel) - luminance_of(noise_reference, pixel)) > 4.0) { continue; }
        mask[pixel] = luminance_of(unshadowed, pixel) - luminance_of(shadowed, pixel) > 4.0;
    }
    return mask;
}

// The ShadowsDarkenSomePixels ratio says a shadow exists; this says it is the caster's, where geometry puts it.
TEST(GoldenRender, ShadowFootprintMatchesTheAnalyticCaster)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // The settings hold the way towards the Sun; the cases name the way its light travels.
    const glm::vec3 default_light = -glm::vec3(scene_vars.directional_light_direction[0],
      scene_vars.directional_light_direction[1],
      scene_vars.directional_light_direction[2]);
    // Opposite light tilts put the shadow on opposite sides of the slab; the camera moves pick each cascade.
    const std::array<ShadowRigCase, 5> cases{ {
      { "default light", glm::vec3(0.0F, 6.0F, 26.0F), default_light, glm::vec3(0.0F), 0.0F, 1 },
      { "light toward the camera",
        glm::vec3(0.0F, 6.0F, 26.0F),
        glm::vec3(0.55F, -1.0F, 0.6F),
        glm::vec3(0.0F),
        0.0F,
        1 },
      { "near camera", glm::vec3(0.0F, 6.0F, 10.0F), default_light, glm::vec3(0.0F), 0.0F, 0 },
      { "far camera", glm::vec3(0.0F, 6.0F, 44.0F), default_light, glm::vec3(0.0F), 0.0F, 2 },
      { "model moved and turned",
        glm::vec3(0.0F, 6.0F, 26.0F),
        default_light,
        glm::vec3(4.0F, 0.0F, -3.0F),
        90.0F,
        -1 },
    } };

    std::vector<ShadowAgreement> results;
    for (const ShadowRigCase &rig_case : cases) {
        SCOPED_TRACE(rig_case.name);
        harness.camera->set_camera_position(rig_case.camera_position);
        for (int axis = 0; axis < 3; ++axis) {
            scene_vars.directional_light_direction[axis] = -rig_case.light_travel[axis];
            scene_vars.model_position[axis] = rig_case.model_position[axis];
            scene_vars.model_rotation[axis] = axis == 1 ? rig_case.model_yaw_degrees : 0.0F;
        }
        scene_vars.selected_model_index = 0;
        scene_vars.model_transform_changed = true;
        harness.render_frame();
        scene_vars.model_transform_changed = false;

        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<bool> measured = measured_shadow_mask(harness, width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
        ASSERT_EQ(measured.size(), static_cast<size_t>(width) * height);

        const glm::mat4 model = Kataglyphis::makeGuiModelTransform(
          std::span<const float, 3>(scene_vars.model_position), std::span<const float, 3>(scene_vars.model_rotation));
        const WorldBox slab = transformed_box(SHADOW_RIG_SLAB, model);
        const WorldBox ground_box =
          transformed_box(WorldBox{ glm::vec3(SHADOW_RIG_GROUND.x0, 0.0F, SHADOW_RIG_GROUND.z0),
                            glm::vec3(SHADOW_RIG_GROUND.x1, 0.0F, SHADOW_RIG_GROUND.z1) },
            model);
        const GroundRect ground{ ground_box.lo.y, ground_box.lo.x, ground_box.hi.x, ground_box.lo.z, ground_box.hi.z };
        const GroundShadowPrediction prediction = predict_ground_shadow(
          harness_inverse_view_projection(harness, width, height), width, height, ground, slab, rig_case.light_travel);

        // The case is only about its cascade if its predicted shadow sits there (CascadedShadowMap's splits).
        const std::vector<Kataglyphis::CascadeData> cascades =
          Kataglyphis::computeCascadeData(static_cast<uint32_t>(scene_vars.num_shadow_cascades),
            Kataglyphis::CascadeFitParams{ .cameraView = harness.camera->calculate_viewmatrix(),
              .cameraFov = harness.camera->get_fov(),
              .aspect = static_cast<float>(width) / static_cast<float>(height),
              .nearPlane = harness.camera->get_near_plane(),
              .farPlane = harness.camera->get_far_plane(),
              .lightDir = glm::normalize(rig_case.light_travel),
              .shadowDistance = scene_vars.shadow_distance,
              .splitLambda = scene_vars.cascade_split_lambda });
        std::vector<size_t> per_cascade(cascades.size() + 1U, 0U);
        for (size_t pixel = 0; pixel < prediction.pixels.size(); ++pixel) {
            if (prediction.pixels[pixel] != GroundPixel::Shadowed) { continue; }
            const float distance = glm::dot(
              prediction.world[pixel] - harness.camera->get_camera_position(), harness.camera->get_camera_direction());
            size_t index = 0;
            while (index < cascades.size() && distance >= cascades[index].splitDepth) { ++index; }
            ++per_cascade[index];
        }

        // PCF and the map's texels blur a few pixels either side of the analytic edge.
        constexpr uint32_t EDGE_BAND_PX = 6U;
        const ShadowAgreement agreement =
          compare_shadow_masks(prediction.pixels, measured, width, height, EDGE_BAND_PX);
        results.push_back(agreement);

        std::ostringstream cascade_log;
        for (size_t i = 0; i < per_cascade.size(); ++i) { cascade_log << " c" << i << "=" << per_cascade[i]; }
        GTEST_LOG_(INFO) << rig_case.name << ": compared " << agreement.compared << " px, agreement "
                         << agreement.agreement() << ", shadowed predicted " << agreement.predicted_shadowed
                         << " measured " << agreement.measured_shadowed << ", centroid predicted ("
                         << agreement.predicted_centroid.x << ", " << agreement.predicted_centroid.y << ") measured ("
                         << agreement.measured_centroid.x << ", " << agreement.measured_centroid.y
                         << "); predicted shadow per cascade:" << cascade_log.str();

        const size_t predicted_total = std::accumulate(per_cascade.begin(), per_cascade.end(), static_cast<size_t>(0));
        ASSERT_GT(predicted_total, 0U) << "The prediction puts no shadow in view; the case measures nothing.";
        if (rig_case.expected_cascade >= 0) {
            EXPECT_GT(static_cast<double>(per_cascade[static_cast<size_t>(rig_case.expected_cascade)]),
              0.6 * static_cast<double>(predicted_total))
              << "The case's shadow is not mostly in cascade " << rig_case.expected_cascade << "; move its camera.";
        }
        ASSERT_GT(agreement.predicted_shadowed, 2000U) << "Too little predicted shadow away from its edges.";
        EXPECT_GT(agreement.agreement(), 0.97) << "The rendered shadow is not where the caster and the light put it.";
        EXPECT_LT(glm::distance(agreement.predicted_centroid, agreement.measured_centroid), 8.0)
          << "The rendered shadow's centroid is off the predicted one.";
    }

    // Opposite tilts must move the shadow the way the geometry does, not merely somewhere.
    const glm::dvec2 predicted_shift = results[1].predicted_centroid - results[0].predicted_centroid;
    const glm::dvec2 measured_shift = results[1].measured_centroid - results[0].measured_centroid;
    EXPECT_GT(glm::length(predicted_shift), 20.0) << "The two tilts barely move the predicted shadow.";
    EXPECT_GT(glm::dot(predicted_shift, measured_shift), 0.9 * glm::dot(predicted_shift, predicted_shift))
      << "The shadow moved (" << measured_shift.x << ", " << measured_shift.y << ") px, the geometry says ("
      << predicted_shift.x << ", " << predicted_shift.y << ").";
}

// The receiver-plane kernel's point: a lit receiver far from any shadow stays lit at the widest PCF radius.
TEST(GoldenRender, WidestPcfLeavesLitGroundLit)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;
    scene_vars.pcf_radius = MAX_PCF_RADIUS;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;
    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> unshadowed = harness.capture_frame(width, height);
    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> shadowed = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_EQ(unshadowed.size(), static_cast<size_t>(width) * height * 4U);
    ASSERT_EQ(shadowed.size(), unshadowed.size());

    // The settings hold the way towards the Sun; the prediction takes the way its light travels.
    const glm::vec3 light = -glm::vec3(scene_vars.directional_light_direction[0],
      scene_vars.directional_light_direction[1],
      scene_vars.directional_light_direction[2]);
    const GroundShadowPrediction prediction =
      predict_ground_shadow(harness_inverse_view_projection(harness, width, height),
        width,
        height,
        SHADOW_RIG_GROUND,
        SHADOW_RIG_SLAB,
        light);

    // Lit ground 16 px or more from any predicted shadow or non-ground pixel, past the widest penumbra.
    constexpr uint32_t FAR_PX = 16U;
    std::vector<bool> darkened(unshadowed.size() / 4U, false);
    for (size_t pixel = 0; pixel < darkened.size(); ++pixel) {
        darkened[pixel] = luminance_of(unshadowed, pixel) - luminance_of(shadowed, pixel) >= 3.0;
    }
    std::vector<GroundPixel> lit_only = prediction.pixels;
    for (GroundPixel &kind : lit_only) {
        if (kind == GroundPixel::Shadowed) { kind = GroundPixel::NotGround; }
    }
    const ShadowAgreement far_lit = compare_shadow_masks(lit_only, darkened, width, height, FAR_PX);
    const double darkened_fraction =
      far_lit.compared > 0U ? static_cast<double>(far_lit.measured_shadowed) / static_cast<double>(far_lit.compared)
                            : 1.0;
    GTEST_LOG_(INFO) << "PCF radius " << MAX_PCF_RADIUS << ": " << far_lit.measured_shadowed << " of "
                     << far_lit.compared << " far-lit ground pixels darkened by 3+ levels ("
                     << darkened_fraction * 100.0 << " %)";

    ASSERT_GT(far_lit.compared, 20000U) << "Too little lit ground in view; the test measures nothing.";
    // Measured 0 % with the receiver plane; the kernel before it darkened 62 % here (gpu-golden-testing.md).
    EXPECT_LT(darkened_fraction, 0.01) << "PCF darkens lit ground far from any shadow: shadow acne from the kernel.";
}

// Forward and deferred never match exactly, but a broken path diverges far beyond this.
TEST(GoldenRender, DeferredMatchesForwardRoughly)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = false;
    scene_vars.shadows_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;

    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> forward = harness.capture_frame(width, height);
    ASSERT_FALSE(forward.empty()) << "Forward capture returned no pixels.";

    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> deferred = harness.capture_frame(width, height);
    ASSERT_FALSE(deferred.empty()) << "Deferred capture returned no pixels.";

    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_EQ(forward.size(), deferred.size());

    const double mean_forward = mean_luminance(forward);
    const double mean_deferred = mean_luminance(deferred);

    // Both paths must produce a non-degenerate image in the first place.
    EXPECT_GT(luminance_stddev(forward), 2.0) << "Forward frame is uniform.";
    EXPECT_GT(luminance_stddev(deferred), 2.0) << "Deferred frame is uniform.";

    // Generous on purpose: catches a broken path, not slightly different shading.
    constexpr double LUMINANCE_TOLERANCE = 60.0;
    EXPECT_NEAR(mean_deferred, mean_forward, LUMINANCE_TOLERANCE)
      << "Forward and deferred disagree far more than expected (forward " << mean_forward << ", deferred "
      << mean_deferred << "); one of the two lighting paths is likely broken.";

    // Mean luminance cannot tell forward-vs-forward from parity; absolute channel means expose a probe shader.
    const auto channel_means = [](const std::vector<uint8_t> &px, const char *label) {
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        const auto n = static_cast<double>(px.size() / 4);
        for (size_t i = 0; i < px.size(); i += 4) {
            r += px[i];
            g += px[i + 1];
            b += px[i + 2];
        }
        GTEST_LOG_(INFO) << label << " channel means R " << r / n << " G " << g / n << " B " << b / n;
    };
    channel_means(forward, "forward");
    channel_means(deferred, "deferred");

    // Per-pixel difference separates "shade alike" from "means happen to agree"; the limit is measured.
    double abs_diff_sum = 0.0;
    for (size_t i = 0; i < forward.size(); ++i) {
        abs_diff_sum += std::abs(static_cast<int>(forward[i]) - static_cast<int>(deferred[i]));
    }
    const double mean_abs_diff = abs_diff_sum / static_cast<double>(forward.size());
    GTEST_LOG_(INFO) << "deferred-vs-forward mean abs channel diff: " << mean_abs_diff;
    constexpr double MEAN_ABS_DIFF_LIMIT = 1.0;
    EXPECT_LT(mean_abs_diff, MEAN_ABS_DIFF_LIMIT)
      << "Deferred diverges from forward per-pixel; the paths no longer shade alike.";
}

// The pixels shadows darken by more than 15 luma levels; acne alone stays under that.
static std::vector<bool> shadow_mask(const std::vector<uint8_t> &unshadowed, const std::vector<uint8_t> &shadowed)
{
    std::vector<bool> mask(unshadowed.size() / 4U);
    for (size_t pixel = 0; pixel < mask.size(); ++pixel) {
        mask[pixel] = luminance_of(unshadowed, pixel) - luminance_of(shadowed, pixel) > 15.0;
    }
    return mask;
}

static double mask_iou(const std::vector<bool> &a, const std::vector<bool> &b, uint32_t w, uint32_t h, bool flip_b)
{
    size_t both = 0;
    size_t either = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const bool in_a = a[static_cast<size_t>(y) * w + x];
            const bool in_b = b[static_cast<size_t>(flip_b ? h - 1U - y : y) * w + x];
            both += (in_a && in_b) ? 1U : 0U;
            either += (in_a || in_b) ? 1U : 0U;
        }
    }
    return either > 0U ? static_cast<double>(both) / static_cast<double>(either) : 0.0;
}

// Deferred rebuilds world positions from the fullscreen uv; a mirrored rebuild puts its shadows in the other half.
TEST(GoldenRender, DeferredShadowsLandWhereForwardShadowsLand)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    auto &renderer_vars = harness.useForwardRaster();
    scene_vars.shadows_enabled = true;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;
    const float default_intensity = scene_vars.cascaded_shadow_intensity;
    const auto mask_for = [&](RasterizationMode mode) {
        renderer_vars.rasterizationMode = mode;
        scene_vars.cascaded_shadow_intensity = 0.0F;
        harness.render_frames(SETTLE_FRAMES);
        const std::vector<uint8_t> unshadowed = harness.capture_frame(width, height);
        scene_vars.cascaded_shadow_intensity = default_intensity;
        harness.render_frames(SETTLE_FRAMES);
        const std::vector<uint8_t> shadowed = harness.capture_frame(width, height);
        return shadow_mask(unshadowed, shadowed);
    };
    const std::vector<bool> forward = mask_for(RasterizationMode::Forward);
    const std::vector<bool> deferred = mask_for(RasterizationMode::Deferred);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(forward.size(), static_cast<size_t>(width) * height);
    ASSERT_EQ(deferred.size(), forward.size());

    const double forward_coverage =
      static_cast<double>(std::count(forward.begin(), forward.end(), true)) / static_cast<double>(forward.size());
    const double iou = mask_iou(deferred, forward, width, height, false);
    const double iou_flipped = mask_iou(deferred, forward, width, height, true);
    GTEST_LOG_(INFO) << "forward shadow coverage " << forward_coverage << ", deferred-vs-forward IoU " << iou
                     << ", against the flipped forward mask " << iou_flipped;

    ASSERT_GT(forward_coverage, 0.01) << "forward casts no measurable shadow - the comparison would be vacuous";
    ASSERT_LT(forward_coverage, 0.5) << "forward darkens most of the frame - not a shadow";
    EXPECT_GT(iou, 0.5) << "deferred shadows do not land where forward's do";
    EXPECT_GT(iou, iou_flipped + 0.25) << "deferred shadows match forward's vertical mirror at least as well";
}

// The light must scale diffuse as well as specular, or only sparse highlights respond to radiance.

namespace {

// Which renderer a punctual-light golden draws with.
enum class PunctualMode { Forward, Deferred, Raytracing, PathTracing };

struct PunctualPools
{
    size_t red = 0;
    size_t green = 0;
    bool skipped = false;
};

// What a punctual render recorded besides its pixels.
struct PunctualRenderInfo
{
    std::vector<std::string> stages;
    uint32_t width = 0;
};

// Renders `model` lit by its lights alone (the sun and its sky at zero), framed on the plane, with `mode`'s renderer.
std::vector<uint8_t> render_punctual_scene(const std::string &model,
  PunctualMode mode,
  bool &skipped,
  bool lightShadows = true,
  PunctualRenderInfo *info = nullptr)
{
    ScopedModelOverride scene(model.c_str());
    EngineHarness harness;
    if (!frameCaptureAvailable(harness)) {
        skipped = true;
        return {};
    }
    const bool traced = mode == PunctualMode::Raytracing || mode == PunctualMode::PathTracing;
    if (traced && !harness.renderer->supportsHardwareRaytracing()) {
        skipped = true;
        return {};
    }
    harness.camera->set_camera_position(glm::vec3(0.0F, 6.0F, 7.0F));
    harness.camera->set_orientation(-90.0F, -40.0F);
    harness.settings.scene.directional_light_radiance = 0.0F;
    harness.settings.scene.directional_light_ambient = 0.0F;
    harness.settings.scene.punctual_shadows_enabled = lightShadows;
    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.rasterizationMode =
      mode == PunctualMode::Deferred ? RasterizationMode::Deferred : RasterizationMode::Forward;
    renderer_vars.raytracing = mode == PunctualMode::Raytracing;
    renderer_vars.pathTracing = mode == PunctualMode::PathTracing;
    // Two short paths a pixel: the pools are thousands of pixels, so their count survives the noise.
    renderer_vars.pathTracingSamplesPerPixel = 2;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES + (mode == PunctualMode::PathTracing ? 12 : SETTLE_FRAMES));
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> frame = harness.capture_frame(width, height);
    EXPECT_FALSE(harness.renderer->hasDeviceLost());
    if (info != nullptr) { *info = { harness.renderer->renderStageRegistry().lastRecorded(), width }; }
    return frame;
}

// Pixels the lights turn clearly red or green against the same scene with every light at zero intensity.
PunctualPools punctual_pools(PunctualMode mode)
{
    PunctualPools pools;
    const auto dark =
      write_gltf_variant(punctual_lights_model().c_str(), "kataglyphis_punctual_dark.gltf", [](nlohmann::json &gltf) {
          for (auto &light : gltf["extensions"]["KHR_lights_punctual"]["lights"]) { light["intensity"] = 0.0; }
      });
    EXPECT_TRUE(dark.has_value()) << "could not write the dark twin of " << punctual_lights_model();
    if (!dark.has_value()) { return pools; }

    const std::vector<uint8_t> lit = render_punctual_scene(punctual_lights_model(), mode, pools.skipped);
    if (pools.skipped) { return pools; }
    const std::vector<uint8_t> unlit = render_punctual_scene(dark->string(), mode, pools.skipped);
    EXPECT_EQ(lit.size(), unlit.size());
    if (lit.empty() || lit.size() != unlit.size()) { return pools; }

    for (size_t i = 0; i + 3 < lit.size(); i += 4) {
        const int dr = int(lit[i]) - int(unlit[i]);
        const int dg = int(lit[i + 1]) - int(unlit[i + 1]);
        const int db = int(lit[i + 2]) - int(unlit[i + 2]);
        if (dr > 60 && dr > dg + 40 && dr > db + 40) { ++pools.red; }
        if (dg > 60 && dg > dr + 40 && dg > db + 40) { ++pools.green; }
    }
    GTEST_LOG_(INFO) << "punctual pools: " << pools.red << " red, " << pools.green << " green pixels";
    return pools;
}

void expect_punctual_pools(PunctualMode mode)
{
    const PunctualPools pools = punctual_pools(mode);
    if (pools.skipped) { GTEST_SKIP() << "no frame capture, or no hardware ray tracing for a traced mode"; }
    // OxidANT's punctual_lights_pool_on_plane asks 200 and 100 of a smaller frame.
    EXPECT_GT(pools.red, 200U) << "the red point light lit no pool on the plane";
    EXPECT_GT(pools.green, 100U) << "the green spot light lit no pool on the plane";
}

}// namespace

TEST(GoldenRender, PunctualLightsPoolOnThePlaneInForward)
{
    SKIP_WITHOUT_GPU();
    expect_punctual_pools(PunctualMode::Forward);
}

TEST(GoldenRender, PunctualLightsPoolOnThePlaneInDeferred)
{
    SKIP_WITHOUT_GPU();
    expect_punctual_pools(PunctualMode::Deferred);
}

TEST(GoldenRender, PunctualLightsPoolOnThePlaneWhenRaytraced)
{
    SKIP_WITHOUT_GPU();
    expect_punctual_pools(PunctualMode::Raytracing);
}

TEST(GoldenRender, PunctualLightsPoolOnThePlaneWhenPathTraced)
{
    SKIP_WITHOUT_GPU();
    expect_punctual_pools(PunctualMode::PathTracing);
}

namespace {

// The Sun directional_light.gltf names: towards it, the opposite of its node's -Z (0.6, -0.48, 0.64), and its colour.
constexpr std::array<float, 3> kFixtureSunDirection{ -0.6F, 0.48F, -0.64F };
constexpr std::array<float, 3> kFixtureSunColor{ 1.0F, 0.7F, 0.4F };

struct SunScene
{
    std::vector<uint8_t> frame;
    GUISceneSharedVars after;
    bool skipped = false;
};

// A forward frame of `model` framed on its plane, and the editor's scene settings once it drew.
SunScene render_sun_scene(const std::string &model, bool setTheFixtureSunByHand)
{
    SunScene result;
    ScopedModelOverride scene(model.c_str());
    EngineHarness harness;
    if (!frameCaptureAvailable(harness)) {
        result.skipped = true;
        return result;
    }
    harness.camera->set_camera_position(glm::vec3(0.0F, 6.0F, 7.0F));
    harness.camera->set_orientation(-90.0F, -40.0F);
    if (setTheFixtureSunByHand) {
        auto &vars = harness.settings.scene;
        std::copy(
          kFixtureSunDirection.begin(), kFixtureSunDirection.end(), std::begin(vars.directional_light_direction));
        std::copy(kFixtureSunColor.begin(), kFixtureSunColor.end(), std::begin(vars.directional_light_color));
    }
    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES);
    uint32_t width = 0;
    uint32_t height = 0;
    result.frame = harness.capture_frame(width, height);
    EXPECT_FALSE(harness.renderer->hasDeviceLost());
    result.after = harness.settings.scene;
    return result;
}

}// namespace

// Shaded as a punctual light, the Colosseum's 4098 lx sun added an unattenuated second sun that drew the frame white.
TEST(GoldenRender, AGltfDirectionalLightBecomesTheSunInsteadOfAPunctualLight)
{
    SKIP_WITHOUT_GPU();
    const auto sunless = write_gltf_variant(DIRECTIONAL_LIGHT_MODEL,
      "kataglyphis_sunless.gltf",
      [](nlohmann::json &gltf) { gltf["nodes"][1].erase("extensions"); });
    ASSERT_TRUE(sunless.has_value()) << "could not write the sunless twin of " << DIRECTIONAL_LIGHT_MODEL;

    const SunScene adopted = render_sun_scene(DIRECTIONAL_LIGHT_MODEL, false);
    if (adopted.skipped) { GTEST_SKIP() << "frame capture is unsupported here"; }
    for (std::size_t axis = 0; axis < 3U; ++axis) {
        EXPECT_NEAR(adopted.after.directional_light_direction[axis], kFixtureSunDirection[axis], 1e-4F)
          << "the editor's Sun did not take the file's direction, axis " << axis;
        EXPECT_NEAR(adopted.after.directional_light_color[axis], kFixtureSunColor[axis], 1e-5F)
          << "the editor's Sun did not take the file's colour, channel " << axis;
    }
    EXPECT_FLOAT_EQ(adopted.after.directional_light_radiance, GUISceneSharedVars{}.directional_light_radiance)
      << "the file's lux replaced the editor's radiance";

    // The twin draws no light of its own; the editor's Sun, set to the file's by hand, must light it alike.
    const SunScene reference = render_sun_scene(sunless->string(), true);
    ASSERT_FALSE(adopted.frame.empty());
    ASSERT_EQ(adopted.frame.size(), reference.frame.size());
    const size_t pixels = adopted.frame.size() / 4U;
    size_t differing = 0;
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        if (channel_delta(adopted.frame, reference.frame, pixel) > 8) { ++differing; }
    }
    GTEST_LOG_(INFO) << "file Sun against the hand-set one: " << differing << " of " << pixels
                     << " pixels differ by more than 8";
    EXPECT_LT(differing, pixels / 1000U)
      << "the file's directional light still reaches the frame as a light of its own, or its Sun was not adopted";
}

namespace {

struct PunctualShadowCounts
{
    // Pixels of the red pool, and of them how many the card's shadow darkens.
    size_t redPool = 0;
    size_t darkened = 0;
    size_t darkenedLeftOfCentre = 0;
    // Pixels whose green the cube shadows dim: the spot light's pool has no occluder, so acne would show here.
    size_t greenDimmed = 0;
    size_t greenPool = 0;
    bool skipped = false;
};

// point_light.gltf with a 0.8-unit card halfway between the red point light and the plane.
PunctualShadowCounts punctual_shadow_counts(PunctualMode mode)
{
    PunctualShadowCounts counts;
    const auto carded =
      write_gltf_variant(punctual_lights_model().c_str(), "kataglyphis_punctual_card.gltf", [](nlohmann::json &gltf) {
          gltf["scenes"][0]["nodes"].push_back(gltf["nodes"].size());
          gltf["nodes"].push_back(
            { { "mesh", 0 }, { "translation", { -1.2, 0.5, 0.0 } }, { "scale", { 0.1, 1.0, 0.1 } } });
      });
    EXPECT_TRUE(carded.has_value()) << "could not write the carded twin of " << punctual_lights_model();
    if (!carded.has_value()) { return counts; }

    PunctualRenderInfo with_shadows;
    PunctualRenderInfo without_shadows;
    const std::vector<uint8_t> on = render_punctual_scene(carded->string(), mode, counts.skipped, true, &with_shadows);
    if (counts.skipped) { return counts; }
    const std::vector<uint8_t> off =
      render_punctual_scene(carded->string(), mode, counts.skipped, false, &without_shadows);
    const auto recorded = [](const PunctualRenderInfo &info) {
        return std::find(info.stages.begin(), info.stages.end(), "punctual_shadows") != info.stages.end();
    };
    EXPECT_TRUE(recorded(with_shadows)) << "the punctual_shadows stage did not record with two shadowed lights";
    EXPECT_FALSE(recorded(without_shadows)) << "the punctual_shadows stage recorded with the light shadows off";
    EXPECT_EQ(on.size(), off.size());
    if (on.empty() || on.size() != off.size() || with_shadows.width == 0) { return counts; }

    const size_t width = with_shadows.width;
    for (size_t i = 0; i + 3 < off.size(); i += 4) {
        const int r = off[i];
        const int g = off[i + 1];
        const int b = off[i + 2];
        if (r > 60 && r > g + 40 && r > b + 40) { ++counts.redPool; }
        if (r - int(on[i]) > 40) {
            ++counts.darkened;
            if ((i / 4) % width < width / 2) { ++counts.darkenedLeftOfCentre; }
        }
        // Only inside the green pool: the red light's own green is dimmed by the card's shadow too.
        if (g > 60 && g > r + 40 && g > b + 40) {
            ++counts.greenPool;
            if (g - int(on[i + 1]) > 20) { ++counts.greenDimmed; }
        }
    }
    GTEST_LOG_(INFO) << "punctual shadow: " << counts.darkened << " of " << counts.redPool << " red px darkened ("
                     << counts.darkenedLeftOfCentre << " left of centre), " << counts.greenDimmed << " of "
                     << counts.greenPool << " green px dimmed";
    return counts;
}

void expect_punctual_shadow(PunctualMode mode)
{
    const PunctualShadowCounts counts = punctual_shadow_counts(mode);
    if (counts.skipped) { GTEST_SKIP() << "no frame capture"; }
    EXPECT_GT(counts.darkened, 400U) << "the card casts no shadow from the point light onto the plane";
    EXPECT_LT(counts.darkened * 2, counts.redPool) << "the shadow darkens most of the red pool: the cube leaks shadow";
    EXPECT_GT(counts.darkenedLeftOfCentre * 10, counts.darkened * 9)
      << "the shadow is not under the card, which sits left of the camera";
    EXPECT_GT(counts.greenPool, 100U) << "the green spot light lit no pool";
    EXPECT_LT(counts.greenDimmed * 50, counts.greenPool) << "the unoccluded spot light's pool shadows itself (acne)";
}

}// namespace

// A card between the red point light and the plane must shadow the plane, and the open spot light must not.
TEST(GoldenRender, PunctualLightShadowsTheCardOntoThePlaneInForward)
{
    SKIP_WITHOUT_GPU();
    expect_punctual_shadow(PunctualMode::Forward);
}

TEST(GoldenRender, PunctualLightShadowsTheCardOntoThePlaneInDeferred)
{
    SKIP_WITHOUT_GPU();
    expect_punctual_shadow(PunctualMode::Deferred);
}

TEST(GoldenRender, ForwardLightingRespondsToTheDirectionalLight)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    harness.useForwardRaster();
    // The sun alone: the sky, which TheSkyLightsShadowedGround* checks, lit the radiance-0 crop to a mean of 70.5.
    scene_vars.directional_light_ambient = 0.0F;
    // The switched-off sky's navy marks the sky pixels, which the crop's means leave out: no light reaches them.
    scene_vars.skybox_enabled = false;
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> lit = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(lit.empty());

    scene_vars.directional_light_radiance = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> unlit = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(lit.size(), unlit.size());

    const double response = swung_fraction(lit, unlit, width, height, panel_free_crop(width, height));

    // Specular swings many pixels, so an unlit geometry mean catches a light-blind diffuse; the sky alone read 29.8.
    size_t geometry_pixels = 0;
    const auto crop_mean_luminance = [&geometry_pixels](const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h) {
        const uint32_t x0 = (w * 18U) / 25U;
        const uint32_t x1 = (w * 49U) / 50U;
        const uint32_t y0 = h / 20U;
        const uint32_t y1 = (h * 19U) / 20U;
        double sum = 0.0;
        size_t count = 0;
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = x0; x < x1; ++x) {
                const size_t pixel = static_cast<size_t>(y) * w + x;
                if (is_disabled_sky(rgba, pixel)) { continue; }
                sum += luminance_of(rgba, pixel);
                ++count;
            }
        }
        geometry_pixels = count;
        return count > 0U ? sum / static_cast<double>(count) : 0.0;
    };
    const double unlit_luma = crop_mean_luminance(unlit, width, height);
    const double lit_luma = crop_mean_luminance(lit, width, height);
    GTEST_LOG_(INFO) << "forward lit-vs-unlit swung-pixel fraction (panel-free crop): " << response
                     << ", lit crop mean luminance: " << lit_luma << ", unlit crop mean luminance: " << unlit_luma
                     << " over " << geometry_pixels << " geometry px";
    ASSERT_GT(geometry_pixels, 20000U) << "Too little geometry in the crop; the test measures nothing.";

    EXPECT_LT(unlit_luma, 30.0) << "The scene stays lit with the directional light at zero radiance - "
                                   "the light factors are not reaching the diffuse term.";

    // Vacuous alone; a cheap sanity floor.
    EXPECT_GT(response, 0.05) << "Forward lighting did not respond to the directional light radiance.";
}
