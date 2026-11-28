// Goldens: ray- and path-traced frames: convergence, controls, materials, the white furnaces and the traced sky.

#include "ScopedLogCapture.hpp"
#include "renderer/PathTracingDispatch.hpp"

#include "GoldenRenderHarness.hpp"

import kataglyphis.vulkan.renderer;

using namespace Kataglyphis::TestSupport::GoldenRender;

namespace {

using Kataglyphis::Test::GoldenMetrics::sky_silhouette;
using Kataglyphis::Test::GoldenMetrics::SKY_SILHOUETTE_STEP;
using Kataglyphis::Test::GoldenMetrics::SkySilhouette;
using Kataglyphis::Test::GoldenMetrics::kWhiteFurnaceAlbedo;
using Kataglyphis::Test::GoldenMetrics::white_single_scatter_albedo;

// The lit-but-not-glowing control for EMISSIVE_CARD_MODEL.
constexpr const char *METALLIC_CARD_MODEL = "Models/GltfTest/metallic_card.gltf";
// One metallic-roughness texel (0, 255, 0): the roughness factor itself, metallic 0.
constexpr const char *FULL_ROUGHNESS_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNg+M8AAAICAQB7CYF4AAAAAElFTkSuQmCC";
// Largest and mean relative difference of two HDR captures' RGB, against a floor that keeps black from dividing.
struct HdrDifference
{
    double largest = 0.0;
    double mean = 0.0;
};

HdrDifference hdr_difference(const std::vector<float> &a, const std::vector<float> &b)
{
    HdrDifference difference;
    std::size_t channels = 0;
    for (std::size_t i = 0; i + 3U < a.size() && i + 3U < b.size(); i += 4U) {
        for (std::size_t c = 0; c < 3U; ++c) {
            const auto x = static_cast<double>(a[i + c]);
            const auto y = static_cast<double>(b[i + c]);
            const double relative = std::abs(x - y) / std::max({ std::abs(x), std::abs(y), 1.0e-3 });
            difference.largest = std::max(difference.largest, relative);
            difference.mean += relative;
            ++channels;
        }
    }
    difference.mean /= static_cast<double>(std::max<std::size_t>(channels, 1U));
    return difference;
}

// The renderer's "path tracer: N dispatches a frame, ..." lines, in the order it logged them.
std::vector<std::string> dispatch_plan_lines(const Kataglyphis::TestSupport::ScopedLogCapture &capture)
{
    std::vector<std::string> lines;
    for (const auto &record : capture.records()) {
        if (record.text.starts_with("path tracer: ")) { lines.push_back(record.text); }
    }
    return lines;
}

// Four float32 VEC3 normals (sin 70, 0, cos 70): a card facing +Z whose vertex normals lean 70 degrees toward +X.
constexpr const char *LEANING_VERTEX_NORMALS =
  "data:application/octet-stream;base64,so9wPwAAAABEHa8+so9wPwAAAABEHa8+so9wPwAAAABEHa8+so9wPwAAAABEHa8+";

}// namespace

// New samples every frame must make early frames differ and late frames converge (1/N running mean).
TEST(GoldenRender, PathTracingAccumulatesAndConverges)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);

    // Nudge to reset history, or the early pair measures the mean healing from pre-load frames.
    harness.camera->set_camera_position(harness.camera->get_camera_position() + glm::vec3(0.001F, 0.0F, 0.0F));
    harness.render_frame();

    // Changed-pixel fraction in the GUI-free right edge; means and centre crops measured GUI noise.
    const auto changed_fraction =
      [](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, uint32_t w, uint32_t h) {
          const uint32_t x0 = (w * 3U) / 4U;
          const uint32_t x1 = (w * 49U) / 50U;
          const uint32_t y0 = h / 20U;
          const uint32_t y1 = (h * 19U) / 20U;
          size_t changed = 0;
          size_t total = 0;
          for (uint32_t y = y0; y < y1; ++y) {
              for (uint32_t x = x0; x < x1; ++x) {
                  const size_t base = (static_cast<size_t>(y) * w + x) * 4U;
                  for (size_t c = 0; c < 3U; ++c) {
                      if (std::abs(static_cast<int>(a[base + c]) - static_cast<int>(b[base + c])) > 2) {
                          ++changed;
                          break;
                      }
                  }
                  ++total;
              }
          }
          return static_cast<double>(changed) / static_cast<double>(total);
      };

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> early_a = harness.capture_frame(width, height);
    const std::vector<uint8_t> early_b = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(early_a.empty());
    ASSERT_EQ(early_a.size(), early_b.size());

    // Deepen the history, then measure the per-frame movement again.
    harness.render_frames(40);
    const std::vector<uint8_t> late_a = harness.capture_frame(width, height);
    const std::vector<uint8_t> late_b = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(late_a.size(), late_b.size());

    // Logged, not asserted: autocorrelation shows a future seed change's noise character.
    {
        const uint32_t cx0 = (width * 18U) / 25U, cx1 = (width * 49U) / 50U;
        const uint32_t cy0 = height / 4U, cy1 = (height * 3U) / 4U;
        auto corr = [&](uint32_t lag) {
            double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
            size_t n = 0;
            for (uint32_t y = cy0; y < cy1; ++y)
                for (uint32_t x = cx0; x + lag < cx1; ++x) {
                    const size_t pixel = static_cast<size_t>(y) * width + x;
                    const double a = luminance_of(early_a, pixel) - luminance_of(early_b, pixel);
                    const double b = luminance_of(early_a, pixel + lag) - luminance_of(early_b, pixel + lag);
                    sx += a;
                    sy += b;
                    sxx += a * a;
                    syy += b * b;
                    sxy += a * b;
                    ++n;
                }
            const auto count = static_cast<double>(n);
            const double cov = sxy / count - (sx / count) * (sy / count);
            const double va = sxx / count - (sx / count) * (sx / count);
            const double vb = syy / count - (sy / count) * (sy / count);
            return (va > 0 && vb > 0) ? cov / std::sqrt(va * vb) : 0.0;
        };
        GTEST_LOG_(INFO) << "noise autocorrelation lag-1: " << corr(1) << ", lag-16: " << corr(16);
    }

    const double early_delta = changed_fraction(early_a, early_b, width, height);
    const double late_delta = changed_fraction(late_a, late_b, width, height);
    GTEST_LOG_(INFO) << "consecutive-frame changed fraction (right-edge crop): early = " << early_delta
                     << ", late = " << late_delta;

    // (2) Not blank: the accumulated image reaches the screen at all.
    EXPECT_GT(fraction_above(late_b, 8.0), 0.01)
      << "Path-traced frame is (nearly) black - accumulated image not reaching the screen.";

    // (1) The crop is GUI-free, so frozen sampling gives exactly 0; the limit is measured.
    EXPECT_GT(early_delta, 1.0e-4) << "Consecutive path-traced frames are (near) identical in the scene region - "
                                      "per-frame seeding inactive.";

    // (3) Half the early fraction leaves room for driver variance but still demands convergence.
    EXPECT_LT(late_delta, early_delta / 2.0)
      << "Per-frame movement did not shrink with accumulation depth - history not converging.";
}

// Pixel jitter lets accumulation blend straddling pixels; without it every edge pixel is fully fg or bg.
TEST(GoldenRender, PathTracingAntiAliasesGeometricEdges)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    // The edge needs contrast: under the default Sun (radiance 5) the slab's top cleared the step in 61 of 312 columns.
    harness.settings.scene.directional_light_radiance = 10.0F;
    harness.render_frames(WARMUP_FRAMES);

    // History-reset nudge, as in PathTracingAccumulatesAndConverges.
    harness.camera->set_camera_position(harness.camera->get_camera_position() + glm::vec3(0.001F, 0.0F, 0.0F));
    harness.render_frame();

    // Shallow history looks "in between" from noise alone, which would pass vacuously.
    harness.render_frames(path_traced_history(80, 25));

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(frame.empty());

    // Only the slab's top edge against the sky: penumbrae and the ground's 1-px far-edge row sit in between unjittered.
    const Crop crop = panel_free_crop(width, height);
    const SkySilhouette silhouette = sky_silhouette(frame, width, crop);
    const size_t crop_columns = crop.x1 - crop.x0;
    GTEST_LOG_(INFO) << "slab-vs-sky silhouette: rows " << silhouette.first_row << "-" << silhouette.last_row << ", "
                     << silhouette.columns << " of " << crop_columns << " columns, " << silhouette.blended
                     << " with a blended pixel, steps " << silhouette.min_step << "-" << silhouette.max_step
                     << " levels";

    ASSERT_GE(silhouette.columns * 2U, crop_columns)
      << "The slab's top edge against the sky (a step of more than " << SKY_SILHOUETTE_STEP
      << " levels below the sky at the top of the panel-free crop) crosses only " << silhouette.columns << " of "
      << crop_columns << " columns; check the crop, the rig or the golden framing.";

    const double blended_fraction = static_cast<double>(silhouette.blended) / static_cast<double>(silhouette.columns);
    // Measured on x64 lavapipe: about 1.0 jittered, 0.0 through the pixel centre, in both variants.
    EXPECT_GT(blended_fraction, 0.5)
      << "Only " << (blended_fraction * 100.0)
      << "% of the slab's silhouette against the sky has a blended pixel between sky and slab - the primary ray "
         "lands on the exact pixel centre every sample instead of being jittered across the pixel's area.";
}

