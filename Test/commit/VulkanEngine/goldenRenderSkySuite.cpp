// Goldens: the drawn sky, its ambient and IBL light, the HDR target, and post's encode and FXAA.

#include "GoldenRenderHarness.hpp"

import kataglyphis.vulkan.renderer;

using namespace Kataglyphis::TestSupport::GoldenRender;

namespace {

using Kataglyphis::Test::GoldenMetrics::post_display_level;

// Every texel finite and at most hdr_safe.slang's HDR_MAX, and the largest one, from an RGBA float capture.
struct HdrRange
{
    size_t non_finite = 0;
    float largest = 0.0F;
};

HdrRange hdr_range(const std::vector<float> &rgba)
{
    HdrRange range;
    for (size_t i = 0; i < rgba.size(); ++i) {
        if (i % 4U == 3U) { continue; }
        if (!std::isfinite(rgba[i])) {
            ++range.non_finite;
            continue;
        }
        range.largest = std::max(range.largest, rgba[i]);
    }
    return range;
}

}// namespace

// Ground pixels whose whole (2 band + 1)^2 window the prediction calls shadowed: no PCF texel or edge reaches them.
std::vector<bool> shadow_core(const std::vector<GroundPixel> &predicted, uint32_t w, uint32_t h, uint32_t band)
{
    const size_t stride = static_cast<size_t>(w) + 1U;
    std::vector<uint32_t> sum(stride * (h + 1U), 0U);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t at = (static_cast<size_t>(y) + 1U) * stride + x + 1U;
            const uint32_t hit = predicted[static_cast<size_t>(y) * w + x] == GroundPixel::Shadowed ? 1U : 0U;
            sum[at] = hit + sum[at - 1U] + sum[at - stride] - sum[at - stride - 1U];
        }
    }
    std::vector<bool> core(predicted.size(), false);
    const auto area = (2U * band + 1U) * (2U * band + 1U);
    for (uint32_t y = band; y + band < h; ++y) {
        for (uint32_t x = band; x + band < w; ++x) {
            const size_t x0 = x - band;
            const size_t y0 = y - band;
            const size_t x1 = static_cast<size_t>(x) + band + 1U;
            const size_t y1 = static_cast<size_t>(y) + band + 1U;
            core[static_cast<size_t>(y) * w + x] =
              sum[y1 * stride + x1] - sum[y0 * stride + x1] - sum[y1 * stride + x0] + sum[y0 * stride + x0] == area;
        }
    }
    return core;
}

// Without OxidANT's analytic_ambient shadowed ground was black; linear HDR reads the term as the shaders wrote it.
TEST(GoldenRender, TheSkyLightsShadowedGroundInEveryRasterAndRayTracedFrame)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = true;
    // The whole sun leaves the shadow, as the ray tracer's binary shadow ray takes it.
    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.camera->set_camera_position(glm::vec3(0.0F, 6.0F, 26.0F));
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
    };
    std::vector<Mode> modes = { Mode{ "forward", RasterizationMode::Forward, false },
        Mode{ "deferred", RasterizationMode::Deferred, false } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back(Mode{ "ray traced", RasterizationMode::Forward, true });
    }
    const float default_ambient = GUISceneSharedVars{}.directional_light_ambient;
    const std::array<float, 3> ambients{ 0.0F, default_ambient, kMaxAmbient };
    std::vector<glm::dvec3> at_default;
    for (const Mode &mode : modes) {
        SCOPED_TRACE(mode.name);
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        std::array<glm::dvec3, 3> means{};
        size_t core_pixels = 0;
        for (size_t i = 0; i < ambients.size(); ++i) {
            scene_vars.directional_light_ambient = ambients[i];
            harness.render_frames(SETTLE_FRAMES);
            uint32_t width = 0;
            uint32_t height = 0;
            const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
            ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
            ASSERT_EQ(radiance.size(), static_cast<size_t>(width) * height * 4U);
            const glm::vec3 travel = -glm::vec3(scene_vars.directional_light_direction[0],
              scene_vars.directional_light_direction[1],
              scene_vars.directional_light_direction[2]);
            const GroundShadowPrediction prediction =
              predict_ground_shadow(harness_inverse_view_projection(harness, width, height),
                width,
                height,
                SHADOW_RIG_GROUND,
                SHADOW_RIG_SLAB,
                travel);
            // Past the widest PCF footprint and a cascade texel either side of the analytic edge.
            constexpr uint32_t CORE_BAND_PX = 6U;
            const std::vector<bool> core = shadow_core(prediction.pixels, width, height, CORE_BAND_PX);
            glm::dvec3 sum(0.0);
            core_pixels = 0;
            for (size_t pixel = 0; pixel < core.size(); ++pixel) {
                if (!core[pixel]) { continue; }
                sum += glm::dvec3(radiance[pixel * 4U], radiance[pixel * 4U + 1U], radiance[pixel * 4U + 2U]);
                ++core_pixels;
            }
            ASSERT_GT(core_pixels, 2000U) << "Too little shadow in view; the test measures nothing.";
            means[i] = sum / static_cast<double>(core_pixels);
            GTEST_LOG_(INFO) << mode.name << ", ambient " << ambients[i] << ": " << core_pixels
                             << " shadowed ground px, mean radiance (" << means[i].r << ", " << means[i].g << ", "
                             << means[i].b << ")";
        }
        at_default.push_back(means[1]);

        // Without the sky the shadow holds no light at all: the sun is the only other light in the rig.
        EXPECT_LT(std::max({ means[0].r, means[0].g, means[0].b }), 1e-3) << "light in the shadow at ambient 0";
        // The irradiance of an upward face is the zenith's, blue (0.09, 0.16, 0.35): no grey constant passes.
        EXPECT_GT(means[1].b, 0.02) << "the default ambient leaves the shadow unlit";
        EXPECT_GT(means[1].b, 2.0 * means[1].r) << "the shadow's light is not the sky's blue";
        // The slider scales the term and nothing else: 2 / 0.35 in every channel, linear in the HDR target.
        const double expected_ratio = static_cast<double>(kMaxAmbient) / static_cast<double>(default_ambient);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(means[2][c] / means[1][c], expected_ratio, 0.02 * expected_ratio)
              << "channel " << c << " does not scale with the Ambient slider";
        }
    }
    // One shared function: the three shaders put the same sky into the same shadow.
    for (size_t m = 1; m < at_default.size(); ++m) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(at_default[m][c], at_default[0][c], 0.05 * at_default[0][c])
              << modes[m].name << " lights the shadow unlike forward, channel " << c;
        }
    }
}

