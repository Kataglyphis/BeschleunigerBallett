// Goldens: clouds draw, move, stop at the scene depth, take the sky's light and composite in every mode.

#include "GoldenRenderHarness.hpp"

import kataglyphis.vulkan.renderer;

using namespace Kataglyphis::TestSupport::GoldenRender;

namespace {

// Half extents are Scale x 10 in volumetric_clouds.slang, so a test names the box it wants and the density.
void place_cloud_box(GUISceneSharedVars &vars, glm::vec3 centre, glm::vec3 half_extents, float density)
{
    vars.cloud_density_multiplier = density;
    for (int axis = 0; axis < 3; ++axis) {
        vars.cloud_mesh_offset[axis] = centre[axis];
        vars.cloud_mesh_scale[axis] = half_extents[axis] / 10.0F;
    }
}

}// namespace

// A cross-frame hazard shows as a hang, not a pixel; see docs/source/clouds.md § Queue ownership and barriers.
TEST(GoldenRender, CloudsAcrossManyFramesDoesNotLoseTheDevice)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.clouds_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up with clouds enabled.";

    // Well past MAX_FRAME_DRAWS, so a cross-frame hazard gets several chances to surface.
    constexpr int CLOUD_FRAMES = 30;
    harness.render_frames(CLOUD_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost rendering repeated frames with clouds enabled.";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_FALSE(frame.empty()) << "Frame capture returned no pixels.";
}

// A transparent or flat-wash cloud buffer passes the device-survival test; this requires added detail.
TEST(GoldenRender, EnablingCloudsChangesTheFrameAndAddsDetail)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.clouds_enabled = false;

    // Test-only: the default slab sits far above the camera's eye line, and default wisps are too faint to measure.
    place_cloud_box(scene_vars,
      glm::vec3(scene_vars.cloud_mesh_offset[0], 6.0F, scene_vars.cloud_mesh_offset[2]),
      glm::vec3(20000.0F, 100.0F, 20000.0F),
      2.0F);
    scene_vars.cloud_coverage_threshold = 0.60F;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> baseline = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost capturing the clouds-off baseline.";
    ASSERT_FALSE(baseline.empty()) << "Baseline capture returned no pixels.";

    // Same harness, same camera: only the clouds toggle changes.
    scene_vars.clouds_enabled = true;
    constexpr int CLOUD_SETTLE_FRAMES = 30;
    harness.render_frames(CLOUD_SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost rendering with clouds enabled.";

    uint32_t width2 = 0;
    uint32_t height2 = 0;
    const std::vector<uint8_t> clouds = harness.capture_frame(width2, height2);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost capturing the clouds-on frame.";
    ASSERT_FALSE(clouds.empty()) << "Clouds-on capture returned no pixels.";
    ASSERT_EQ(width, width2);
    ASSERT_EQ(height, height2);

    if (const auto dump = Kataglyphis::environmentVariable("KATAGLYPHIS_CLOUDS_DUMP")) {
        const std::string base = *dump;
        const auto stride = static_cast<int>(width) * 4;
        stbi_write_png((base + "-baseline.png").c_str(), int(width), int(height), 4, baseline.data(), stride);
        stbi_write_png((base + "-clouds.png").c_str(), int(width), int(height), 4, clouds.data(), stride);
    }

    // Upper half only: below is geometry whose sharp skeleton edges would inflate the clouds-off baseline.
    const Crop panel_free = panel_free_crop(width, height);
    const Crop crop{
        panel_free.x0, panel_free.x1, panel_free.y0, panel_free.y0 + (panel_free.y1 - panel_free.y0) / 2U
    };
    const double swung = swung_fraction(baseline, clouds, width, height, crop);
    const double baseline_detail = detail_fraction(baseline, width, height, crop);
    const double clouds_detail = detail_fraction(clouds, width, height, crop);

    GTEST_LOG_(INFO) << "clouds toggle: swung fraction " << swung << ", detail baseline " << baseline_detail
                     << ", detail clouds " << clouds_detail;

    // Measured; re-measure if the rig, camera or cloud_mesh_offset default changes.
    constexpr double SWUNG_FRACTION_MIN = 0.05;
    EXPECT_GT(swung, SWUNG_FRACTION_MIN)
      << "Enabling clouds barely changed the panel-free crop; the cloud box may be out of view "
         "(scene_vars.cloud_mesh_offset) or the clouds pass is not writing pixels at all.";

    // A flat noise volume composites as a smooth wash and lowers detail, with no driver-specific value pinned.
    EXPECT_GT(clouds_detail, baseline_detail)
      << "Clouds did not add pixel-to-pixel detail over the clouds-off baseline (baseline=" << baseline_detail
      << ", clouds=" << clouds_detail << "); the noise volume may be flat or uniformly filled again.";
}

// Pixels a clouds-on capture changes against a clouds-off one, per image half; "drifted" counts any 3-level move.
struct CloudFootprint
{
    size_t changed = 0;
    double centroid_x = 0.0;
    std::array<size_t, 2> changed_per_half{};
    std::array<size_t, 2> drifted_per_half{};
};