// Path tracing does NEE toward the GUI light, so zero radiance must darken the GUI-free right edge hard.
TEST(GoldenRender, PathTracingRespondsToTheDirectionalLight)
{
    SKIP_WITHOUT_GPU();

    // The skeleton scene's right-edge crop is nearly all sky, which the light does not touch.
    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);

    // Count swung pixels, not a mean: the lit ground clamps at the UNORM ceiling and the band hugs the panel.
    harness.render_frames(SETTLE_FRAMES);
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> lit = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(lit.empty());

    // History resets on camera/scene changes, not light changes, so wait for the mean to wash out.
    scene_vars.directional_light_radiance = 0.0F;
    harness.render_frames(30);
    const std::vector<uint8_t> unlit = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(lit.size(), unlit.size());

    const double response = swung_fraction(lit, unlit, width, height, panel_free_crop(width, height));
    // Logged: with NEE's 1/pi, PT should match forward's brightness, not sit pi times above it.
    const auto crop_mean_luminance = [](const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h) {
        const uint32_t x0 = (w * 18U) / 25U;
        const uint32_t x1 = (w * 49U) / 50U;
        const uint32_t y0 = h / 20U;
        const uint32_t y1 = (h * 19U) / 20U;
        double sum = 0.0;
        size_t count = 0;
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = x0; x < x1; ++x) {
                sum += luminance_of(rgba, static_cast<size_t>(y) * w + x);
                ++count;
            }
        }
        return sum / static_cast<double>(count);
    };
    GTEST_LOG_(INFO) << "PT lit-vs-unlit swung-pixel fraction (panel-free crop): " << response
                     << ", lit crop mean luminance: " << crop_mean_luminance(lit, width, height);

    // Measured with margin; a light-blind kernel leaves only accumulation drift, well below it.
    EXPECT_GT(response, 5.0e-3) << "Path-traced pixels did not respond to the directional light radiance.";
}

// One bounce means no indirect sky light at all, so a kernel honouring the cap changes large regions.
TEST(GoldenRender, PathTracingHonorsTheQualityControls)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> full_quality = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(full_quality.empty());

    renderer_vars.pathTracingMaxBounces = 1;
    harness.render_frames(10);
    const std::vector<uint8_t> single_bounce = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(full_quality.size(), single_bounce.size());

    const double response = swung_fraction(full_quality, single_bounce, width, height, panel_free_crop(width, height));
    GTEST_LOG_(INFO) << "PT bounces 8-vs-1 swung-pixel fraction (panel-free crop): " << response;

    // Measured: removing all indirect light must change a real share of scene pixels.
    EXPECT_GT(response, 5.0e-3) << "The bounce-cap slider did not change the path-traced image - "
                                   "quality push constants not reaching the kernel.";
}

// metallic_card's 0.75 comes without a metallic-roughness texture; the path tracer once lit it like a dielectric.
TEST(GoldenRender, PathTracingHonorsTheUntexturedMetallicFactor)
{
    SKIP_WITHOUT_GPU();

    const auto dielectric_card = write_gltf_variant(METALLIC_CARD_MODEL,
      "kataglyphis_dielectric_card.gltf",
      [](nlohmann::json &gltf) { gltf["materials"][0]["pbrMetallicRoughness"]["metallicFactor"] = 0.0; });
    ASSERT_TRUE(dielectric_card.has_value()) << "could not write the dielectric twin of " << METALLIC_CARD_MODEL;

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    // Two short paths a pixel: the card means over thousands of pixels average the noise out.
    renderer_vars.pathTracingSamplesPerPixel = 2;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    // Mirror images of one placement, so the sun meets both cards alike.
    const glm::mat4 right = panel_free_card_placement();
    const glm::mat4 left = glm::translate(glm::mat4(1.0F), glm::vec3(-2.0F * right[3].x, 0.0F, 0.0F)) * right;
    ASSERT_TRUE(harness.renderer->addModel(METALLIC_CARD_MODEL, right).has_value())
      << "adding the metallic card failed";
    ASSERT_TRUE(harness.renderer->addModel(dielectric_card->string(), left).has_value())
      << "adding the dielectric card failed";
    harness.render_frames(SETTLE_FRAMES + 16);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    // Linear radiance: the specular lobe gives the metal back part of its light, which ACES would squeeze near white.
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(radiance.empty());

    const Crop metallic_box = panel_free_card_box(width, height);
    const uint32_t centre_y = (metallic_box.y0 + metallic_box.y1) / 2U;
    const uint32_t metallic_x = (metallic_box.x0 + metallic_box.x1) / 2U;
    const double metallic_mean = mean_hdr_in_square(radiance, width, metallic_x, centre_y, 40U);
    const double dielectric_mean = mean_hdr_in_square(radiance, width, width - metallic_x, centre_y, 40U);
    GTEST_LOG_(INFO) << "path-traced card radiance: metallic 0.75 " << metallic_mean << ", dielectric "
                     << dielectric_mean;

    // The gain returns most of the metal's rough lobe (0.82 of the dielectric); an unread metallicFactor gives 1.
    ASSERT_GT(dielectric_mean, 0.05) << "the dielectric card is unlit - check the placement";
    EXPECT_LT(metallic_mean, 0.9 * dielectric_mean)
      << "the untextured metallicFactor does not reach the path tracer: the metal shades like its dielectric twin";
}

// A transform change must rebuild the TLAS; deterministic RT over a stale one is bit-identical.
TEST(GoldenRender, RaytracedWorldFollowsTheModelTransform)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(before.empty());

    // Raise the model the way the GUI does it.
    scene_vars.selected_model_index = 0;
    scene_vars.model_position[1] += 3.0F;
    scene_vars.model_transform_changed = true;
    harness.render_frames(SETTLE_FRAMES);

    const std::vector<uint8_t> after = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(before.size(), after.size());

    const double moved = swung_fraction(before, after, width, height, panel_free_crop(width, height));
    GTEST_LOG_(INFO) << "RT transform-move swung-pixel fraction (panel-free crop): " << moved;

    // Measured; a stale TLAS gives exactly zero.
    EXPECT_GT(moved, 5.0e-3) << "The traced image did not follow the model transform - stale TLAS.";
}

// Seeding the payload with albedo adds an ambient term forward/deferred lack, so shadows never go dark.
TEST(GoldenRender, RaytracedShadowsAreDarkerThanLitGround)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    // The shadow ray alone: the sky's ambient lit the darkest decile to 43.4, as the albedo seed's 41.6 did.
    harness.settings.scene.directional_light_ambient = 0.0F;
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(frame.empty());

    // Darkest vs brightest decile, since a whole-crop mean averages shadow and light together.
    const Crop crop = panel_free_crop(width, height);
    std::vector<double> luminances;
    luminances.reserve(static_cast<size_t>(crop.x1 - crop.x0) * static_cast<size_t>(crop.y1 - crop.y0));
    for (uint32_t y = crop.y0; y < crop.y1; ++y) {
        for (uint32_t x = crop.x0; x < crop.x1; ++x) {
            luminances.push_back(luminance_of(frame, static_cast<size_t>(y) * width + x));
        }
    }
    std::sort(luminances.begin(), luminances.end());
    const size_t decile = std::max<size_t>(1U, luminances.size() / 10U);
    double dark_sum = 0.0;
    for (size_t i = 0; i < decile; ++i) { dark_sum += luminances[i]; }
    double lit_sum = 0.0;
    for (size_t i = luminances.size() - decile; i < luminances.size(); ++i) { lit_sum += luminances[i]; }
    const double dark_mean = dark_sum / static_cast<double>(decile);
    const double lit_mean = lit_sum / static_cast<double>(decile);

    GTEST_LOG_(INFO) << "RT shadow darkest-decile mean luminance: " << dark_mean
                     << ", lit-decile mean luminance: " << lit_mean;

    // Measured: between the fix (exactly black) and an albedo-seeded payload in raytrace.rchit.slang.
    EXPECT_LT(dark_mean, lit_mean * 0.05) << "Ray-traced shadow did not go dark relative to the lit ground - the "
                                             "closest-hit shader is still seeding an ambient/unlit-albedo term.";
}