// brdf_direct's GGX lobe carries the sun, so a sun disk in the ambient kept a mirror's glint inside the sun's shadow.
TEST(GoldenRender, AMirrorInTheSunsShadowReflectsTheSkyAndNoSun)
{
    SKIP_WITHOUT_GPU();

    const auto mirror = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_mirror_card.gltf", [](auto &gltf) {
        auto &material = gltf["materials"][0];
        material["pbrMetallicRoughness"] = {
            { "baseColorFactor", { 0.9, 0.9, 0.9, 1.0 } }, { "metallicFactor", 1.0 }, { "roughnessFactor", 0.05 }
        };
        material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
    });
    ASSERT_TRUE(mirror.has_value()) << "could not write the mirror card";
    // The analytic sky's bake: it has no sun disk, and its brightest radiance, the horizon's, bounds the reflection.
    const ScopedEnvVar analytic_sky("KATAGLYPHIS_SKYBOX_DIR", "Resources/Textures/Skybox/no-such-sky");

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = true;
    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.camera->set_camera_position(glm::vec3(0.0F, 6.0F, 26.0F));
    harness.render_frames(WARMUP_FRAMES);

    // Under the slab, which shadows it from the sun, turned so the eye sees the sun mirrored in its centre.
    const glm::vec3 centre(0.0F, 2.0F, -4.0F);
    const glm::vec3 to_sun = glm::normalize(glm::vec3(scene_vars.directional_light_direction[0],
      scene_vars.directional_light_direction[1],
      scene_vars.directional_light_direction[2]));
    const glm::vec3 mirror_normal =
      glm::normalize(to_sun + glm::normalize(harness.camera->get_camera_position() - centre));
    const glm::vec3 card_normal(0.0F, 0.0F, 1.0F);
    const glm::mat4 placement = glm::translate(glm::mat4(1.0F), centre)
                                * glm::rotate(glm::mat4(1.0F),
                                  std::acos(glm::dot(card_normal, mirror_normal)),
                                  glm::normalize(glm::cross(card_normal, mirror_normal)))
                                * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    ASSERT_TRUE(harness.renderer->addModel(mirror->string(), placement).has_value()) << "adding the mirror failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
    };
    std::vector<Mode> modes = { Mode{ "forward", RasterizationMode::Forward, false },
        Mode{ "deferred", RasterizationMode::Deferred, false } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back(Mode{ "ray traced", RasterizationMode::Forward, true });
    }
    const float brightest_sky = std::max(
      { Kataglyphis::kAnalyticSkyHorizon.r, Kataglyphis::kAnalyticSkyHorizon.g, Kataglyphis::kAnalyticSkyHorizon.b });
    for (const Mode &mode : modes) {
        SCOPED_TRACE(mode.name);
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        std::array<glm::dvec3, 2> means{};
        for (size_t i = 0; i < means.size(); ++i) {
            scene_vars.directional_light_ambient = i == 0U ? 0.0F : kMaxAmbient;
            harness.render_frames(SETTLE_FRAMES);
            uint32_t width = 0;
            uint32_t height = 0;
            const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
            ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
            ASSERT_EQ(radiance.size(), static_cast<size_t>(width) * height * 4U);
            const glm::vec4 clip =
              glm::inverse(harness_inverse_view_projection(harness, width, height)) * glm::vec4(centre, 1.0F);
            const auto px = static_cast<int>((clip.x / clip.w * 0.5F + 0.5F) * static_cast<float>(width));
            const auto py = static_cast<int>((clip.y / clip.w * 0.5F + 0.5F) * static_cast<float>(height));
            ASSERT_TRUE(px >= 3 && py >= 3 && px + 3 < static_cast<int>(width) && py + 3 < static_cast<int>(height))
              << "the mirror's centre projects to (" << px << ", " << py << "), outside the frame";
            glm::dvec3 sum(0.0);
            for (int y = py - 3; y <= py + 3; ++y) {
                for (int x = px - 3; x <= px + 3; ++x) {
                    const float *texel = &radiance[(static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4U];
                    sum += glm::dvec3(texel[0], texel[1], texel[2]);
                }
            }
            means[i] = sum / 49.0;
            GTEST_LOG_(INFO) << mode.name << ", ambient " << scene_vars.directional_light_ambient << ": mirror at ("
                             << px << ", " << py << "), mean radiance (" << means[i].r << ", " << means[i].g << ", "
                             << means[i].b << ")";
        }
        EXPECT_LT(std::max({ means[0].r, means[0].g, means[0].b }), 1e-3)
          << "the mirror is lit at ambient 0: the slab no longer shadows it, so the test measures nothing";
        // Per unit of Ambient a surface returns at most the sky it reflects: the brightest radiance is the horizon's.
        const glm::dvec3 gain = (means[1] - means[0]) / static_cast<double>(kMaxAmbient);
        for (int c = 0; c < 3; ++c) {
            EXPECT_LE(gain[c], 1.02 * static_cast<double>(brightest_sky))
              << "channel " << c << ": the ambient put the sun into its shadow";
        }
        EXPECT_GT(gain.b, 0.2) << "the mirror reflects no sky";
    }
}

// One texel (200, 30, 30) as PNG bytes: a uniformly red skybox face, whose bake must light a card red.
constexpr std::string_view RED_FACE_PNG{
    "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A\x00\x00\x00\x0D\x49\x48\x44\x52\x00\x00\x00\x01\x00\x00\x00"
    "\x01\x08\x02\x00\x00\x00\x90\x77\x53\xDE\x00\x00\x00\x0C\x49\x44\x41\x54\x78\xDA\x63\x38\x21"
    "\x27\x07\x00\x02\xB6\x01\x05\x0A\x5B\xA6\x06\x00\x00\x00\x00\x49\x45\x4E\x44\xAE\x42\x60\x82",
    69
};

// Six faces of RED_FACE_PNG in a per-process temp folder for KATAGLYPHIS_SKYBOX_DIR, removed with the handle.
class RedSkybox
{
  public:
    RedSkybox()
    {
        std::error_code error;
        dir_ = std::filesystem::temp_directory_path(error)
               / ("kataglyphis-red-sky-" + std::to_string(Kataglyphis::TestSupport::GltfVariant::processId()));
        if (error) { return; }
        std::filesystem::create_directories(dir_, error);
        for (const char *face : { "RT.png", "LF.png", "UP.png", "DN.png", "FT.png", "BK.png" }) {
            std::ofstream out(dir_ / face, std::ios::binary | std::ios::trunc);
            out.write(RED_FACE_PNG.data(), static_cast<std::streamsize>(RED_FACE_PNG.size()));
            written_ = written_ && static_cast<bool>(out);
        }
    }
    RedSkybox(const RedSkybox &) = delete;
    RedSkybox &operator=(const RedSkybox &) = delete;
    ~RedSkybox()
    {
        std::error_code ignored;
        std::filesystem::remove_all(dir_, ignored);
    }