static CloudFootprint
  cloud_footprint(const std::vector<uint8_t> &off, const std::vector<uint8_t> &on, uint32_t w, uint32_t h)
{
    CloudFootprint footprint;
    double column_sum = 0.0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const int delta = channel_delta(off, on, static_cast<size_t>(y) * w + x);
            const size_t half = x < w / 2U ? 0U : 1U;
            if (delta > 2) { ++footprint.drifted_per_half[half]; }
            if (delta <= 5) { continue; }
            ++footprint.changed;
            ++footprint.changed_per_half[half];
            column_sum += static_cast<double>(x);
        }
    }
    footprint.centroid_x = footprint.changed > 0U ? column_sum / static_cast<double>(footprint.changed) : 0.0;
    return footprint;
}

// Offset places the box and Scale sizes it; the box was once a sheared slab that Offset could not move.
TEST(GoldenRender, CloudOffsetMovesTheCloudVolume)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.clouds_enabled = false;
    scene_vars.cloud_coverage_threshold = 0.3F;
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> baseline = harness.capture_frame(width, height);
    ASSERT_FALSE(baseline.empty());
    dump_cloud_frame("offset-baseline", baseline, width, height);

    // 3-unit boxes between the camera and the rig, either side of the view axis; each projects inside its own half.
    const auto footprint_of_box_at = [&](float x) {
        place_cloud_box(scene_vars, glm::vec3(x, 6.0F, 12.0F), glm::vec3(3.0F), 2.0F);
        scene_vars.clouds_enabled = true;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t w = 0;
        uint32_t h = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(w, h);
        dump_cloud_frame(x < 0.0F ? "offset-left" : "offset-right", frame, w, h);
        EXPECT_EQ(frame.size(), baseline.size());
        return frame.size() == baseline.size() ? cloud_footprint(baseline, frame, width, height) : CloudFootprint{};
    };
    const CloudFootprint left_box = footprint_of_box_at(-6.0F);
    const CloudFootprint right_box = footprint_of_box_at(6.0F);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    GTEST_LOG_(INFO) << "cloud box at x=-6: " << left_box.changed << " px, centroid column " << left_box.centroid_x
                     << "; at x=+6: " << right_box.changed << " px, centroid column " << right_box.centroid_x << " of "
                     << width;

    const double pixels = static_cast<double>(width) * static_cast<double>(height);
    const double half_pixels = pixels / 2.0;
    ASSERT_GT(static_cast<double>(left_box.changed), 0.005 * pixels) << "the box at x=-6 drew no clouds";
    ASSERT_GT(static_cast<double>(right_box.changed), 0.005 * pixels) << "the box at x=+6 drew no clouds";
    EXPECT_LT(left_box.centroid_x, 0.4 * width) << "the box at x=-6 does not show left of the view axis";
    EXPECT_GT(right_box.centroid_x, 0.6 * width) << "the box at x=+6 does not show right of the view axis";
    // Each box leaves the far half alone, the sky included: cloud-free, post must show the clouds-off frame.
    EXPECT_LT(static_cast<double>(left_box.changed_per_half[1]), 0.001 * half_pixels)
      << "the box at x=-6 changed the right half, outside its 3-unit extent";
    EXPECT_LT(static_cast<double>(right_box.changed_per_half[0]), 0.001 * half_pixels)
      << "the box at x=+6 changed the left half, outside its 3-unit extent";
    EXPECT_LT(static_cast<double>(left_box.drifted_per_half[1]), 0.002 * half_pixels)
      << "with clouds on, cloud-free pixels drifted from the clouds-off frame: post's composite is off";
    EXPECT_LT(static_cast<double>(right_box.drifted_per_half[0]), 0.002 * half_pixels)
      << "with clouds on, cloud-free pixels drifted from the clouds-off frame: post's composite is off";
}