// RT overwrites the raster image, so pixels prove nothing: the zeroed counters are the oracle, timing is logged.
TEST(GoldenRender, RaytracingFrameSkipsTheRasterPass)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.frustum_culling_enabled = true;

    // A full GpuPassAverage::WINDOW of forward-only samples.
    const int window = static_cast<int>(Kataglyphis::GpuTimingSubsystem::GpuPassAverage::WINDOW);
    harness.render_frames(WARMUP_FRAMES + window);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    const unsigned int forward_meshes_drawn = harness.feedback.visibility.meshes_drawn;
    ASSERT_GT(forward_meshes_drawn, 0U) << "the renderer reported drawing no meshes at all in forward mode";
    const float forward_main_ms =
      harness.feedback.gpuTimings
        .pass_ms[static_cast<size_t>(Kataglyphis::VulkanRendererInternals::FrontendShared::GpuTimedPass::Main)];

    renderer_vars.raytracing = true;
    // Flush again so the average holds RT-only samples.
    harness.render_frames(window);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_EQ(harness.feedback.visibility.meshes_drawn, 0U)
      << "the raster pass must not run - and must not report stale mesh counts - while RT owns the frame";
    EXPECT_EQ(harness.feedback.visibility.meshes_total, 0U)
      << "the raster pass must not run - and must not report stale mesh counts - while RT owns the frame";

    if (harness.feedback.gpuTimings.supported) {
        const float rt_main_ms =
          harness.feedback.gpuTimings
            .pass_ms[static_cast<size_t>(Kataglyphis::VulkanRendererInternals::FrontendShared::GpuTimedPass::Main)];
        GTEST_LOG_(INFO) << "Main-pass GPU time: forward(raster only)=" << forward_main_ms
                         << "ms, RT(dispatch only)=" << rt_main_ms << "ms";
        const auto &pass_ms = harness.feedback.gpuTimings.pass_ms;
        EXPECT_GE(pass_ms[static_cast<size_t>(GpuTimedPass::RayTrace)], 0.0F) << "an RT frame must time RayTrace";
        EXPECT_LT(pass_ms[static_cast<size_t>(GpuTimedPass::Forward)], 0.0F) << "an RT frame timed Forward";
    }
}

// F5 swaps the path-tracing kernel for another estimator, so its samples must not average into the old mean.
TEST(GoldenRender, ShaderHotReloadRestartsThePathTracedMean)
{
    SKIP_WITHOUT_GPU();

    // A lone card at 1 spp keeps the traced frames cheap; nothing here reads a pixel.
    ScopedModelOverride card(EMISSIVE_CARD_MODEL);
    EngineHarness harness;
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }
    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.pathTracing = true;
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_TRUE(harness.feedback.tracingReady) << "no TLAS after the warm-up, so nothing was path traced";
    const uint64_t before = harness.feedback.pathTracingAccumulatedSamples;
    ASSERT_GE(before, 3U) << "the running mean did not accumulate across the warm-up frames";

    renderer_vars.shader_hot_reload_triggered = true;
    harness.render_frame();
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost after the shader reload.";
    EXPECT_FALSE(renderer_vars.shader_hot_reload_triggered) << "the reload request was not spent";
    EXPECT_EQ(harness.feedback.pathTracingAccumulatedSamples, 1U)
      << "the reloaded kernel's first frame was averaged into the old kernel's " << before << "-sample mean";

    harness.render_frame();
    EXPECT_EQ(harness.feedback.pathTracingAccumulatedSamples, 2U) << "the restarted mean does not accumulate";
}

// Tiles and sample chunks change only the float summation order: each sample's index in the mean seeds it.
TEST(GoldenRender, PathTracingSplitsAFrameWithoutChangingItsMean)
{
    SKIP_WITHOUT_GPU();

    const Kataglyphis::TestSupport::ScopedLogCapture capture;
    // A lone card at 4 spp and two bounces: the sun's NEE, a bounce and the sky, in a few cheap frames.
    ScopedModelOverride card(METALLIC_CARD_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }
    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.pathTracing = true;
    renderer_vars.pathTracingSamplesPerPixel = 4;
    // One bounce while warming up: the same split at two, so only the bounces can log the plan again.
    renderer_vars.pathTracingMaxBounces = 1;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_TRUE(harness.feedback.tracingReady) << "no TLAS after the warm-up, so nothing was path traced";
    renderer_vars.pathTracingMaxBounces = 2;
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, 2.0F));
    harness.camera->set_orientation(-90.0F, 0.0F);

    // Each run restarts the mean and traces the same 12 samples per pixel in three frames.
    uint32_t width = 0;
    uint32_t height = 0;
    const auto traced_mean = [&] {
        renderer_vars.pathTracingRestartRequested = true;
        harness.render_frames(2);
        std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        EXPECT_EQ(harness.feedback.pathTracingAccumulatedSamples, 12U) << "the restart did not start the mean over";
        return radiance;
    };
    const std::vector<float> whole = traced_mean();
    // One sample of a whole tile per dispatch.
    const uint64_t one_sample = uint64_t{ Kataglyphis::kPathTracingTileSize } * Kataglyphis::kPathTracingTileSize * 2U;
    std::vector<float> split;
    {
        const ScopedEnvVar budget("KATAGLYPHIS_PT_DISPATCH_SEGMENTS", std::to_string(one_sample).c_str());
        split = traced_mean();
    }
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(whole.empty());
    ASSERT_EQ(split.size(), whole.size());

    // The default budget holds a tile's four samples in one dispatch and the small one splits them, as the plan says.
    const std::size_t tiles = Kataglyphis::planPathTracingDispatches(width, height, 4, 2, 0).size();
    const std::size_t chunks = Kataglyphis::planPathTracingDispatches(width, height, 4, 2, 0, one_sample).size();
    ASSERT_EQ(chunks, 4U * tiles) << "the small budget does not split the samples";
    const auto plan_line = [](std::size_t dispatches, uint32_t samples, uint32_t bounces) {
        return "path tracer: " + std::to_string(dispatches) + " dispatches a frame, up to " + std::to_string(samples)
               + " samples of " + std::to_string(bounces) + " bounces each";
    };
    const std::vector<std::string> expected_plans = {
        plan_line(tiles, 4, 1), plan_line(tiles, 4, 2), plan_line(chunks, 1, 2)
    };
    EXPECT_EQ(dispatch_plan_lines(capture), expected_plans)
      << "the renderer must log the warm-up's, the whole and the split frame's plan, each once";

    const HdrDifference difference = hdr_difference(whole, split);
    GTEST_LOG_(INFO) << "split against whole frames: largest relative difference " << difference.largest << ", mean "
                     << difference.mean;
    // Half-float rounding of fp32 means that differ in their last bits; a sample drawn differently moves far more.
    EXPECT_LT(difference.largest, 0.01) << "a split frame traced other samples than the whole one";
    EXPECT_LT(difference.mean, 1.0e-4) << "a split frame's mean drifted from the whole frame's";
}