    [[nodiscard]] bool written() const { return written_ && !dir_.empty(); }
    [[nodiscard]] std::string string() const { return dir_.string(); }

  private:
    std::filesystem::path dir_;
    bool written_{ true };
};

// The mean HDR radiance of the 7x7 pixels around where world_point projects; nullopt when it falls off the frame.
std::optional<glm::dvec3> mean_radiance_at(EngineHarness &harness,
  const std::vector<float> &radiance,
  uint32_t width,
  uint32_t height,
  const glm::vec3 &world_point)
{
    const glm::vec4 clip =
      glm::inverse(harness_inverse_view_projection(harness, width, height)) * glm::vec4(world_point, 1.0F);
    const auto px = static_cast<int>((clip.x / clip.w * 0.5F + 0.5F) * static_cast<float>(width));
    const auto py = static_cast<int>((clip.y / clip.w * 0.5F + 0.5F) * static_cast<float>(height));
    if (px < 3 || py < 3 || px + 3 >= static_cast<int>(width) || py + 3 >= static_cast<int>(height)) {
        return std::nullopt;
    }
    glm::dvec3 sum(0.0);
    for (int y = py - 3; y <= py + 3; ++y) {
        for (int x = px - 3; x <= px + 3; ++x) {
            const float *texel = &radiance[(static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4U];
            sum += glm::dvec3(texel[0], texel[1], texel[2]);
        }
    }
    return sum / 49.0;
}

// A white, fully rough dielectric card: what reaches the eye from it is its irradiance, nothing glossy.
std::optional<Kataglyphis::TestSupport::GltfVariant> write_white_card(const char *file_name,
  const std::function<void(nlohmann::json &)> &extra = {})
{
    return write_gltf_variant(EMISSIVE_CARD_MODEL, file_name, [&extra](auto &gltf) {
        auto &material = gltf["materials"][0];
        material["pbrMetallicRoughness"] = {
            { "baseColorFactor", { 0.9, 0.9, 0.9, 1.0 } }, { "metallicFactor", 0.0 }, { "roughnessFactor", 1.0 }
        };
        material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
        if (extra) { extra(gltf); }
    });
}

struct LitMode
{
    const char *name;
    RasterizationMode raster;
    bool raytracing;
};

std::vector<LitMode> raster_and_ray_traced_modes(EngineHarness &harness)
{
    std::vector<LitMode> modes = { LitMode{ "forward", RasterizationMode::Forward, false },
        LitMode{ "deferred", RasterizationMode::Deferred, false } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back(LitMode{ "ray traced", RasterizationMode::Forward, true });
    }
    return modes;
}

// The skybox's bake lights the scene, as the Rust renderer's does: a red sky lights a white card red in every mode.
TEST(GoldenRender, TheDrawnSkyboxLightsACardThroughItsBakeInEveryMode)
{
    SKIP_WITHOUT_GPU();

    const RedSkybox red_sky;
    ASSERT_TRUE(red_sky.written()) << "could not write the red skybox";
    const ScopedEnvVar use_red_sky("KATAGLYPHIS_SKYBOX_DIR", red_sky.string().c_str());
    const auto card = write_white_card("kataglyphis_sky_lit_card.gltf");
    ASSERT_TRUE(card.has_value()) << "could not write the white card";

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    ASSERT_FALSE(harness.renderer->skyIsAnalytic()) << "the red faces did not load, so the test measures nothing";
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    // The sky alone: no sun, so the card shows what the ambient term gives it.
    scene_vars.directional_light_radiance = 0.0F;
    scene_vars.shadows_enabled = false;
    harness.camera->set_camera_position(glm::vec3(0.0F, 6.0F, 26.0F));
    const glm::vec3 centre(0.0F, 5.0F, 18.0F);
    const glm::mat4 placement = glm::translate(glm::mat4(1.0F), centre) * glm::scale(glm::mat4(1.0F), glm::vec3(3.0F));
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_TRUE(harness.renderer->addModel(card->string(), placement).has_value()) << "adding the card failed";

    const std::vector<LitMode> modes = raster_and_ray_traced_modes(harness);
    std::array<std::vector<glm::dvec3>, 2> lit{};
    for (size_t sky = 0; sky < 2; ++sky) {
        // Shown, the skybox's bake lights the scene; switched off, the analytic sky does.
        scene_vars.skybox_enabled = sky == 0U;
        for (const LitMode &mode : modes) {
            SCOPED_TRACE(mode.name);
            renderer_vars.rasterizationMode = mode.raster;
            renderer_vars.raytracing = mode.raytracing;
            harness.render_frames(SETTLE_FRAMES);
            uint32_t width = 0;
            uint32_t height = 0;
            const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
            ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
            ASSERT_EQ(radiance.size(), static_cast<size_t>(width) * height * 4U);
            const auto mean = mean_radiance_at(harness, radiance, width, height, centre);
            ASSERT_TRUE(mean.has_value()) << "the card's centre projects outside the frame";
            GTEST_LOG_(INFO) << mode.name << (sky == 0U ? ", red skybox" : ", skybox off") << ": card (" << mean->r
                             << ", " << mean->g << ", " << mean->b << ")";
            lit[sky].push_back(*mean);
        }
    }
    for (size_t m = 0; m < modes.size(); ++m) {
        SCOPED_TRACE(modes[m].name);
        const glm::dvec3 red = lit[0][m];
        EXPECT_GT(red.r, 0.05) << "the red sky leaves the card unlit";
        EXPECT_GT(red.r, 5.0 * red.g) << "the card's light is not the red sky's";
        EXPECT_GT(red.r, 5.0 * red.b) << "the card's light is not the red sky's";
        const glm::dvec3 analytic = lit[1][m];
        EXPECT_GT(analytic.b, analytic.r) << "with the skybox off the card should take the analytic sky's blue";
        // One bake, one shared function: every mode puts the same light on the card.
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(red[c], lit[0][0][c], 0.05 * lit[0][0][c] + 1e-4) << "channel " << c << " unlike forward";
        }
    }
}