// A wall in front of the box hides it: a box behind the rig's slab shows around the slab, never on it.
TEST(GoldenRender, CloudsStopAtTheSceneDepth)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    // The disabled sky is one known colour, so a pixel that is not it is geometry.
    scene_vars.skybox_enabled = false;
    scene_vars.cloud_coverage_threshold = 0.3F;
    // Behind the slab (z -10..2) and above the ground; rays over and under the slab reach it.
    place_cloud_box(scene_vars, glm::vec3(0.0F, 9.0F, -20.0F), glm::vec3(40.0F, 6.0F, 4.0F), 1.0F);
    harness.render_frames(WARMUP_FRAMES);

    for (const RasterizationMode path : { RasterizationMode::Forward, RasterizationMode::Deferred }) {
        const char *name = path == RasterizationMode::Forward ? "forward" : "deferred";
        renderer_vars.rasterizationMode = path;

        scene_vars.clouds_enabled = false;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> off = harness.capture_frame(width, height);
        scene_vars.clouds_enabled = true;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t w2 = 0;
        uint32_t h2 = 0;
        const std::vector<uint8_t> on = harness.capture_frame(w2, h2);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << name;
        ASSERT_FALSE(off.empty()) << name;
        ASSERT_EQ(off.size(), on.size()) << name;
        dump_cloud_frame(std::string("depth-") + name + "-off", off, width, height);
        dump_cloud_frame(std::string("depth-") + name + "-on", on, width, height);

        size_t geometry = 0;
        size_t geometry_changed = 0;
        size_t sky = 0;
        size_t sky_changed = 0;
        for (size_t pixel = 0; pixel < off.size() / 4U; ++pixel) {
            const int delta = channel_delta(off, on, pixel);
            if (is_disabled_sky(off, pixel)) {
                ++sky;
                if (delta > 5) { ++sky_changed; }
            } else {
                ++geometry;
                if (delta > 2) { ++geometry_changed; }
            }
        }
        GTEST_LOG_(INFO) << name << ": " << geometry_changed << " of " << geometry << " geometry px changed, "
                         << sky_changed << " of " << sky << " sky px";

        ASSERT_GT(geometry, off.size() / 4U / 10U) << name << ": the rig fills too little of the frame to judge";
        EXPECT_GT(static_cast<double>(sky_changed), 0.1 * static_cast<double>(sky))
          << name << ": the box behind the slab drew no clouds around it - nothing to hide";
        EXPECT_LT(static_cast<double>(geometry_changed), 0.001 * static_cast<double>(geometry))
          << name << ": clouds behind the slab drew over it - the march ignores the scene depth";
    }
}

// RT and PT write no depth buffer, so their own primary hits must hide the box, as the raster depth does.
TEST(GoldenRender, CloudsStopAtTracedGeometry)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.skybox_enabled = false;
    scene_vars.cloud_coverage_threshold = 0.3F;
    place_cloud_box(scene_vars, glm::vec3(0.0F, 9.0F, -20.0F), glm::vec3(40.0F, 6.0F, 4.0F), 1.0F);
    // One path per pixel and frame: the depth is what is measured here, not the image.
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    harness.render_frames(WARMUP_FRAMES);

    // Raster names the geometry; eroded, as RT's pixel centres and PT's jitter may disagree with it on an edge.
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> raster = harness.capture_frame(width, height);
    ASSERT_FALSE(raster.empty());
    std::vector<bool> geometry_mask(raster.size() / 4U);
    std::vector<bool> sky_mask(raster.size() / 4U);
    for (size_t pixel = 0; pixel < geometry_mask.size(); ++pixel) {
        sky_mask[pixel] = is_disabled_sky(raster, pixel);
        geometry_mask[pixel] = !sky_mask[pixel];
    }
    geometry_mask = eroded(geometry_mask, width, height);
    sky_mask = eroded(sky_mask, width, height);

    struct Mode
    {
        const char *name;
        bool raytracing;
        int frames;
    };
    for (const Mode mode : { Mode{ "ray traced", true, SETTLE_FRAMES }, Mode{ "path traced", false, 8 } }) {
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = !mode.raytracing;

        scene_vars.clouds_enabled = false;
        harness.render_frames(mode.frames);
        uint32_t w1 = 0;
        uint32_t h1 = 0;
        const std::vector<uint8_t> off = harness.capture_frame(w1, h1);
        scene_vars.clouds_enabled = true;
        harness.render_frames(mode.frames);
        uint32_t w2 = 0;
        uint32_t h2 = 0;
        const std::vector<uint8_t> on = harness.capture_frame(w2, h2);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
        ASSERT_EQ(off.size(), raster.size()) << mode.name;
        ASSERT_EQ(on.size(), raster.size()) << mode.name;

        size_t geometry = 0;
        size_t geometry_changed = 0;
        size_t sky = 0;
        size_t sky_changed = 0;
        for (size_t pixel = 0; pixel < geometry_mask.size(); ++pixel) {
            const int delta = channel_delta(off, on, pixel);
            if (geometry_mask[pixel]) {
                ++geometry;
                // PT keeps accumulating across the toggle, so a penumbra pixel may still settle by a few levels.
                if (delta > (mode.raytracing ? 2 : 16)) { ++geometry_changed; }
            } else if (sky_mask[pixel]) {
                ++sky;
                if (delta > 5) { ++sky_changed; }
            }
        }
        GTEST_LOG_(INFO) << mode.name << ": " << geometry_changed << " of " << geometry << " geometry px changed, "
                         << sky_changed << " of " << sky << " sky px";

        ASSERT_GT(geometry, geometry_mask.size() / 10U) << mode.name;
        EXPECT_GT(static_cast<double>(sky_changed), 0.1 * static_cast<double>(sky))
          << mode.name << ": the box behind the slab drew no clouds around it - nothing to hide";
        EXPECT_LT(static_cast<double>(geometry_changed), 0.005 * static_cast<double>(geometry))
          << mode.name << ": clouds behind the slab drew over it - the traced depth does not reach the march";
    }
}