// The editor's Restart accumulation and White furnace: what Rust's PATH TRACING rows do, through the same settings.
TEST(GoldenRender, PathTracingRestartAndTheFurnaceFollowTheEditor)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride card(METALLIC_CARD_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }
    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.pathTracing = true;
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_TRUE(harness.feedback.tracingReady) << "no TLAS after the warm-up, so nothing was path traced";
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, 2.0F));
    harness.camera->set_orientation(-90.0F, 0.0F);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto first_frame_after_restart = [&] {
        renderer_vars.pathTracingRestartRequested = true;
        std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        EXPECT_FALSE(renderer_vars.pathTracingRestartRequested) << "the restart request was not spent";
        EXPECT_EQ(harness.feedback.pathTracingAccumulatedSamples, 1U) << "the restarted mean holds older samples";
        return radiance;
    };

    // A restarted mean draws the samples of the first frame after any reset again, so the frame repeats exactly.
    const std::vector<float> first = first_frame_after_restart();
    harness.render_frames(2);
    const std::vector<float> deeper = harness.capture_hdr_frame(width, height);
    EXPECT_EQ(harness.feedback.pathTracingAccumulatedSamples, 4U) << "the mean does not count its samples";
    const std::vector<float> again = first_frame_after_restart();
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(first.empty());
    ASSERT_EQ(again.size(), first.size());
    const HdrDifference restarted = hdr_difference(first, again);
    const HdrDifference accumulated = hdr_difference(first, deeper);
    GTEST_LOG_(INFO) << "restarted frame against the first: largest " << restarted.largest << "; four samples: mean "
                     << accumulated.mean;
    EXPECT_LT(restarted.largest, 1.0e-6) << "the restarted frame is not the first frame after a reset";
    EXPECT_GT(accumulated.mean, 1.0e-3) << "four samples left the one-sample frame unchanged - the mean is frozen";

    // The furnace's sky has radiance 1, so the corners, where every camera ray misses the card, read exactly 1.
    const auto corner = [&](const std::vector<float> &radiance) {
        const std::size_t pixel = (static_cast<std::size_t>(height / 8U) * width + width / 8U) * 4U;
        return glm::vec3(radiance[pixel], radiance[pixel + 1U], radiance[pixel + 2U]);
    };
    GTEST_LOG_(INFO) << "corner without the furnace: (" << corner(first).r << ", " << corner(first).g << ", "
                     << corner(first).b << ")";
    EXPECT_GT(glm::length(corner(first) - glm::vec3(1.0F)), 0.05F) << "the scene's sky already reads 1 at the corner";

    renderer_vars.pathTracingFurnace = true;
    const std::vector<float> editor_furnace = harness.capture_hdr_frame(width, height);
    EXPECT_EQ(harness.feedback.pathTracingAccumulatedSamples, 1U) << "the White furnace toggle kept the scene's mean";
    ASSERT_FALSE(editor_furnace.empty());
    EXPECT_EQ(corner(editor_furnace), glm::vec3(1.0F)) << "the editor's furnace sky is not a uniform 1";

    // The goldens' KATAGLYPHIS_PT_FURNACE=1 is the same furnace: radiance 1, a white dielectric at roughness 0.5.
    renderer_vars.pathTracingFurnace = false;
    std::vector<float> variable_furnace;
    {
        const ScopedEnvVar furnace("KATAGLYPHIS_PT_FURNACE", "1.0");
        variable_furnace = first_frame_after_restart();
    }
    ASSERT_EQ(variable_furnace.size(), editor_furnace.size());
    EXPECT_LT(hdr_difference(editor_furnace, variable_furnace).largest, 1.0e-6)
      << "the editor's White furnace is not the furnace KATAGLYPHIS_PT_FURNACE=1 draws";

    const std::vector<float> scene = harness.capture_hdr_frame(width, height);
    EXPECT_EQ(harness.feedback.pathTracingAccumulatedSamples, 1U) << "leaving the furnace kept the furnace's mean";
    EXPECT_LT(hdr_difference(first, scene).largest, 1.0e-6) << "the scene did not come back after the furnace";
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
}

// Nothing rewrites the traced sets per frame, so each change they depend on must reach updateAllDescriptorSets.
TEST(GoldenRender, TracedFramesFollowTheRasterModeAndTheSwapchain)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    ScopedValidationErrorCounter validation_errors;
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> forward = harness.capture_frame(width, height);
    ASSERT_FALSE(forward.empty());
    ASSERT_GT(distinct_luminance_buckets(forward), 16U) << "the traced rig frame is blank - check the scene";

    const auto differing = [&](const std::vector<uint8_t> &other) {
        size_t count = 0;
        for (size_t pixel = 0; pixel < forward.size() / 4U; ++pixel) {
            if (channel_delta(forward, other, pixel) > 8) { ++count; }
        }
        return static_cast<double>(count) / static_cast<double>(forward.size() / 4U);
    };

    // The traced pass writes the mode's offscreen image, which post then reads.
    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> deferred = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(deferred.size(), forward.size());
    const double deferred_differing = differing(deferred);

    // A recreated swapchain recreates every image the traced sets name.
    harness.renderer->recreateSwapChain();
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> recreated = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(recreated.size(), forward.size());
    const double recreated_differing = differing(recreated);

    GTEST_LOG_(INFO) << "traced rig, pixels differing from the forward-mode frame: " << deferred_differing
                     << " in deferred mode, " << recreated_differing << " after the swapchain recreation";
    EXPECT_LT(deferred_differing, 0.01) << "the traced frame did not follow the switch to deferred mode: the traced "
                                           "pass still writes the forward image while post reads the deferred one";
    EXPECT_LT(recreated_differing, 0.01) << "the traced frame changed across a swapchain recreation";
    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "a traced frame logged an error - a descriptor still naming an image the recreation destroyed";
}

// Ray tracing traces a binary shadow ray and path tracing uses NEE; the cascade pass once ran under both anyway.
TEST(GoldenRender, TracedFramesSkipTheShadowPass)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.useForwardRaster();
    harness.settings.scene.shadows_enabled = true;
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    const auto shadow_pass_ms = [&harness] {
        return harness.feedback.gpuTimings.pass_ms[static_cast<size_t>(GpuTimedPass::ShadowCascades)];
    };

    // A fresh query pool reads back nothing for its first frames, so the raster control runs long enough to sample.
    harness.render_frames(WARMUP_FRAMES + 10);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_GT(harness.feedback.visibility.shadow_casters_total, 0U) << "the raster frame drew no shadow casters";
    if (harness.feedback.gpuTimings.supported) {
        EXPECT_GE(shadow_pass_ms(), 0.0F) << "the raster frame's shadow pass reported no GPU time";
    }

    for (const bool raytracing : { true, false }) {
        renderer_vars.raytracing = raytracing;
        renderer_vars.pathTracing = !raytracing;
        // Past every swapchain image's timing slice, so no raster sample is left.
        harness.render_frames(6);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());

        const char *mode = raytracing ? "ray traced" : "path traced";
        EXPECT_EQ(harness.feedback.visibility.shadow_casters_total, 0U)
          << mode << " frames still record the shadow pass";
        if (harness.feedback.gpuTimings.supported) {
            EXPECT_LT(shadow_pass_ms(), 0.0F) << mode << " frames still time a \"Shadow cascades\" pass";
        }
    }
}

// A missed card shows the furnace sky's 1 too; one segment leaves a hit black (no light, no bounce).
static double furnace_centre_after_one_segment(EngineHarness &harness, const glm::vec3 &position, float yaw)
{
    auto &renderer_vars = harness.settings.renderer;
    const int bounces = renderer_vars.pathTracingMaxBounces;
    renderer_vars.pathTracingMaxBounces = 1;
    harness.camera->set_camera_position(position);
    harness.camera->set_orientation(yaw, 0.0F);
    harness.render_frames(2);
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
    renderer_vars.pathTracingMaxBounces = bounces;
    if (harness.renderer->hasDeviceLost() || radiance.empty()) { return -1.0; }
    const double centre = mean_hdr_in_square(radiance, width, width / 2U, height / 2U, 24U);
    GTEST_LOG_(INFO) << "one-segment placement frame at yaw " << yaw << ": centre " << centre << " of the sky's 1";
    return centre;
}