// The bake leaves the photo's sun to the directional Sun: a mirror facing it reflects the sky around the disk.
TEST(GoldenRender, AMirrorFacingThePhotoSunReflectsTheSkyAroundIt)
{
    SKIP_WITHOUT_GPU();

    // The default photo sky's sun, measured from its faces: the centroid of the saturated texels around it.
    const glm::vec3 PHOTO_SKY_SUN = glm::normalize(glm::vec3(0.6248F, 0.6769F, 0.3891F));

    const auto mirror = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_photo_sun_mirror.gltf", [](auto &gltf) {
        auto &material = gltf["materials"][0];
        material["pbrMetallicRoughness"] = {
            { "baseColorFactor", { 0.9, 0.9, 0.9, 1.0 } }, { "metallicFactor", 1.0 }, { "roughnessFactor", 0.05 }
        };
        material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
    });
    ASSERT_TRUE(mirror.has_value()) << "could not write the mirror card";

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    ASSERT_FALSE(harness.renderer->skyIsAnalytic())
      << "the photo sky did not load - not fetched, or not the repo root?";
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    // The sky alone lights the mirror: a sun spike in it can only come from the bake.
    scene_vars.directional_light_radiance = 0.0F;
    scene_vars.directional_light_ambient = 1.0F;
    scene_vars.shadows_enabled = false;
    const glm::vec3 eye(0.0F, 6.0F, 26.0F);
    harness.camera->set_camera_position(eye);
    const auto look_along = [&harness](const glm::vec3 &dir) {
        harness.camera->set_orientation(glm::degrees(std::atan2(dir.z, dir.x)), glm::degrees(std::asin(dir.y)));
    };
    look_along(PHOTO_SKY_SUN);
    harness.render_frames(WARMUP_FRAMES);

    // The sky pass still draws the sun's saturated disk; the ring at 11-14 degrees is the sky around it.
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<float> sky = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost drawing the sky.";
    ASSERT_EQ(sky.size(), static_cast<size_t>(width) * height * 4U);
    const glm::mat4 clip_to_world = harness_inverse_view_projection(harness, width, height);
    glm::dvec3 disk(0.0);
    glm::dvec3 ring(0.0);
    size_t disk_pixels = 0;
    size_t ring_pixels = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const glm::vec2 ndc((static_cast<float>(x) + 0.5F) / static_cast<float>(width) * 2.0F - 1.0F,
              (static_cast<float>(y) + 0.5F) / static_cast<float>(height) * 2.0F - 1.0F);
            const glm::vec4 point = clip_to_world * glm::vec4(ndc, 0.5F, 1.0F);
            const float degrees = glm::degrees(std::acos(
              std::clamp(glm::dot(glm::normalize(glm::vec3(point) / point.w - eye), PHOTO_SKY_SUN), -1.0F, 1.0F)));
            const float *texel = &sky[(static_cast<size_t>(y) * width + x) * 4U];
            const glm::dvec3 rgb(texel[0], texel[1], texel[2]);
            if (degrees < 2.0F) {
                disk += rgb;
                ++disk_pixels;
            } else if (degrees >= 11.0F && degrees <= 14.0F) {
                ring += rgb;
                ++ring_pixels;
            }
        }
    }
    ASSERT_GT(disk_pixels, 10U) << "the sun's disk is off the frame";
    ASSERT_GT(ring_pixels, 100U) << "the ring around the sun is off the frame";
    disk /= static_cast<double>(disk_pixels);
    ring /= static_cast<double>(ring_pixels);
    const auto luminance = [](const glm::dvec3 &c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; };
    GTEST_LOG_(INFO) << "drawn photo sun (" << disk.r << ", " << disk.g << ", " << disk.b << "), the ring around it ("
                     << ring.r << ", " << ring.g << ", " << ring.b << ") luminance " << luminance(ring);
    EXPECT_GT(std::min({ disk.r, disk.g, disk.b }), 5.0)
      << "the sky pass no longer draws a saturated sun at PHOTO_SKY_SUN: the faces changed, or the mask reached it";

    // Turned so the eye sees the sun mirrored in its centre.
    const glm::vec3 centre(0.0F, 5.0F, 18.0F);
    const glm::vec3 mirror_normal = glm::normalize(PHOTO_SKY_SUN + glm::normalize(eye - centre));
    const glm::vec3 card_normal(0.0F, 0.0F, 1.0F);
    const glm::mat4 placement = glm::translate(glm::mat4(1.0F), centre)
                                * glm::rotate(glm::mat4(1.0F),
                                  std::acos(glm::dot(card_normal, mirror_normal)),
                                  glm::normalize(glm::cross(card_normal, mirror_normal)))
                                * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    ASSERT_TRUE(harness.renderer->addModel(mirror->string(), placement).has_value()) << "adding the mirror failed";
    look_along(glm::normalize(centre - eye));

    std::vector<LitMode> modes = raster_and_ray_traced_modes(harness);
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back(LitMode{ "path traced", RasterizationMode::Forward, false });
    }
    for (const LitMode &mode : modes) {
        SCOPED_TRACE(mode.name);
        const bool traced = std::string_view(mode.name) == "path traced";
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = traced;
        renderer_vars.pathTracingSamplesPerPixel = 1;
        renderer_vars.pathTracingMaxBounces = 2;
        // The mirror's GGX lobe is narrow, so four one-sample frames settle its mean.
        harness.render_frames(traced ? 4 : SETTLE_FRAMES);
        const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
        ASSERT_EQ(radiance.size(), static_cast<size_t>(width) * height * 4U);
        const auto mean = mean_radiance_at(harness, radiance, width, height, centre);
        ASSERT_TRUE(mean.has_value()) << "the mirror's centre projects outside the frame";
        GTEST_LOG_(INFO) << mode.name << ": the mirror facing the photo sun (" << mean->r << ", " << mean->g << ", "
                         << mean->b << ") luminance " << luminance(*mean) << ", " << luminance(*mean) / luminance(ring)
                         << " times the ring's";
        // Its saturated disk mirrored is about 5 times the ring: the bake would light the scene with the sun twice.
        EXPECT_LT(luminance(*mean), 2.0 * luminance(ring)) << "the mirror shows the photo's sun: the bake kept it";
        EXPECT_GT(luminance(*mean), 0.3 * luminance(ring)) << "the mirror reflects no sky";
    }
}