// Over a sky of zero red, a cloud's red is its own tonemapped light, as over a black card; once it was scaled by alpha.
TEST(GoldenRender, CloudsCompositeOverTheSkyAsOverGeometry)
{
    SKIP_WITHOUT_GPU();

    const auto black_card =
      write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_black_unlit_card.gltf", [](auto &gltf) {
          auto &material = gltf["materials"][0];
          material["pbrMetallicRoughness"]["baseColorFactor"] = { 0.0, 0.0, 0.0, 1.0 };
          material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
          material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
          gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_materials_unlit" });
      });
    ASSERT_TRUE(black_card.has_value()) << "could not write the black unlit card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    // The disabled sky is linear (0, 0, 0.5): no red, like the black card.
    scene_vars.skybox_enabled = false;
    scene_vars.cloud_coverage_threshold = 0.3F;
    // Powder darkens thin cloud to almost nothing, and thin, lit cloud is what tells the two composites apart.
    scene_vars.cloud_powder_effect = false;
    // Beyond the ground's far edge, in the band of sky under the slab; the card stands behind them.
    place_cloud_box(scene_vars, glm::vec3(0.0F, 4.0F, -40.0F), glm::vec3(40.0F, 4.0F, 5.0F), 0.6F);
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture = [&](bool clouds) {
        scene_vars.clouds_enabled = clouds;
        harness.render_frames(SETTLE_FRAMES);
        return harness.capture_frame(width, height);
    };
    const std::vector<uint8_t> sky_off = capture(false);
    const std::vector<uint8_t> sky_on = capture(true);

    const glm::mat4 card_placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 5.0F, -60.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(20.0F));
    ASSERT_TRUE(harness.renderer->addModel(black_card->string(), card_placement).has_value())
      << "adding the black card failed";
    const std::vector<uint8_t> card_off = capture(false);
    const std::vector<uint8_t> card_on = capture(true);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    for (const auto &[name, frame] : { std::pair{ "sky-off", &sky_off },
           std::pair{ "sky-on", &sky_on },
           std::pair{ "card-off", &card_off },
           std::pair{ "card-on", &card_on } }) {
        dump_cloud_frame(std::string("composite-") + name, *frame, width, height);
    }
    ASSERT_FALSE(sky_off.empty());
    ASSERT_EQ(sky_on.size(), sky_off.size());
    ASSERT_EQ(card_off.size(), sky_off.size());
    ASSERT_EQ(card_on.size(), sky_off.size());

    // Pixels that were sky before the card and are the card after it: the same cloud over both backgrounds.
    size_t region = 0;
    size_t clouded = 0;
    size_t mismatched = 0;
    for (size_t pixel = 0; pixel < sky_off.size() / 4U; ++pixel) {
        const size_t base = pixel * 4U;
        const bool card = card_off[base] <= 2U && card_off[base + 1U] <= 2U && card_off[base + 2U] <= 2U;
        if (!is_disabled_sky(sky_off, pixel) || !card) { continue; }
        ++region;
        if (sky_on[base] >= 10U) { ++clouded; }
        const int red = std::abs(static_cast<int>(sky_on[base]) - static_cast<int>(card_on[base]));
        const int green = std::abs(static_cast<int>(sky_on[base + 1U]) - static_cast<int>(card_on[base + 1U]));
        if (std::max(red, green) > 3) { ++mismatched; }
    }
    GTEST_LOG_(INFO) << "sky-then-card region: " << region << " px, " << clouded << " clouded, " << mismatched
                     << " with red or green off by more than 3 levels between sky and card";

    ASSERT_GT(region, 2000U) << "the card does not cover enough former sky - check the framing";
    ASSERT_GT(static_cast<double>(clouded), 0.2 * static_cast<double>(region))
      << "the clouds in front of the card are too thin to measure";
    EXPECT_LT(static_cast<double>(mismatched), 0.01 * static_cast<double>(region))
      << "the same cloud shows a different red over the sky than over a black card: the sky composite is not the "
         "linear HDR premultiplied-over that geometry gets";
}