// A white dielectric under a uniform sky of radiance 1, graded in linear radiance, where ACES hides a few percent.
TEST(GoldenRender, PathTracingPassesTheWhiteFurnaceTest)
{
    SKIP_WITHOUT_GPU();

    // The furnace metal golden covers metallic 1.
    const ScopedEnvVar furnace("KATAGLYPHIS_PT_FURNACE", "1.0");
    const ScopedEnvVar furnace_metallic("KATAGLYPHIS_PT_FURNACE_METALLIC", "0");
    std::optional<ScopedEnvVar> furnace_roughness;

    // A lone card returns its albedo of the sky after one bounce, the first-bounce oracle for both lobes and their mixture.
    {
        ScopedModelOverride card(METALLIC_CARD_MODEL);
        EngineHarness harness;
        SKIP_WITHOUT_FRAME_CAPTURE(harness);
        if (!harness.renderer->supportsHardwareRaytracing()) {
            GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
        }

        auto &renderer_vars = harness.settings.renderer;
        harness.settings.scene.directional_light_radiance = 0.0F;
        renderer_vars.raytracing = false;
        renderer_vars.pathTracing = true;
        renderer_vars.pathTracingSamplesPerPixel = 4;
        renderer_vars.pathTracingMaxBounces = 2;
        harness.render_frames(WARMUP_FRAMES);

        struct View
        {
            float mu;
            glm::vec3 position;
            float yaw;
        };
        const std::array<View, 2> views = { View{ 1.0F, glm::vec3(0.0F, 0.0F, 2.0F), -90.0F },
            View{ 0.5F, glm::vec3(1.7320508F, 0.0F, 1.0F), -150.0F } };
        for (const View &view : views) {
            const double centre = furnace_centre_after_one_segment(harness, view.position, view.yaw);
            ASSERT_GE(centre, 0.0) << "the placement frame failed at mu " << view.mu;
            EXPECT_LT(centre, 0.01) << "the card does not cover the measured square at mu " << view.mu << ": " << centre
                                    << " of the sky's 1 behind it";
        }
        const int frames = path_traced_history(8, 4);
        // 0.25 narrows the GGX lobe until the 0.1 floor on its selection chance decides the mixture.
        for (const char *roughness : { "0.5", "0.25" }) {
            furnace_roughness.reset();
            furnace_roughness.emplace("KATAGLYPHIS_PT_FURNACE_ROUGHNESS", roughness);
            for (const View &view : views) {
                harness.camera->set_camera_position(view.position);
                harness.camera->set_orientation(view.yaw, 0.0F);
                harness.render_frames(frames);
                uint32_t width = 0;
                uint32_t height = 0;
                const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
                ASSERT_FALSE(harness.renderer->hasDeviceLost());
                ASSERT_FALSE(radiance.empty());

                const double measured = mean_hdr_in_square(radiance, width, width / 2U, height / 2U, 24U);
                const double single =
                  white_single_scatter_albedo(static_cast<double>(view.mu), std::strtod(roughness, nullptr), 0.0);
                GTEST_LOG_(INFO) << "white dielectric card, roughness " << roughness << ", mu " << view.mu << ": "
                                 << measured << " of a sky of 1 (single scattering " << single << ")";
                EXPECT_NEAR(measured, kWhiteFurnaceAlbedo, 0.01)
                  << "the lobe mixture or its energy compensation is off: roughness " << roughness << ", mu "
                  << view.mu;
            }
        }
    }

    // Many bounces compound a per-bounce error, which bounds centred on the sky's 1 catch either way.
    furnace_roughness.reset();
    furnace_roughness.emplace("KATAGLYPHIS_PT_FURNACE_ROUGHNESS", "0.5");
    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    // NEE would add energy on top of the furnace; many bounces keep truncation loss small.
    scene_vars.directional_light_radiance = 0.0F;
    renderer_vars.pathTracingMaxBounces = 16;
    harness.render_frames(WARMUP_FRAMES);

    constexpr double FURNACE_PIXEL_BAND = 0.1;
    struct CropStats
    {
        double mean = 0.0;
        double uniform_fraction = 0.0;
        double dark_fraction = 0.0;
    };
    const auto crop_stats = [](const std::vector<float> &radiance, uint32_t width, uint32_t height) {
        const uint32_t x0 = (width * 18U) / 25U;
        const uint32_t x1 = (width * 49U) / 50U;
        const uint32_t y0 = height / 20U;
        const uint32_t y1 = (height * 19U) / 20U;
        double sum = 0.0;
        size_t within = 0;
        size_t dark = 0;
        size_t total = 0;
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = x0; x < x1; ++x) {
                const size_t base = (static_cast<size_t>(y) * width + x) * 4U;
                const double value =
                  static_cast<double>(radiance[base] + radiance[base + 1U] + radiance[base + 2U]) / 3.0;
                sum += value;
                if (std::abs(value - 1.0) <= FURNACE_PIXEL_BAND) { ++within; }
                if (value < 0.5) { ++dark; }
                ++total;
            }
        }
        const auto share = [total](size_t count) { return static_cast<double>(count) / static_cast<double>(total); };
        return CropStats{ sum / static_cast<double>(total), share(within), share(dark) };
    };

    // A missed rig reads the sky's 1 in the crop too, so one segment must show its geometry there (measured 0.748).
    constexpr double FURNACE_RIG_MIN_COVERED = 0.5;
    uint32_t width = 0;
    uint32_t height = 0;
    renderer_vars.pathTracingMaxBounces = 1;
    harness.render_frames(2);
    const std::vector<float> one_segment = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(one_segment.empty());
    const double covered = crop_stats(one_segment, width, height).dark_fraction;
    GTEST_LOG_(INFO) << "furnace rig crop covered by geometry after one segment: " << covered;
    EXPECT_GT(covered, FURNACE_RIG_MIN_COVERED) << "the rig's geometry is not in the crop the furnace grades";

    renderer_vars.pathTracingMaxBounces = 16;
    harness.render_frames(path_traced_history(80, 40));
    const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(radiance.empty());

    // Measured 0.9997 in both variants; albedo 0.99 gives 0.9889 and 1.01 gives 1.0106, each past its bound.
    constexpr double FURNACE_RIG_MEAN_MIN = 0.994;
    constexpr double FURNACE_RIG_MEAN_MAX = 1.005;
    const CropStats graded = crop_stats(radiance, width, height);
    const double mean = graded.mean;
    const double uniform_fraction = graded.uniform_fraction;
    GTEST_LOG_(INFO) << "furnace rig crop mean radiance: " << mean << " (sky 1), fraction within +-"
                     << FURNACE_PIXEL_BAND << ": " << uniform_fraction;

    EXPECT_GT(mean, FURNACE_RIG_MEAN_MIN) << "Furnace converges LOW - the estimator is losing energy.";
    EXPECT_LT(mean, FURNACE_RIG_MEAN_MAX) << "Furnace converges HIGH - the estimator is gaining energy.";
    EXPECT_GT(uniform_fraction, 0.98) << "The furnace image is not uniform - geometry is visible, so some path "
                                         "class is biased.";
}

// A white metal reflects through its GGX lobe alone; with the multiple-scattering gain it returns the whole sky.
TEST(GoldenRender, PathTracingPassesTheWhiteMetalFurnaceTest)
{
    SKIP_WITHOUT_GPU();

    const ScopedEnvVar furnace("KATAGLYPHIS_PT_FURNACE", "1.0");
    const ScopedEnvVar furnace_metallic("KATAGLYPHIS_PT_FURNACE_METALLIC", "1");
    std::optional<ScopedEnvVar> furnace_roughness;
    // A lone card: every reflected ray reaches the sky, so a pixel's radiance is the lobe's albedo times the sky's.
    ScopedModelOverride card(METALLIC_CARD_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    harness.settings.scene.directional_light_radiance = 0.0F;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.pathTracingSamplesPerPixel = 8;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    struct View
    {
        float mu;
        glm::vec3 position;
        float yaw;
    };
    // Two units from the card's centre, head-on and at 60 degrees; the measured square spans a few degrees.
    const std::array<View, 2> views = { View{ 1.0F, glm::vec3(0.0F, 0.0F, 2.0F), -90.0F },
        View{ 0.5F, glm::vec3(1.7320508F, 0.0F, 1.0F), -150.0F } };
    for (const View &view : views) {
        const double centre = furnace_centre_after_one_segment(harness, view.position, view.yaw);
        ASSERT_GE(centre, 0.0) << "the placement frame failed at mu " << view.mu;
        EXPECT_LT(centre, 0.01) << "the card does not cover the measured square at mu " << view.mu << ": " << centre
                                << " of the sky's 1 behind it";
    }
    // At roughness 1 a single scatter returns 0.307 of the sky head-on, so the gain triples it.
    for (const char *roughness : { "0.3", "0.7", "1.0" }) {
        furnace_roughness.reset();
        furnace_roughness.emplace("KATAGLYPHIS_PT_FURNACE_ROUGHNESS", roughness);
        for (const View &view : views) {
            // A camera move restarts the history, so each pair accumulates on its own.
            harness.camera->set_camera_position(view.position);
            harness.camera->set_orientation(view.yaw, 0.0F);
            harness.render_frames(12);
            uint32_t width = 0;
            uint32_t height = 0;
            const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
            ASSERT_FALSE(harness.renderer->hasDeviceLost());
            ASSERT_FALSE(radiance.empty());

            const double measured = mean_hdr_in_square(radiance, width, width / 2U, height / 2U, 24U);
            const double single =
              white_single_scatter_albedo(static_cast<double>(view.mu), std::strtod(roughness, nullptr), 1.0);
            GTEST_LOG_(INFO) << "white metal, roughness " << roughness << ", mu " << view.mu << ": " << measured
                             << " of a sky of 1 (single scattering " << single << ")";
            EXPECT_NEAR(measured, kWhiteFurnaceAlbedo, 0.01)
              << "the GGX lobe's sampling weight or its gain is off: roughness " << roughness << ", mu " << view.mu;
        }
    }
}

// A metal reflects only through its specular lobe; the path tracer once had none and rendered every metal black.
TEST(GoldenRender, PathTracedMetalCardReflectsTheSkyAndTheSun)
{
    SKIP_WITHOUT_GPU();

    const auto metal = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_white_metal_card.gltf", [](auto &gltf) {
        auto &material = gltf["materials"][0];
        material["pbrMetallicRoughness"] = {
            { "baseColorFactor", { 0.9, 0.9, 0.9, 1.0 } }, { "metallicFactor", 1.0 }, { "roughnessFactor", 0.3 }
        };
        material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
    });
    ASSERT_TRUE(metal.has_value()) << "could not write the white metal card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.pathTracingSamplesPerPixel = 4;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    // Mirror images of one placement; only the right card turns its normal halfway between the sun and the camera.
    const glm::vec3 right_centre(5.0F, 4.0F, 15.0F);
    const glm::vec3 to_sun = glm::normalize(glm::vec3(scene_vars.directional_light_direction[0],
      scene_vars.directional_light_direction[1],
      scene_vars.directional_light_direction[2]));
    const glm::vec3 to_camera = glm::normalize(harness.camera->get_camera_position() - right_centre);
    const glm::vec3 mirror_normal = glm::normalize(to_sun + to_camera);
    const glm::vec3 card_normal(0.0F, 0.0F, 1.0F);
    const glm::mat4 highlight = glm::translate(glm::mat4(1.0F), right_centre)
                                * glm::rotate(glm::mat4(1.0F),
                                  std::acos(glm::dot(card_normal, mirror_normal)),
                                  glm::normalize(glm::cross(card_normal, mirror_normal)))
                                * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    const glm::vec3 left_centre(-right_centre.x, right_centre.y, right_centre.z);
    const glm::mat4 plain = glm::translate(glm::mat4(1.0F), left_centre) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    ASSERT_TRUE(harness.renderer->addModel(metal->string(), highlight).has_value()) << "adding the tilted card failed";
    ASSERT_TRUE(harness.renderer->addModel(metal->string(), plain).has_value()) << "adding the plain card failed";
    harness.render_frames(SETTLE_FRAMES + 12);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(frame.empty());
    const Crop card_box = panel_free_card_box(width, height);
    const uint32_t centre_x = (card_box.x0 + card_box.x1) / 2U;
    const uint32_t centre_y = (card_box.y0 + card_box.y1) / 2U;
    const double highlight_mean = mean_luminance_in_square(frame, width, centre_x, centre_y, 20U);
    const double plain_mean = mean_luminance_in_square(frame, width, width - centre_x, centre_y, 20U);
    GTEST_LOG_(INFO) << "path-traced white metal: " << highlight_mean << " where the sun mirrors into the camera, "
                     << plain_mean << " facing it";

    // Facing the camera, the card mirrors the sky behind it at F0 0.9: lit, if far from the sun's highlight.
    EXPECT_GT(plain_mean, 60.0) << "the metal card is (nearly) black: no specular lobe reflects the sky";
    // The sun's GGX peak is a hundredfold the sky; tonemapped it is white.
    EXPECT_GT(highlight_mean, 245.0) << "no specular highlight where the sun mirrors into the camera";
    EXPECT_GT(highlight_mean, plain_mean + 30.0) << "the highlight is not brighter than the reflected sky";
}