// A bake that bails, as after a failed skybox upload, leaves the maps sampleable and the scene on the analytic sky.
TEST(GoldenRender, AFailedAmbientBakeLeavesSampleableMapsAndTheAnalyticSky)
{
    SKIP_WITHOUT_GPU();

    const RedSkybox red_sky;
    ASSERT_TRUE(red_sky.written()) << "could not write the red skybox";
    const ScopedEnvVar use_red_sky("KATAGLYPHIS_SKYBOX_DIR", red_sky.string().c_str());
    const auto card = write_white_card("kataglyphis_unbaked_card.gltf");
    ASSERT_TRUE(card.has_value()) << "could not write the white card";

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    ASSERT_FALSE(harness.renderer->skyIsAnalytic()) << "the red faces did not load, so the test measures nothing";
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.directional_light_radiance = 0.0F;
    scene_vars.shadows_enabled = false;
    harness.camera->set_camera_position(glm::vec3(0.0F, 6.0F, 26.0F));
    const glm::vec3 centre(0.0F, 5.0F, 18.0F);
    const glm::mat4 placement = glm::translate(glm::mat4(1.0F), centre) * glm::scale(glm::mat4(1.0F), glm::vec3(3.0F));
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_TRUE(harness.renderer->addModel(card->string(), placement).has_value()) << "adding the card failed";

    // After the harness: the arm64 runner's PowerVR ICD logs loader errors while devices are enumerated.
    ScopedValidationErrorCounter validation_errors;
    harness.renderer->simulateFailedAmbientBakeForTesting();
    for (const LitMode &mode : raster_and_ray_traced_modes(harness)) {
        SCOPED_TRACE(mode.name);
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost drawing over unbaked maps.";
        ASSERT_EQ(radiance.size(), static_cast<size_t>(width) * height * 4U);
        const auto mean = mean_radiance_at(harness, radiance, width, height, centre);
        ASSERT_TRUE(mean.has_value()) << "the card's centre projects outside the frame";
        GTEST_LOG_(INFO) << mode.name << ", unbaked under the red skybox: card (" << mean->r << ", " << mean->g << ", "
                         << mean->b << ")";
        EXPECT_GT(mean->b, 0.05) << "the card is unlit: the zeroed maps lit it instead of the analytic sky";
        EXPECT_GT(mean->b, mean->r) << "the card takes the red sky: an unbaked map lit it";
    }

    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "drawing over unbaked maps logged a validation error - VUID-vkCmdDraw-None-09600 if set 0 bound a map "
         "that nothing moved out of UNDEFINED (AmbientMaps.cpp)";
}

// glTF scopes occlusionTexture to indirect light: it darkens the sky's share of a card and leaves the sun's alone.
TEST(GoldenRender, TheOcclusionMapDarkensTheSkysLightAndNotTheSuns)
{
    SKIP_WITHOUT_GPU();

    const auto plain = write_white_card("kataglyphis_plain_card.gltf");
    // A black occlusion texel at strength 0.5: lerp(1, 0, 0.5) leaves half the sky.
    const auto occluded = write_white_card("kataglyphis_occluded_card.gltf", [](auto &gltf) {
        gltf["images"] = nlohmann::json::array({ { { "uri", ZERO_ROUGHNESS_PNG } } });
        gltf["textures"] = nlohmann::json::array({ { { "source", 0 } } });
        gltf["materials"][0]["occlusionTexture"] = { { "index", 0 }, { "strength", 0.5 } };
    });
    ASSERT_TRUE(plain.has_value() && occluded.has_value()) << "could not write the cards";

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    harness.camera->set_camera_position(glm::vec3(0.0F, 6.0F, 26.0F));
    const glm::vec3 left(-1.8F, 5.0F, 18.0F);
    const glm::vec3 right(1.8F, 5.0F, 18.0F);
    const auto place = [](const glm::vec3 &at) {
        return glm::translate(glm::mat4(1.0F), at) * glm::scale(glm::mat4(1.0F), glm::vec3(3.0F));
    };
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_TRUE(harness.renderer->addModel(plain->string(), place(left)).has_value()) << "adding the plain card failed";
    ASSERT_TRUE(harness.renderer->addModel(occluded->string(), place(right)).has_value())
      << "adding the occluded card failed";

    const float sun_radiance = GUISceneSharedVars{}.directional_light_radiance;
    for (const LitMode &mode : raster_and_ray_traced_modes(harness)) {
        SCOPED_TRACE(mode.name);
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        // 0: the sky alone; 1: the sun alone.
        std::array<std::array<glm::dvec3, 2>, 2> cards{};
        for (size_t light = 0; light < 2; ++light) {
            scene_vars.directional_light_radiance = light == 0U ? 0.0F : sun_radiance;
            scene_vars.directional_light_ambient = light == 0U ? kMaxAmbient : 0.0F;
            harness.render_frames(SETTLE_FRAMES);
            uint32_t width = 0;
            uint32_t height = 0;
            const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
            ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
            ASSERT_EQ(radiance.size(), static_cast<size_t>(width) * height * 4U);
            const auto plain_mean = mean_radiance_at(harness, radiance, width, height, left);
            const auto occluded_mean = mean_radiance_at(harness, radiance, width, height, right);
            ASSERT_TRUE(plain_mean.has_value() && occluded_mean.has_value()) << "a card projects outside the frame";
            cards[light] = { *plain_mean, *occluded_mean };
            GTEST_LOG_(INFO) << mode.name << (light == 0U ? ", sky only" : ", sun only") << ": plain (" << plain_mean->r
                             << ", " << plain_mean->g << ", " << plain_mean->b << "), occluded (" << occluded_mean->r
                             << ", " << occluded_mean->g << ", " << occluded_mean->b << ")";
        }
        for (int c = 0; c < 3; ++c) {
            ASSERT_GT(cards[0][0][c], 1e-3) << "the sky leaves the plain card unlit, so the test measures nothing";
            EXPECT_NEAR(cards[0][1][c] / cards[0][0][c], 0.5, 0.03) << "channel " << c << ": the sky's share";
            ASSERT_GT(cards[1][0][c], 1e-3) << "the sun leaves the plain card unlit, so the test measures nothing";
            EXPECT_NEAR(cards[1][1][c] / cards[1][0][c], 1.0, 0.01)
              << "channel " << c << ": the occlusion map darkened the sun";
        }
    }
}