// The skybox faces as SkyBox loads them, averaged by the same function; nullopt without the textures.
std::optional<Kataglyphis::CloudAmbient> skybox_cubemap_ambient()
{
    const std::array<const char *, 6> names{ "RT.png", "LF.png", "UP.png", "DN.png", "FT.png", "BK.png" };
    std::array<unsigned char *, 6> pixels{};
    int width = 0;
    int height = 0;
    bool loaded = true;
    for (size_t face = 0; face < names.size(); ++face) {
        const std::string path = std::string("Resources/Textures/Skybox/kloofendal_43d_clear_puresky/") + names[face];
        int channels = 0;
        pixels[face] = stbi_load(path.c_str(), &width, &height, &channels, 4);
        loaded = loaded && pixels[face] != nullptr;
    }
    std::optional<Kataglyphis::CloudAmbient> ambient;
    if (loaded) {
        const std::array<const unsigned char *, 6> faces{
            pixels[0], pixels[1], pixels[2], pixels[3], pixels[4], pixels[5]
        };
        ambient =
          Kataglyphis::cloudAmbientOfCubemap(faces, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }
    for (unsigned char *face : pixels) { stbi_image_free(face); }
    return ambient;
}

// Sun off, a cloud shows its sky's ambient: it vanishes into the navy and takes the cube map's means, not the analytic sky.
TEST(GoldenRender, CloudsAreLitByTheSkyTheyDraw)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    // After the harness: faces freed into ASan's quarantine before it made LSan lose the window system's allocations.
    const auto cubemap = skybox_cubemap_ambient();
    ASSERT_TRUE(cubemap.has_value())
      << "could not load the skybox faces - not running from the repo root, or not fetched?";

    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.cloud_coverage_threshold = 0.0F;
    scene_vars.cloud_powder_effect = false;
    // The composite test's band of sky under the slab, dense enough that a few units through it are opaque.
    place_cloud_box(scene_vars, glm::vec3(0.0F, 4.0F, -40.0F), glm::vec3(40.0F, 4.0F, 5.0F), 4.0F);
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture = [&](bool skybox, bool clouds, float sun) {
        scene_vars.skybox_enabled = skybox;
        scene_vars.clouds_enabled = clouds;
        scene_vars.directional_light_radiance = sun;
        harness.render_frames(SETTLE_FRAMES);
        return harness.capture_frame(width, height);
    };
    // The sunlit cloud over the navy locates the box; every other capture is lit by the sky alone.
    const std::vector<uint8_t> navy_off = capture(false, false, 0.0F);
    const std::vector<uint8_t> navy_sunlit = capture(false, true, 10.0F);
    const std::vector<uint8_t> navy_on = capture(false, true, 0.0F);
    const std::vector<uint8_t> cubemap_off = capture(true, false, 0.0F);
    const std::vector<uint8_t> cubemap_on = capture(true, true, 0.0F);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    for (const auto &[name, frame] : { std::pair{ "ambient-navy-off", &navy_off },
           std::pair{ "ambient-navy-sunlit", &navy_sunlit },
           std::pair{ "ambient-navy-on", &navy_on },
           std::pair{ "ambient-cubemap-off", &cubemap_off },
           std::pair{ "ambient-cubemap-on", &cubemap_on } }) {
        dump_cloud_frame(name, *frame, width, height);
        ASSERT_EQ(frame->size(), navy_off.size()) << name;
    }
    ASSERT_FALSE(navy_off.empty());

    // Sky pixels the sunlit cloud covers, eroded twice so the box's thin, half-transparent rim drops out.
    std::vector<bool> cloud_mask(navy_off.size() / 4U);
    for (size_t pixel = 0; pixel < cloud_mask.size(); ++pixel) {
        cloud_mask[pixel] = is_disabled_sky(navy_off, pixel) && channel_delta(navy_off, navy_sunlit, pixel) > 5;
    }
    cloud_mask = eroded(eroded(cloud_mask, width, height), width, height);
    const size_t cloud_pixels = static_cast<size_t>(std::count(cloud_mask.begin(), cloud_mask.end(), true));
    ASSERT_GT(cloud_pixels, 5000U) << "the box covers too little sky - check the framing";

    // Under the navy the ambient is the sky's own radiance, so the cloud scatters exactly what it hides.
    size_t navy_mismatched = 0;
    // Under the cube map each opaque pixel lies between the two hemisphere means, by its entry height.
    std::array<int, 3> lowest{};
    std::array<int, 3> highest{};
    for (int c = 0; c < 3; ++c) {
        lowest[static_cast<size_t>(c)] = std::min(displayed_byte(cubemap->top[c]), displayed_byte(cubemap->bottom[c]));
        highest[static_cast<size_t>(c)] = std::max(displayed_byte(cubemap->top[c]), displayed_byte(cubemap->bottom[c]));
    }
    size_t cubemap_outside = 0;
    double cubemap_cloud_luma = 0.0;
    double cubemap_sky_luma = 0.0;
    for (size_t pixel = 0; pixel < cloud_mask.size(); ++pixel) {
        if (!cloud_mask[pixel]) { continue; }
        if (channel_delta(navy_off, navy_on, pixel) > 3) { ++navy_mismatched; }
        const size_t base = pixel * 4U;
        bool outside = false;
        for (size_t c = 0; c < 3U; ++c) {
            const int value = cubemap_on[base + c];
            outside = outside || value < lowest[c] - 3 || value > highest[c] + 3;
        }
        if (outside) { ++cubemap_outside; }
        cubemap_cloud_luma += luminance_of(cubemap_on, pixel);
        cubemap_sky_luma += luminance_of(cubemap_off, pixel);
    }
    const auto count = static_cast<double>(cloud_pixels);
    cubemap_cloud_luma /= count;
    cubemap_sky_luma /= count;
    GTEST_LOG_(INFO) << "sky-lit cloud over " << cloud_pixels << " px: navy " << navy_mismatched
                     << " px off by more than 3 levels; cube map top (" << displayed_byte(cubemap->top.x) << ", "
                     << displayed_byte(cubemap->top.y) << ", " << displayed_byte(cubemap->top.z) << "), bottom ("
                     << displayed_byte(cubemap->bottom.x) << ", " << displayed_byte(cubemap->bottom.y) << ", "
                     << displayed_byte(cubemap->bottom.z) << "), " << cubemap_outside
                     << " px outside them; mean luma cloud " << cubemap_cloud_luma << ", sky behind "
                     << cubemap_sky_luma;

    EXPECT_LT(static_cast<double>(navy_mismatched), 0.01 * count)
      << "with the skybox off a sky-lit cloud must vanish into the navy it is lit by; it is lit by another sky";
    EXPECT_LT(static_cast<double>(cubemap_outside), 0.05 * count)
      << "with the skybox on the cloud's colour is not the cube map hemisphere means the host computed";
    EXPECT_GT(cubemap_cloud_luma, 0.5 * cubemap_sky_luma) << "the sky-lit cloud is much darker than the sky behind it";
}