namespace {

// The mean of `pixels` over the centred square of side 2 * half, in linear RGB.
glm::dvec3 centre_mean_rgb(const std::vector<float> &pixels, uint32_t width, uint32_t height, uint32_t half)
{
    glm::dvec3 sum(0.0);
    for (uint32_t y = height / 2U - half; y < height / 2U + half; ++y) {
        for (uint32_t x = width / 2U - half; x < width / 2U + half; ++x) {
            const size_t base = (static_cast<size_t>(y) * width + x) * 4U;
            for (int c = 0; c < 3; ++c) { sum[c] += static_cast<double>(pixels[base + static_cast<size_t>(c)]); }
        }
    }
    return sum / static_cast<double>(4U * half * half);
}

}// namespace

// A bounce that misses gets the ambient term's light, the IBL or the analytic sky, times the Sun's Ambient slider.
TEST(GoldenRender, PathTracedMirrorReflectsTheSkyboxItDraws)
{
    SKIP_WITHOUT_GPU();

    // F0 1 and the 0.045 roughness floor: the card returns the sky behind the camera, mirrored left to right.
    const auto mirror = write_gltf_variant(METALLIC_CARD_MODEL, "kataglyphis_mirror_card.gltf", [](auto &gltf) {
        gltf["materials"][0]["pbrMetallicRoughness"] = {
            { "baseColorFactor", { 1.0, 1.0, 1.0, 1.0 } }, { "metallicFactor", 1.0 }, { "roughnessFactor", 0.0 }
        };
    });
    ASSERT_TRUE(mirror.has_value()) << "could not write the mirror card";

    ScopedModelOverride card(mirror->string().c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
    scene_vars.directional_light_radiance = 0.0F;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.pathTracingSamplesPerPixel = 4;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);
    const glm::vec3 eye(0.0F, 0.0F, 2.0F);
    harness.camera->set_camera_position(eye);
    harness.camera->set_orientation(-90.0F, 0.0F);

    constexpr uint32_t HALF = 24U;
    uint32_t width = 0;
    uint32_t height = 0;
    const auto traced_mirror = [&](bool skybox, float ambient) {
        scene_vars.skybox_enabled = skybox;
        scene_vars.directional_light_ambient = ambient;
        harness.render_frames(path_traced_history(8, 4));
        const std::vector<float> hdr = harness.capture_hdr_frame(width, height);
        EXPECT_FALSE(hdr.empty());
        return hdr.empty() ? glm::dvec3(-1.0) : centre_mean_rgb(hdr, width, height, HALF);
    };
    constexpr float AMBIENT = 0.35F;
    const glm::dvec3 analytic = traced_mirror(false, AMBIENT);
    const glm::dvec3 cubemap = traced_mirror(true, AMBIENT);
    const glm::dvec3 doubled = traced_mirror(true, 2.0F * AMBIENT);
    ASSERT_GT(width, 2U * HALF);
    ASSERT_GT(height, 2U * HALF);

    // The analytic sky along each crop pixel's ray as the card at z = 0, facing +z, reflects it.
    const glm::mat4 clip_to_world = harness_inverse_view_projection(harness, width, height);
    glm::dvec3 gradient(0.0);
    for (uint32_t y = height / 2U - HALF; y < height / 2U + HALF; ++y) {
        for (uint32_t x = width / 2U - HALF; x < width / 2U + HALF; ++x) {
            const glm::vec2 ndc((static_cast<float>(x) + 0.5F) / static_cast<float>(width) * 2.0F - 1.0F,
              (static_cast<float>(y) + 0.5F) / static_cast<float>(height) * 2.0F - 1.0F);
            const glm::vec4 point = clip_to_world * glm::vec4(ndc, 0.5F, 1.0F);
            const glm::vec3 view = glm::normalize(glm::vec3(point) / point.w - eye);
            gradient += glm::dvec3(Kataglyphis::analyticSkyRadiance(glm::reflect(view, glm::vec3(0.0F, 0.0F, 1.0F))));
        }
    }
    gradient /= static_cast<double>(4U * HALF * HALF);

    // The sky the raster draws where the mirror points, in HDR: sky.slang untonemaps the cube map, as the bake does.
    harness.useForwardRaster();
    harness.camera->set_orientation(90.0F, 0.0F);
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<float> drawn_hdr = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(drawn_hdr.empty());
    const glm::dvec3 sky = centre_mean_rgb(drawn_hdr, width, height, HALF);
    const auto ambient = static_cast<double>(AMBIENT);

    GTEST_LOG_(INFO) << "mirror card at ambient " << AMBIENT << ", skybox off: (" << analytic.r << ", " << analytic.g
                     << ", " << analytic.b << ") against the analytic sky it reflects times it ("
                     << ambient * gradient.r << ", " << ambient * gradient.g << ", " << ambient * gradient.b
                     << "); skybox on: (" << cubemap.r << ", " << cubemap.g << ", " << cubemap.b
                     << ") against the drawn sky times it (" << ambient * sky.r << ", " << ambient * sky.g << ", "
                     << ambient * sky.b << "); at twice the ambient (" << doubled.r << ", " << doubled.g << ", "
                     << doubled.b << ")";

    for (int c = 0; c < 3; ++c) {
        // The switched-off skybox lights with the analytic sky, as the raster's ambient term does; the navy is no light.
        EXPECT_NEAR(analytic[c], ambient * gradient[c], 0.05 * ambient * gradient[c] + 0.002)
          << "with the skybox off a bounce that misses is not the analytic sky times the ambient, channel " << c;
        EXPECT_NEAR(cubemap[c], ambient * sky[c], 0.08 * ambient * sky[c] + 0.002)
          << "the mirror does not reflect the IBL baked from the drawn skybox, channel " << c;
        EXPECT_NEAR(doubled[c], 2.0 * cubemap[c], 0.05 * cubemap[c] + 0.002)
          << "the bounce sky does not follow the Ambient slider, channel " << c;
    }
}