// Post's whole chain against its formula, texel by texel of one frame: the HDR capture is exactly what post read.
TEST(GoldenRender, PostEncodesEachHdrTexelThroughAcesAndSrgb)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // A dim and the default exposure of one view: the dim one fills the toe and the middle, the default the shoulder.
    const std::array<float, 2> radiances{ 0.1F * scene_vars.directional_light_radiance,
        scene_vars.directional_light_radiance };
    size_t dark = 0;
    size_t mid_grey = 0;
    size_t shoulder = 0;
    double mid_grey_level_sum = 0.0;
    double mid_grey_formula_sum = 0.0;
    for (const float radiance : radiances) {
        SCOPED_TRACE("directional radiance " + std::to_string(radiance));
        scene_vars.directional_light_radiance = radiance;
        harness.render_frames(SETTLE_FRAMES);

        harness.renderer->requestFrameCapture();
        harness.renderer->requestHdrCapture();
        harness.render_frame();
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t hdr_width = 0;
        uint32_t hdr_height = 0;
        const std::vector<uint8_t> display = harness.renderer->takeCapturedFrame(width, height);
        const std::vector<uint8_t> raw_hdr = harness.renderer->takeCapturedHdrFrame(hdr_width, hdr_height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
        ASSERT_EQ(width, hdr_width);
        ASSERT_EQ(height, hdr_height);
        ASSERT_EQ(display.size(), static_cast<size_t>(width) * height * 4U);
        ASSERT_EQ(raw_hdr.size(), display.size() * 2U);

        // Every texel, the sky's included: it is radiance in the HDR target like the scene, at alpha 1.
        size_t opaque = 0;
        size_t matching = 0;
        int worst_delta = 0;
        for (size_t pixel = 0; pixel < display.size() / 4U; ++pixel) {
            const auto half_at = [&](size_t channel) {
                const size_t at = (pixel * 4U + channel) * 2U;
                return static_cast<double>(
                  glm::unpackHalf1x16(static_cast<uint16_t>(raw_hdr[at] | (raw_hdr[at + 1U] << 8U))));
            };
            if (half_at(3U) < 0.999) { continue; }
            ++opaque;
            int pixel_delta = 0;
            for (size_t channel = 0; channel < 3U; ++channel) {
                const double linear = half_at(channel);
                const double expected = post_display_level(linear);
                pixel_delta = std::max(pixel_delta,
                  static_cast<int>(
                    std::lround(std::abs(expected - static_cast<double>(display[pixel * 4U + channel])))));
                if (channel != 1U) { continue; }
                if (linear < 0.05) { ++dark; }
                if (linear > 1.0) { ++shoulder; }
                // Half a stop each side of 0.18, graded against the formula's mean over the same texels.
                if (linear > 0.18 / std::numbers::sqrt2 && linear < 0.18 * std::numbers::sqrt2) {
                    ++mid_grey;
                    mid_grey_level_sum += static_cast<double>(display[pixel * 4U + channel]);
                    mid_grey_formula_sum += expected;
                }
            }
            worst_delta = std::max(worst_delta, pixel_delta);
            // One level for the 8-bit rounding, one for the GPU's pow and division.
            if (pixel_delta <= 2) { ++matching; }
        }
        const double match_fraction = opaque > 0U ? static_cast<double>(matching) / static_cast<double>(opaque) : 0.0;
        GTEST_LOG_(INFO) << opaque << " opaque texels, " << match_fraction * 100.0 << " % within 2 levels (worst "
                         << worst_delta << ")";
        ASSERT_EQ(opaque, display.size() / 4U) << "Transparent HDR texels: the sky left part of the target unfilled.";
        EXPECT_GT(match_fraction, 0.999) << "Post's output is not ACES then sRGB of what it read.";
    }

    // Both frames together must exercise the toe, the middle and the shoulder, or a match proves one segment only.
    const double mid_grey_level = mid_grey > 0U ? mid_grey_level_sum / static_cast<double>(mid_grey) : 0.0;
    const double mid_grey_formula = mid_grey > 0U ? mid_grey_formula_sum / static_cast<double>(mid_grey) : 0.0;
    GTEST_LOG_(INFO) << "green texels: below 0.05 " << dark << ", within half a stop of 0.18 " << mid_grey
                     << " (mean level " << mid_grey_level << ", formula " << mid_grey_formula << "), above 1.0 "
                     << shoulder;
    EXPECT_GT(dark, 1000U) << "No dark texels: the toe goes unchecked.";
    EXPECT_GT(mid_grey, 1000U) << "No texels near 0.18: the middle goes unchecked.";
    EXPECT_GT(shoulder, 1000U) << "No texels above 1.0: the shoulder goes unchecked.";
    EXPECT_NEAR(mid_grey_level, mid_grey_formula, 2.0)
      << "Linear 0.18 should display at level 141: 46 is no tonemap and no encode, 68 no encode, 118 no tonemap.";
}