// The wind moves the noise by velocity times time; time 0 and a whole number of periods must not move it at all.
TEST(GoldenRender, CloudWindMovesTheNoiseAndRepeatsWithItsPeriod)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.clouds_enabled = true;
    scene_vars.cloud_coverage_threshold = 0.3F;
    place_cloud_box(scene_vars, glm::vec3(0.0F, 4.0F, -40.0F), glm::vec3(40.0F, 4.0F, 5.0F), 1.0F);
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture = [&](glm::vec3 wind, double time) {
        for (int axis = 0; axis < 3; ++axis) { scene_vars.cloud_wind[axis] = wind[axis]; }
        harness.settings.time_seconds = time;
        harness.render_frames(SETTLE_FRAMES);
        return harness.capture_frame(width, height);
    };
    // 512 s carries every axis a whole number of 256-unit periods: 1024, -2048 and 256 units.
    const glm::vec3 wind(2.0F, -4.0F, 0.5F);
    const std::vector<uint8_t> windless = capture(glm::vec3(0.0F), 0.0);
    const std::vector<uint8_t> at_zero = capture(wind, 0.0);
    const std::vector<uint8_t> at_three = capture(wind, 3.0);
    const std::vector<uint8_t> at_three_again = capture(wind, 3.0);
    const std::vector<uint8_t> whole_periods = capture(wind, 512.0);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(windless.empty());
    dump_cloud_frame("wind-windless", windless, width, height);
    dump_cloud_frame("wind-three", at_three, width, height);

    const auto differing = [&](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
        size_t count = 0;
        for (size_t pixel = 0; pixel < a.size() / 4U; ++pixel) {
            if (channel_delta(a, b, pixel) > 0) { ++count; }
        }
        return count;
    };
    const size_t zero_vs_windless = differing(at_zero, windless);
    const size_t three_vs_windless = differing(at_three, windless);
    const size_t three_vs_three = differing(at_three, at_three_again);
    const size_t periods_vs_windless = differing(whole_periods, windless);
    GTEST_LOG_(INFO) << "pixels differing: time 0 vs no wind " << zero_vs_windless << ", 3 s vs no wind "
                     << three_vs_windless << ", 3 s twice " << three_vs_three << ", 512 s vs no wind "
                     << periods_vs_windless << " of " << windless.size() / 4U;

    EXPECT_EQ(zero_vs_windless, 0U) << "at time 0 the wind moved the noise; a golden pinned there would drift";
    EXPECT_EQ(three_vs_three, 0U) << "the same time rendered a different cloud";
    EXPECT_EQ(periods_vs_windless, 0U) << "a whole number of noise periods moved the cloud: the wrap is off";
    EXPECT_GT(three_vs_windless, 1000U) << "3 s of wind (6, 244, 1.5 units) did not move the cloud";
}