// A camera ray that misses sees what the sky pass draws, in either sky mode: both read OxidANT's drawn_sky.slang.
TEST(GoldenRender, PathTracedCameraMissShowsTheDrawnSky)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride card(METALLIC_CARD_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    harness.render_frames(WARMUP_FRAMES);
    // Facing away from the card, at z = 0 behind the camera, so the crop holds sky alone.
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, 2.0F));
    harness.camera->set_orientation(90.0F, 10.0F);

    constexpr uint32_t HALF = 24U;
    const auto sky_in = [&](bool path_traced) {
        harness.useForwardRaster();
        renderer_vars.pathTracing = path_traced;
        harness.render_frames(path_traced ? path_traced_history(4, 2) : SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<float> hdr = harness.capture_hdr_frame(width, height);
        EXPECT_FALSE(hdr.empty());
        return hdr.empty() ? glm::dvec3(-1.0) : centre_mean_rgb(hdr, width, height, HALF);
    };

    std::vector<glm::dvec3> drawn;
    for (const bool skybox : { true, false }) {
        scene_vars.skybox_enabled = skybox;
        const glm::dvec3 raster = sky_in(false);
        const glm::dvec3 traced = sky_in(true);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());
        GTEST_LOG_(INFO) << "skybox " << (skybox ? "on" : "off") << ": raster (" << raster.r << ", " << raster.g << ", "
                         << raster.b << "), path traced (" << traced.r << ", " << traced.g << ", " << traced.b << ")";
        for (int c = 0; c < 3; ++c) {
            // The path tracer jitters its rays inside the pixel, which barely moves the crop's mean.
            EXPECT_NEAR(traced[c], raster[c], 0.02 * raster[c] + 0.002)
              << "skybox " << (skybox ? "on" : "off") << ": a path-traced camera miss is not the drawn sky, channel "
              << c;
        }
        drawn.push_back(raster);
    }
    // The cube map and the navy differ, so the check above saw both modes.
    EXPECT_GT(glm::length(drawn[0] - drawn[1]), 0.05) << "the two skies are alike, so the test proves nothing";
}

// An unlit glTF material that omits metallicFactor (1 by the spec) once went black in PT: metallic darkened it first.
TEST(GoldenRender, PathTracedUnlitCardKeepsItsAlbedo)
{
    SKIP_WITHOUT_GPU();

    const auto unlit = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_unlit_orange_card.gltf", [](auto &gltf) {
        auto &material = gltf["materials"][0];
        material["pbrMetallicRoughness"] = { { "baseColorFactor", { 0.8, 0.4, 0.1, 1.0 } } };
        material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
        material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
        gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_materials_unlit" });
    });
    ASSERT_TRUE(unlit.has_value()) << "could not write the unlit card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_TRUE(harness.renderer->addModel(unlit->string(), panel_free_card_placement()).has_value())
      << "adding the unlit card failed";
    harness.render_frames(SETTLE_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<float> forward = harness.capture_hdr_frame(width, height);
    renderer_vars.pathTracing = true;
    // Unlit ends every path at its first hit, so the card interior converges in one frame.
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<float> traced = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(forward.empty());
    ASSERT_EQ(traced.size(), forward.size());

    const Crop card_box = panel_free_card_box(width, height);
    const uint32_t centre_x = (card_box.x0 + card_box.x1) / 2U;
    const uint32_t centre_y = (card_box.y0 + card_box.y1) / 2U;
    const size_t centre = (static_cast<size_t>(centre_y) * width + centre_x) * 4U;
    GTEST_LOG_(INFO) << "unlit card centre: forward (" << forward[centre] << ", " << forward[centre + 1U] << ", "
                     << forward[centre + 2U] << "), path traced (" << traced[centre] << ", " << traced[centre + 1U]
                     << ", " << traced[centre + 2U] << ")";

    // Raster returns the albedo for an unlit material; the path tracer must add it as the path's last radiance.
    constexpr std::array<float, 3> ALBEDO = { 0.8F, 0.4F, 0.1F };
    for (size_t channel = 0; channel < 3U; ++channel) {
        EXPECT_NEAR(forward[centre + channel], ALBEDO[channel], 0.01F)
          << "forward does not show the unlit card's albedo in channel " << channel << " - check the placement";
        EXPECT_NEAR(traced[centre + channel], forward[centre + channel], 0.01F)
          << "the path tracer darkens an unlit material that omits metallicFactor, channel " << channel;
    }
}

// Traced camera rays start at the near plane, where raster clips (owner 2026-11-19); from the eye they hit a card raster never draws.
TEST(GoldenRender, TracedCameraRaysStartAtTheNearPlane)
{
    SKIP_WITHOUT_GPU();

    const auto card = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_near_plane_card.gltf", [](auto &gltf) {
        auto &material = gltf["materials"][0];
        material["pbrMetallicRoughness"] = { { "baseColorFactor", { 0.8, 0.4, 0.1, 1.0 } } };
        material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
        material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
        gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_materials_unlit" });
    });
    ASSERT_TRUE(card.has_value()) << "could not write the unlit card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; no traced camera ray to check.";
    }

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    constexpr float EYE_Z = 3.0F;
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, EYE_Z));
    harness.camera->set_orientation(-90.0F, 0.0F);
    harness.render_frames(WARMUP_FRAMES);
    // Half the 0.1 near distance from the eye and far wider than the frustum there; the control card lies beyond.
    ASSERT_TRUE(
      harness.renderer->addModel(card->string(), glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, EYE_Z - 0.05F)))
        .has_value())
      << "adding the card inside the near plane failed";
    ASSERT_TRUE(harness.renderer
        ->addModel(card->string(),
          glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, EYE_Z - 0.5F))
            * glm::scale(glm::mat4(1.0F), glm::vec3(0.1F)))
        .has_value())
      << "adding the control card failed";
    harness.render_frames(SETTLE_FRAMES);

    constexpr std::array<float, 3> ORANGE = { 0.8F, 0.4F, 0.1F };
    const auto is_card = [&](const std::vector<float> &radiance, size_t pixel) {
        for (size_t channel = 0; channel < 3U; ++channel) {
            if (std::abs(radiance[pixel * 4U + channel] - ORANGE[channel]) > 0.02F) { return false; }
        }
        return true;
    };
    for (const auto &[name, raytracing, path_tracing] : { std::tuple{ "forward", false, false },
           std::tuple{ "ray traced", true, false },
           std::tuple{ "path traced", false, true } }) {
        renderer_vars.raytracing = raytracing;
        renderer_vars.pathTracing = path_tracing;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());
        ASSERT_EQ(radiance.size(), static_cast<size_t>(width) * height * 4U);

        // The outer tenth of every side shows the rig, never the card that covers the frustum inside the near plane.
        size_t border = 0;
        size_t border_card = 0;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                if (x >= width / 10U && x < width - width / 10U && y >= height / 10U && y < height - height / 10U) {
                    continue;
                }
                ++border;
                border_card += is_card(radiance, static_cast<size_t>(y) * width + x) ? 1U : 0U;
            }
        }
        const double card_share = static_cast<double>(border_card) / static_cast<double>(border);
        GTEST_LOG_(INFO) << name << ": " << card_share << " of the border shows the card";
        EXPECT_TRUE(is_card(radiance, static_cast<size_t>(height / 2U) * width + width / 2U))
          << name << " does not show the control card beyond the near plane - check the placement";
        EXPECT_LT(card_share, 0.01) << name
                                    << " shows the card inside the near plane: its camera rays start at the eye";
    }
}