// FXAA after the tonemap: a slanted card's one-sample staircase gets blended steps, a flat region keeps its pixels.
TEST(GoldenRender, FxaaSmoothsTheRasterStaircaseAndLeavesFlatRegionsAlone)
{
    SKIP_WITHOUT_GPU();

    nlohmann::json white;
    white["pbrMetallicRoughness"] = { { "baseColorFactor", { 1.0, 1.0, 1.0, 1.0 } } };
    white["doubleSided"] = true;
    white["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
    // Turned 20 degrees about the view axis, so every edge is a staircase of one-pixel steps.
    const double half_turn = glm::radians(10.0);
    const auto scene = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_fxaa_card.gltf", [&](auto &gltf) {
        make_card_scene(
          gltf, { white }, { { 0, { { "rotation", { 0.0, 0.0, std::sin(half_turn), std::cos(half_turn) } } } } });
        gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_materials_unlit" });
    });
    ASSERT_TRUE(scene.has_value()) << "could not write the slanted card";

    ScopedModelOverride card(scene->string().c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    // White on the flat navy: two colours, so every pixel between them is a blend FXAA made.
    scene_vars.skybox_enabled = false;
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, 1.8F));
    harness.camera->set_orientation(-90.0F, 0.0F);
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture = [&](FxaaMode mode) {
        renderer_vars.fxaaMode = mode;
        harness.render_frames(SETTLE_FRAMES);
        return harness.capture_frame(width, height);
    };
    const std::vector<uint8_t> off = capture(FxaaMode::Off);
    const std::vector<std::string> off_stages = harness.renderer->renderStageRegistry().lastRecorded();
    const std::vector<uint8_t> always = capture(FxaaMode::Always);
    const std::vector<std::string> always_stages = harness.renderer->renderStageRegistry().lastRecorded();
    const std::vector<uint8_t> raster = capture(FxaaMode::Raster);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(off.empty());
    ASSERT_EQ(always.size(), off.size());
    EXPECT_EQ(std::count(off_stages.begin(), off_stages.end(), "fxaa"), 0) << "FXAA off still recorded its stage";
    ASSERT_GE(always_stages.size(), 2U);
    EXPECT_EQ(always_stages[always_stages.size() - 2U], "post");
    EXPECT_EQ(always_stages.back(), "fxaa") << "FXAA must be the last stage, after post";
    EXPECT_EQ(raster, always) << "Raster mode must filter a raster frame as Always does";

    // An edge pixel: a step of more than 4 levels to a 4-neighbour in the unfiltered frame.
    const auto level = [&](const std::vector<uint8_t> &frame, uint32_t x, uint32_t y) {
        return luminance_of(frame, static_cast<size_t>(y) * width + x);
    };
    std::vector<bool> edge(off.size() / 4U, false);
    for (uint32_t y = 1; y + 1U < height; ++y) {
        for (uint32_t x = 1; x + 1U < width; ++x) {
            const double centre = level(off, x, y);
            edge[static_cast<size_t>(y) * width + x] =
              std::abs(centre - level(off, x + 1U, y)) > 4.0 || std::abs(centre - level(off, x - 1U, y)) > 4.0
              || std::abs(centre - level(off, x, y + 1U)) > 4.0 || std::abs(centre - level(off, x, y - 1U)) > 4.0;
        }
    }
    // The card and the sky are the only two colours; a blended step lies between them.
    double card_level = 0.0;
    double sky_level = 255.0;
    for (size_t pixel = 0; pixel < edge.size(); ++pixel) {
        card_level = std::max(card_level, luminance_of(off, pixel));
        sky_level = std::min(sky_level, luminance_of(off, pixel));
    }
    const auto between = [&](const std::vector<uint8_t> &frame, size_t pixel) {
        const double value = luminance_of(frame, pixel);
        return value > sky_level + 10.0 && value < card_level - 10.0;
    };
    size_t edges = 0;
    size_t off_blended = 0;
    size_t fxaa_blended = 0;
    size_t flat = 0;
    size_t flat_moved = 0;
    for (uint32_t y = 3; y + 3U < height; ++y) {
        for (uint32_t x = 3; x + 3U < width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * width + x;
            if (edge[pixel]) {
                ++edges;
                if (between(off, pixel)) { ++off_blended; }
                if (between(always, pixel)) { ++fxaa_blended; }
                continue;
            }
            bool near_edge = false;
            for (int dy = -2; dy <= 2 && !near_edge; ++dy) {
                for (int dx = -2; dx <= 2 && !near_edge; ++dx) {
                    near_edge = edge[static_cast<size_t>(static_cast<int>(y) + dy) * width
                                     + static_cast<size_t>(static_cast<int>(x) + dx)];
                }
            }
            if (near_edge) { continue; }
            ++flat;
            if (channel_delta(off, always, pixel) > 1) { ++flat_moved; }
        }
    }
    GTEST_LOG_(INFO) << "card " << card_level << ", sky " << sky_level << "; " << edges << " edge px: blended "
                     << off_blended << " without FXAA, " << fxaa_blended << " with; " << flat_moved << " of " << flat
                     << " flat px moved";
    ASSERT_GT(edges, 1000U) << "the card shows too little edge - check the placement";
    // The control: without FXAA the staircase has no step between card and sky to find.
    EXPECT_LT(static_cast<double>(off_blended), 0.01 * static_cast<double>(edges))
      << "the unfiltered frame already blends its edges, so the measure cannot see FXAA";
    EXPECT_GT(static_cast<double>(fxaa_blended), 0.3 * static_cast<double>(edges))
      << "FXAA did not soften the staircase";
    EXPECT_EQ(flat_moved, 0U) << "FXAA moved pixels away from every edge";

    if (harness.renderer->supportsHardwareRaytracing()) {
        // A traced frame is no raster frame: Raster leaves it as Off does.
        renderer_vars.raytracing = true;
        const std::vector<uint8_t> traced_off = capture(FxaaMode::Off);
        const std::vector<uint8_t> traced_raster = capture(FxaaMode::Raster);
        const std::vector<uint8_t> traced_always = capture(FxaaMode::Always);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());
        EXPECT_EQ(traced_raster, traced_off) << "Raster mode filtered a ray-traced frame";
        EXPECT_NE(traced_always, traced_off) << "Always mode left a ray-traced frame's edges alone";
    }
}

// Without faces (FETCH_ASSETS=OFF, a failed fetch) the sky is OxidANT's gradient, not the black cube that stood in until 2026-11-18.
TEST(GoldenRender, WithoutSkyboxFacesTheSkyIsTheAnalyticGradient)
{
    SKIP_WITHOUT_GPU();

    const ScopedEnvVar no_faces("KATAGLYPHIS_SKYBOX_DIR", "Resources/Textures/Skybox/no-such-sky");
    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    ASSERT_TRUE(harness.renderer->skyIsAnalytic()) << "KATAGLYPHIS_SKYBOX_DIR named no faces, yet a cube map loaded";

    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.clouds_enabled = false;
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture = [&](bool skybox) {
        scene_vars.skybox_enabled = skybox;
        harness.render_frames(SETTLE_FRAMES);
        return harness.capture_frame(width, height);
    };
    // The navy of the switched-off sky marks the sky pixels; the analytic sky then fills exactly those.
    const std::vector<uint8_t> navy = capture(false);
    const std::vector<uint8_t> sky = capture(true);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(navy.empty());
    ASSERT_EQ(sky.size(), navy.size());
    dump_cloud_frame("analytic-sky-navy", navy, width, height);
    dump_cloud_frame("analytic-sky", sky, width, height);

    // Every gradient colour lies between its three anchors, channel by channel, as post shows them.
    std::array<int, 3> lowest{ 255, 255, 255 };
    std::array<int, 3> highest{ 0, 0, 0 };
    for (const glm::vec3 &anchor :
      { Kataglyphis::kAnalyticSkyZenith, Kataglyphis::kAnalyticSkyHorizon, Kataglyphis::kAnalyticSkyGround }) {
        for (size_t c = 0; c < 3U; ++c) {
            lowest[c] = std::min(lowest[c], displayed_byte(anchor[static_cast<int>(c)]));
            highest[c] = std::max(highest[c], displayed_byte(anchor[static_cast<int>(c)]));
        }
    }
    size_t sky_pixels = 0;
    size_t dark = 0;
    size_t outside = 0;
    double blue_minus_red = 0.0;
    for (size_t pixel = 0; pixel < navy.size() / 4U; ++pixel) {
        if (!is_disabled_sky(navy, pixel)) { continue; }
        ++sky_pixels;
        const size_t base = pixel * 4U;
        if (std::max({ sky[base], sky[base + 1U], sky[base + 2U] }) < 20U) { ++dark; }
        bool off = false;
        for (size_t c = 0; c < 3U; ++c) {
            off = off || sky[base + c] < lowest[c] - 3 || sky[base + c] > highest[c] + 3;
        }
        if (off) { ++outside; }
        blue_minus_red += static_cast<double>(sky[base + 2U]) - static_cast<double>(sky[base]);
    }
    ASSERT_GT(sky_pixels, 5000U) << "the rig shows too little sky - check the framing";
    blue_minus_red /= static_cast<double>(sky_pixels);
    GTEST_LOG_(INFO) << "analytic sky over " << sky_pixels << " px: " << dark << " dark, " << outside
                     << " outside the anchors (" << lowest[0] << ".." << highest[0] << ", " << lowest[1] << ".."
                     << highest[1] << ", " << lowest[2] << ".." << highest[2] << "), blue - red " << blue_minus_red;

    EXPECT_EQ(dark, 0U) << "a missing skybox still draws the black fallback cube";
    EXPECT_LT(static_cast<double>(outside), 0.01 * static_cast<double>(sky_pixels))
      << "the sky is not OxidANT's zenith-horizon-ground gradient";
    EXPECT_GT(blue_minus_red, 10.0) << "the analytic sky is not blue overhead";
}