// Half resolution marches a quarter of the pixels and fills the rest by depth, as OxidANT's switch does.
TEST(GoldenRender, HalfResolutionCloudsKeepOffTheSlabAndCloseToTheFullMarch)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    const ScopedValidationErrorCounter validation;

    EXPECT_TRUE(GUISceneSharedVars{}.cloud_half_resolution)
      << "half resolution must be on by default, as OxidANT's (owner 2026-11-16)";
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.skybox_enabled = false;
    scene_vars.cloud_coverage_threshold = 0.3F;
    // Behind the slab, as in CloudsStopAtTheSceneDepth: the sky around the slab clouds, the slab must not.
    place_cloud_box(scene_vars, glm::vec3(0.0F, 9.0F, -20.0F), glm::vec3(40.0F, 6.0F, 4.0F), 1.0F);
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture = [&](bool clouds, bool half) {
        scene_vars.clouds_enabled = clouds;
        scene_vars.cloud_half_resolution = half;
        harness.render_frames(SETTLE_FRAMES);
        return harness.capture_frame(width, height);
    };

    for (const RasterizationMode path : { RasterizationMode::Forward, RasterizationMode::Deferred }) {
        const char *name = path == RasterizationMode::Forward ? "forward" : "deferred";
        renderer_vars.rasterizationMode = path;
        const std::vector<uint8_t> off = capture(false, false);
        const std::vector<uint8_t> full = capture(true, false);
        const std::vector<uint8_t> half = capture(true, true);
        EXPECT_EQ(harness.renderer->renderStageRegistry().lastRecorded(),
          (std::vector<std::string>{ "shadow_cascades", "main", "clouds", "cloud_upsample", "post" }))
          << name;
        // The half target and its descriptors follow a swapchain recreation.
        harness.renderer->recreateSwapChain();
        const std::vector<uint8_t> recreated = capture(true, true);
        const std::vector<uint8_t> again = capture(true, false);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << name;
        ASSERT_FALSE(off.empty()) << name;
        for (const std::vector<uint8_t> *frame : { &full, &half, &recreated, &again }) {
            ASSERT_EQ(frame->size(), off.size()) << name;
        }
        dump_cloud_frame(std::string("half-") + name + "-full", full, width, height);
        dump_cloud_frame(std::string("half-") + name + "-half", half, width, height);

        const size_t pixels = off.size() / 4U;
        std::vector<bool> geometry(pixels);
        for (size_t pixel = 0; pixel < pixels; ++pixel) { geometry[pixel] = !is_disabled_sky(off, pixel); }
        // Sky within three pixels of the slab, where only the depth weight keeps out texels that marched to it.
        std::vector<bool> rim(pixels, false);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t pixel = static_cast<size_t>(y) * width + x;
                if (geometry[pixel]) { continue; }
                for (int dy = -3; dy <= 3 && !rim[pixel]; ++dy) {
                    for (int dx = -3; dx <= 3 && !rim[pixel]; ++dx) {
                        const int nx = static_cast<int>(x) + dx;
                        const int ny = static_cast<int>(y) + dy;
                        if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) || ny >= static_cast<int>(height)) {
                            continue;
                        }
                        rim[pixel] = geometry[static_cast<size_t>(ny) * width + static_cast<size_t>(nx)];
                    }
                }
            }
        }

        size_t geometry_pixels = 0;
        size_t geometry_clouded = 0;
        double rim_full = 0.0;
        double rim_half = 0.0;
        double delta_sum = 0.0;
        size_t over_eight = 0;
        size_t half_differing = 0;
        size_t recreated_differing = 0;
        size_t again_differing = 0;
        for (size_t pixel = 0; pixel < pixels; ++pixel) {
            if (geometry[pixel]) {
                ++geometry_pixels;
                if (channel_delta(off, half, pixel) > 2) { ++geometry_clouded; }
            }
            if (rim[pixel]) {
                rim_full += channel_delta(off, full, pixel);
                rim_half += channel_delta(off, half, pixel);
            }
            const int delta = channel_delta(full, half, pixel);
            delta_sum += delta;
            if (delta > 0) { ++half_differing; }
            if (delta > 8) { ++over_eight; }
            if (channel_delta(half, recreated, pixel) > 0) { ++recreated_differing; }
            if (channel_delta(full, again, pixel) > 0) { ++again_differing; }
        }
        const double mean_delta = delta_sum / static_cast<double>(pixels);
        const double rim_kept = rim_full > 0.0 ? rim_half / rim_full : 0.0;
        GTEST_LOG_(INFO) << name << ": half against full mean " << mean_delta << " levels, " << half_differing
                         << " px differ, " << over_eight << " px over 8 of " << pixels << "; " << geometry_clouded
                         << " of " << geometry_pixels << " slab px clouded; the rim kept " << rim_kept
                         << " of its cloud; recreated " << recreated_differing << " px, full again " << again_differing
                         << " px differ";

        ASSERT_GT(geometry_pixels, pixels / 10U) << name << ": the rig fills too little of the frame to judge";
        ASSERT_GT(rim_full, 0.0) << name << ": no cloud next to the slab - nothing for the upsample to keep";
        EXPECT_GT(half_differing, 0U) << name << ": the half frame equals the full one - did the half march run?";
        // A march to the texel's own pixel instead of its block's nearest depth clouded 6 px; the full march 0.
        EXPECT_EQ(geometry_clouded, 0U)
          << name << ": the half march drew cloud over the slab - it ignores the depth of the pixels it covers";
        EXPECT_GT(rim_kept, 0.95) << name << ": the sky beside the slab lost its cloud - the upsample mixes in texels "
                                  << "that marched to the slab";
        EXPECT_LT(mean_delta, 0.5) << name << ": the half march is far from the full one on average";
        EXPECT_LT(static_cast<double>(over_eight), 0.005 * static_cast<double>(pixels))
          << name << ": too many pixels are more than 8 levels from the full march";
        EXPECT_EQ(recreated_differing, 0U) << name << ": the half march changed across a swapchain recreation";
        EXPECT_EQ(again_differing, 0U) << name << ": switching half resolution off did not restore the full march";
    }
    EXPECT_EQ(validation.errorCount(), 0) << "a half-resolution frame logged an error";
}