// A metallic-roughness texel with G = 0 once made GGX alpha 0: the visible-normal density's 0/0 broke whole samples.
TEST(GoldenRender, PathTracingFloorsAZeroRoughnessTexel)
{
    SKIP_WITHOUT_GPU();

    // White dielectrics: one at roughness factor 1 times a G of 0, one at the 0.045 floor times a G of 1.
    const auto roughness_card = [](const char *file_name, const char *texel, double roughness_factor) {
        return write_gltf_variant(EMISSIVE_CARD_MODEL, file_name, [texel, roughness_factor](nlohmann::json &gltf) {
            auto &material = gltf["materials"][0];
            nlohmann::json texture_ref;
            texture_ref["index"] = 0;
            material["pbrMetallicRoughness"] = { { "baseColorFactor", { 0.8, 0.8, 0.8, 1.0 } },
                { "metallicFactor", 1.0 },
                { "roughnessFactor", roughness_factor },
                { "metallicRoughnessTexture", texture_ref } };
            material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
            nlohmann::json image;
            image["uri"] = texel;
            nlohmann::json texture;
            texture["source"] = 0;
            gltf["images"] = nlohmann::json::array({ image });
            gltf["textures"] = nlohmann::json::array({ texture });
        });
    };
    const auto zero_card = roughness_card("kataglyphis_zero_roughness_card.gltf", ZERO_ROUGHNESS_PNG, 1.0);
    const auto floor_card = roughness_card("kataglyphis_floor_roughness_card.gltf", FULL_ROUGHNESS_PNG, 0.045);
    ASSERT_TRUE(zero_card.has_value() && floor_card.has_value()) << "could not write the roughness cards";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    renderer_vars.pathTracingSamplesPerPixel = 2;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    const glm::mat4 right = panel_free_card_placement();
    const glm::mat4 left = glm::translate(glm::mat4(1.0F), glm::vec3(-2.0F * right[3].x, 0.0F, 0.0F)) * right;
    ASSERT_TRUE(harness.renderer->addModel(zero_card->string(), right).has_value()) << "adding the G = 0 card failed";
    ASSERT_TRUE(harness.renderer->addModel(floor_card->string(), left).has_value()) << "adding the floor card failed";
    harness.render_frames(SETTLE_FRAMES + 16);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(radiance.empty());
    const Crop zero_box = panel_free_card_box(width, height);
    const uint32_t centre_y = (zero_box.y0 + zero_box.y1) / 2U;
    const uint32_t zero_x = (zero_box.x0 + zero_box.x1) / 2U;
    const double zero_mean = mean_hdr_in_square(radiance, width, zero_x, centre_y, 40U);
    const double floor_mean = mean_hdr_in_square(radiance, width, width - zero_x, centre_y, 40U);
    GTEST_LOG_(INFO) << "path-traced card radiance: texel G 0 " << zero_mean << ", roughness floor " << floor_mean;

    ASSERT_GT(floor_mean, 0.05) << "the floor card is unlit - check the placement";
    // The floor makes the two materials one; NaN-zeroed or 1e-6-divided samples move the mean by far more.
    EXPECT_NEAR(zero_mean, floor_mean, 0.03 * floor_mean)
      << "a roughness texel of 0 shades unlike the 0.045 floor: GGX alpha 0 reached the path tracer";
}

// A smooth low-poly mesh's vertex normal can face away where its triangle faces the camera; raster sides by winding.
TEST(GoldenRender, TracedHitsTakeTheirSideFromTheTriangleWinding)
{
    SKIP_WITHOUT_GPU();

    const auto grey_card = [](nlohmann::json &gltf) {
        auto &material = gltf["materials"][0];
        material["pbrMetallicRoughness"] = {
            { "baseColorFactor", { 0.8, 0.8, 0.8, 1.0 } }, { "metallicFactor", 0.0 }, { "roughnessFactor", 0.5 }
        };
        material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
        material["doubleSided"] = false;
    };
    const auto flat = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_flat_grey_card.gltf", grey_card);
    const auto leaning =
      write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_leaning_normal_card.gltf", [&](auto &gltf) {
          grey_card(gltf);
          nlohmann::json buffer;
          buffer["uri"] = LEANING_VERTEX_NORMALS;
          buffer["byteLength"] = 48;
          gltf["buffers"].push_back(buffer);
          nlohmann::json view;
          view["buffer"] = gltf["buffers"].size() - 1U;
          view["byteOffset"] = 0;
          view["byteLength"] = 48;
          view["target"] = 34962;
          gltf["bufferViews"].push_back(view);
          nlohmann::json accessor;
          accessor["bufferView"] = gltf["bufferViews"].size() - 1U;
          accessor["componentType"] = 5126;
          accessor["count"] = 4;
          accessor["type"] = "VEC3";
          gltf["accessors"].push_back(accessor);
          gltf["meshes"][0]["primitives"][0]["attributes"]["NORMAL"] = gltf["accessors"].size() - 1U;
      });
    const auto emitter =
      write_gltf_variant(EMISSIVE_STRENGTH_CARD_MODEL, "kataglyphis_strong_emitter.gltf", [](auto &gltf) {
          gltf["materials"][0]["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = 50.0;
      });
    ASSERT_TRUE(flat.has_value() && leaning.has_value() && emitter.has_value()) << "could not write the cards";

    ScopedModelOverride card(leaning->string().c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; ray tracing unavailable.";
    }

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    // Lit along the leaning normal: raster's N.L is 0.96, a normal flipped by the ray's side gets none.
    scene_vars.directional_light_direction[0] = 0.9F;
    scene_vars.directional_light_direction[1] = 0.3F;
    scene_vars.directional_light_direction[2] = 0.33F;
    renderer_vars.pathTracingSamplesPerPixel = 4;
    renderer_vars.pathTracingMaxBounces = 2;
    // 60 degrees off the card's axis toward -X: the triangle faces the camera, the vertex normal (N.V -0.64) does not.
    harness.camera->set_camera_position(glm::vec3(-1.7320508F, 0.0F, 1.0F));
    harness.camera->set_orientation(-30.0F, 0.0F);
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<float> forward = harness.capture_hdr_frame(width, height);
    renderer_vars.raytracing = true;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<float> traced = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(forward.empty());
    ASSERT_EQ(traced.size(), forward.size());
    const double forward_mean = mean_hdr_in_square(forward, width, width / 2U, height / 2U, 24U);
    const double traced_mean = mean_hdr_in_square(traced, width, width / 2U, height / 2U, 24U);
    GTEST_LOG_(INFO) << "leaning-normal card: forward " << forward_mean << ", ray traced " << traced_mean;
    ASSERT_GT(forward_mean, 0.05) << "forward leaves the card unlit - check the light or the placement";
    EXPECT_NEAR(traced_mean, forward_mean, 0.05 * forward_mean)
      << "the ray-traced hit flips its normal by the vertex normal's side, not the triangle's winding";

    // Sun off: the front sees only the sky, and an emitter wholly behind the card's plane must add nothing.
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    scene_vars.directional_light_radiance = 0.0F;
    harness.render_frames(8);
    const std::vector<float> before = harness.capture_hdr_frame(width, height);
    const glm::mat4 behind = glm::translate(glm::mat4(1.0F), glm::vec3(-1.5F, 0.0F, -1.2F))
                             * glm::rotate(glm::mat4(1.0F), glm::half_pi<float>(), glm::vec3(0.0F, 1.0F, 0.0F))
                             * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    ASSERT_TRUE(harness.renderer->addModel(emitter->string(), behind).has_value()) << "adding the emitter failed";
    harness.render_frames(8);
    const std::vector<float> after = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(after.size(), before.size());
    const double before_mean = mean_hdr_in_square(before, width, width / 2U, height / 2U, 24U);
    const double after_mean = mean_hdr_in_square(after, width, width / 2U, height / 2U, 24U);
    GTEST_LOG_(INFO) << "path-traced leaning-normal card: " << before_mean << " alone, " << after_mean
                     << " with an emitter behind it";
    // The bounce sky is the IBL times the Ambient slider, about 0.124 here; a card that missed it would read 0.
    ASSERT_GT(before_mean, 0.04) << "the path-traced card does not see the sky - check the placement";
    EXPECT_NEAR(after_mean, before_mean, 0.05 * before_mean)
      << "bounces leave through the triangle toward an emitter behind it: the path bound is the vertex normal";

    // A vertex normal facing away from the viewer leaves no lobe, so the triangle's normal shades, as on a flat twin.
    const glm::mat4 in_front = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 0.001F));
    ASSERT_TRUE(harness.renderer->addModel(flat->string(), in_front).has_value()) << "adding the flat card failed";
    harness.render_frames(8);
    const std::vector<float> flat_frame = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(flat_frame.size(), before.size());
    const double flat_mean = mean_hdr_in_square(flat_frame, width, width / 2U, height / 2U, 24U);
    GTEST_LOG_(INFO) << "path-traced flat twin: " << flat_mean;
    EXPECT_NEAR(before_mean, flat_mean, 0.05 * flat_mean)
      << "the path-traced card shades unlike its flat twin: its side or its path bound follows the vertex normal";
}

// Keep LAST in the file: a device-lost abort here kills every later test in the process.
TEST(GoldenRender, RaytracedLargeMeshDoesNotLoseTheDevice)
{
    SKIP_WITHOUT_GPU();

    // No override: the debug scene is the large dinosaur mesh this diagnoses.
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // The PT reproducer device-loses within 0-2 frames.
    constexpr int DIAGNOSTIC_FRAMES = 10;
    harness.render_frames(DIAGNOSTIC_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost())
      << "RT pipeline device-lost on the large dinosaur mesh - the bug is in the shared "
         "vertex-upload/BDA path, not path_tracing.slang/RayQuery specifically.";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_FALSE(frame.empty()) << "Frame capture returned no pixels.";
    EXPECT_GT(fraction_above(frame, 8.0), 0.05)
      << "Captured frame looks blank - the large-mesh RT diagnostic needs a real image to "
         "trust the no-device-lost result.";
}