// The sky is radiance in the HDR target in every mode: drawn by the sky pass, or the path tracer's camera miss.
TEST(GoldenRender, SkyFillsTheHdrTargetInEveryMode)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    // One known radiance: sky.slang untonemaps the navy, so post shows (0, 0, 0.5) again.
    scene_vars.skybox_enabled = false;
    scene_vars.clouds_enabled = false;
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    harness.render_frames(WARMUP_FRAMES);
    const float navy = Kataglyphis::acesTonemapInverse(Kataglyphis::kSkyboxDisabledRadiance.z);

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
        bool pathTracing;
    };
    std::vector<Mode> modes = { Mode{ "forward", RasterizationMode::Forward, false, false },
        Mode{ "deferred", RasterizationMode::Deferred, false, false } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back(Mode{ "ray traced", RasterizationMode::Forward, true, false });
        modes.push_back(Mode{ "path traced", RasterizationMode::Forward, false, true });
    }
    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = mode.pathTracing;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
        ASSERT_EQ(radiance.size(), frame.size()) << mode.name;

        size_t transparent = 0;
        size_t sky_texels = 0;
        size_t sky_shown = 0;
        for (size_t pixel = 0; pixel < frame.size() / 4U; ++pixel) {
            const float *texel = &radiance[pixel * 4U];
            if (texel[3] < 0.999F) { ++transparent; }
            if (std::abs(texel[0]) > 0.01F || std::abs(texel[1]) > 0.01F || std::abs(texel[2] - navy) > 0.01F) {
                continue;
            }
            ++sky_texels;
            if (is_disabled_sky(frame, pixel)) { ++sky_shown; }
        }
        GTEST_LOG_(INFO) << mode.name << ": " << transparent << " transparent HDR texels, " << sky_texels
                         << " at the navy's radiance " << navy << ", " << sky_shown << " of them shown as the navy";
        // Before the sky drew into the HDR target, every sky texel here was alpha 0 and post blended over a sky pass.
        EXPECT_EQ(transparent, 0U) << mode.name << ": the sky left part of the HDR target unfilled";
        EXPECT_GT(sky_texels, 5000U) << mode.name << ": the HDR target holds no sky radiance - check the framing";
        EXPECT_GT(static_cast<double>(sky_shown), 0.99 * static_cast<double>(sky_texels))
          << mode.name << ": post does not show the sky's radiance as the navy";
    }
}

// Radiance past fp16's 65504 stores as +Inf (lavapipe: 65504), and the tonemap turns Inf into black NaN.
TEST(GoldenRender, HdrTargetStaysBelowTheHalfFloatMaximum)
{
    SKIP_WITHOUT_GPU();

    const auto lamp =
      write_gltf_variant(EMISSIVE_STRENGTH_CARD_MODEL, "kataglyphis_overbright_card.gltf", [](auto &gltf) {
          gltf["materials"][0]["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = 1.0e6;
      });
    ASSERT_TRUE(lamp.has_value()) << "could not write the overbright card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_TRUE(harness.renderer->addModel(lamp->string(), panel_free_card_placement()).has_value())
      << "adding the overbright card failed";

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
        bool pathTracing;
    };
    std::vector<Mode> modes = { Mode{ "forward", RasterizationMode::Forward, false, false },
        Mode{ "deferred", RasterizationMode::Deferred, false, false } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back(Mode{ "ray traced", RasterizationMode::Forward, true, false });
        modes.push_back(Mode{ "path traced", RasterizationMode::Forward, false, true });
    }
    // hdr_safe.slang's HDR_MAX, as fp16 rounds it; every pass that writes the HDR target clamps to it.
    constexpr float HDR_MAX = 65000.0F;
    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = mode.pathTracing;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
        ASSERT_FALSE(radiance.empty()) << mode.name;
        ASSERT_FALSE(frame.empty()) << mode.name;

        const HdrRange range = hdr_range(radiance);
        const double lamp_luma = mean_luminance_in_crop(frame, width, height, panel_free_card_box(width, height));
        GTEST_LOG_(INFO) << mode.name << ": " << range.non_finite << " non-finite HDR channels, largest "
                         << range.largest << ", lamp luma " << lamp_luma;

        EXPECT_EQ(range.non_finite, 0U) << mode.name << " wrote Inf or NaN to the HDR target";
        EXPECT_LE(range.largest, HDR_MAX) << mode.name << " wrote radiance past the fp16-safe maximum";
        EXPECT_GE(range.largest, 0.9F * HDR_MAX) << mode.name << ": the lamp never reached the clamp - check it";
        EXPECT_GT(lamp_luma, 240.0) << mode.name << ": the overbright lamp does not tonemap to white";
    }
}