// A box in front of the slab and an unlit card must cloud them in every mode; RT once put an unlit hit at the eye.
TEST(GoldenRender, CloudsInFrontOfGeometryCoverItInEveryMode)
{
    SKIP_WITHOUT_GPU();

    // Unlit, so its colour is the same in every mode and the path tracer adds no noise over it.
    const auto green_card =
      write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_green_unlit_card.gltf", [](auto &gltf) {
          auto &material = gltf["materials"][0];
          material["pbrMetallicRoughness"]["baseColorFactor"] = { 0.0, 1.0, 0.0, 1.0 };
          material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
          material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
          gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_materials_unlit" });
      });
    ASSERT_TRUE(green_card.has_value()) << "could not write the green unlit card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    const bool traced = harness.renderer->supportsHardwareRaytracing();

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.skybox_enabled = false;
    // Threshold 0 fills the box with fog, so every ray through it meets cloud.
    scene_vars.cloud_coverage_threshold = 0.0F;
    // z 4..8, between the camera (26) and the card (3), which stands one unit before the slab's face (2).
    place_cloud_box(scene_vars, glm::vec3(0.0F, 9.0F, 6.0F), glm::vec3(8.0F, 2.5F, 2.0F), 2.0F);
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    harness.render_frames(WARMUP_FRAMES);
    const glm::mat4 card_placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(2.0F, 9.0F, 3.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    ASSERT_TRUE(harness.renderer->addModel(green_card->string(), card_placement).has_value())
      << "adding the green card failed";

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture = [&](bool clouds) {
        scene_vars.clouds_enabled = clouds;
        harness.render_frames(SETTLE_FRAMES);
        return harness.capture_frame(width, height);
    };

    // Forward names the regions: the card by its green, the geometry the clouds cover by what they change.
    const std::vector<uint8_t> forward_off = capture(false);
    const std::vector<uint8_t> forward_on = capture(true);
    ASSERT_FALSE(forward_off.empty());
    ASSERT_EQ(forward_on.size(), forward_off.size());
    std::vector<bool> card_mask(forward_off.size() / 4U);
    std::vector<bool> covered_mask(forward_off.size() / 4U);
    for (size_t pixel = 0; pixel < card_mask.size(); ++pixel) {
        const size_t base = pixel * 4U;
        card_mask[pixel] = forward_off[base] < 40U && forward_off[base + 1U] > 200U && forward_off[base + 2U] < 40U;
        covered_mask[pixel] = !is_disabled_sky(forward_off, pixel) && !card_mask[pixel]
                              && channel_delta(forward_off, forward_on, pixel) > 5;
    }
    // Eroded, as RT's pixel centres and PT's jitter may disagree with raster on an edge.
    card_mask = eroded(card_mask, width, height);
    covered_mask = eroded(covered_mask, width, height);
    const size_t card_pixels = static_cast<size_t>(std::count(card_mask.begin(), card_mask.end(), true));
    const size_t covered_pixels = static_cast<size_t>(std::count(covered_mask.begin(), covered_mask.end(), true));
    ASSERT_GT(card_pixels, 1000U) << "the green card fills too little of the frame - check the placement";
    ASSERT_GT(covered_pixels, 2000U) << "forward shows no cloud over the lit geometry behind the box";

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
        bool pathTracing;
    };
    std::vector<Mode> modes = { Mode{ "forward", RasterizationMode::Forward, false, false },
        Mode{ "deferred", RasterizationMode::Deferred, false, false } };
    if (traced) {
        modes.push_back(Mode{ "ray traced", RasterizationMode::Forward, true, false });
        modes.push_back(Mode{ "path traced", RasterizationMode::Forward, false, true });
    }
    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = mode.pathTracing;
        const std::vector<uint8_t> off = capture(false);
        const std::vector<uint8_t> on = capture(true);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
        ASSERT_EQ(off.size(), forward_off.size()) << mode.name;
        ASSERT_EQ(on.size(), forward_off.size()) << mode.name;

        size_t card_clouded = 0;
        size_t covered_clouded = 0;
        for (size_t pixel = 0; pixel < card_mask.size(); ++pixel) {
            if (card_mask[pixel] && channel_delta(off, on, pixel) > 5) { ++card_clouded; }
            if (covered_mask[pixel] && channel_delta(off, on, pixel) > 5) { ++covered_clouded; }
        }
        GTEST_LOG_(INFO) << mode.name << ": clouds change " << card_clouded << " of " << card_pixels << " card px and "
                         << covered_clouded << " of " << covered_pixels << " lit px behind the box";

        EXPECT_GT(static_cast<double>(card_clouded), 0.9 * static_cast<double>(card_pixels))
          << mode.name << ": clouds in front of the unlit card vanish - its depth reaches the march too near";
        // The path tracer's lit pixels move by noise alone, so its depth is judged on the noise-free card.
        if (!mode.pathTracing) {
            EXPECT_GT(static_cast<double>(covered_clouded), 0.9 * static_cast<double>(covered_pixels))
              << mode.name << ": clouds in front of lit geometry vanish - the depth reaches the march too near";
        }
    }
}
