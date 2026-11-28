// Goldens: emission, texture slots and UVs, alpha masks and blending, and normal maps draw right in every mode.

#include "vulkan_base/PhysicalDeviceChoices.hpp"

#include "GoldenRenderHarness.hpp"

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.renderer;

using namespace Kataglyphis::TestSupport::GoldenRender;

namespace {

// Image 0 is valid base64 but not a decodable image; image 1 is a valid PNG.
constexpr const char *CORRUPT_EMBEDDED_IMAGE_MODEL = "Models/GltfTest/corrupt_embedded_image.gltf";
// A card with a normal texture and nothing else, the base for a normal-map control.
constexpr const char *NORMAL_MAP_CARD_MODEL = "Models/GltfTest/uv_transform_normal_only_card.gltf";
// One texel (218, 128, 218): a tangent-space normal tilted 45 degrees toward +T, as a glTF data URI.
constexpr const char *TILTED_NORMAL_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mO41XALAARsAjUuHifuAAAAAElFTkSuQmCC";
// TILTED_NORMAL_PNG's texel with blue 0, as a two-channel (BC5) map decodes: z must be rebuilt from x and y.
constexpr const char *TILTED_TWO_CHANNEL_NORMAL_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mO41cAAAAOSAVvHBuFyAAAAAElFTkSuQmCC";
// One texel (128, 38, 218): a tangent-space normal tilted 45 degrees toward -B, as a glTF data URI.
constexpr const char *TILTED_AGAINST_BITANGENT_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mNoULsFAAKqAYGCutYQAAAAAElFTkSuQmCC";

}// namespace

// The build-integrity check only sees material.emission as text; this proves it actually brightens pixels.
TEST(GoldenRender, EmissiveMaterialBrightensTheFrame)
{
    SKIP_WITHOUT_GPU();

    {
        // Probe here: the skip macro's `return` only leaves the TEST from this depth, not from the lambda.
        ScopedModelOverride rig(SHADOW_RIG_MODEL);
        EngineHarness probe;
        SKIP_WITHOUT_FRAME_CAPTURE(probe);
    }

    const glm::mat4 placement = panel_free_card_placement();
    const auto dark_twin = write_gltf_variant(EMISSIVE_CARD_MODEL,
      "kataglyphis_emissive_card_dark.gltf",
      [](nlohmann::json &gltf) { gltf["materials"][0]["emissiveFactor"] = { 0.0, 0.0, 0.0 }; });
    ASSERT_TRUE(dark_twin.has_value()) << "could not write the non-emissive twin of " << EMISSIVE_CARD_MODEL;
    const std::string dark_twin_path = dark_twin->string();

    // One harness per card: addModel only adds, so a swap would measure both cards.
    const auto capture_card_mean = [&placement](const char *card_model, const char *label) {
        ScopedModelOverride rig(SHADOW_RIG_MODEL);
        EngineHarness harness;

        harness.useForwardRaster();
        harness.settings.scene.shadows_enabled = true;
        harness.render_frames(WARMUP_FRAMES);
        if (harness.renderer->hasDeviceLost()) {
            ADD_FAILURE() << "Device lost while warming up (" << label << ").";
            return std::optional<double>();
        }

        const auto added = harness.renderer->addModel(card_model, placement);
        if (!added.has_value()) {
            ADD_FAILURE() << "adding the " << label << " card failed";
            return std::optional<double>();
        }
        harness.render_frames(SETTLE_FRAMES);
        if (harness.renderer->hasDeviceLost()) {
            ADD_FAILURE() << "Device lost after adding the " << label << " card.";
            return std::optional<double>();
        }

        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        if (frame.empty()) {
            ADD_FAILURE() << label << " card capture returned no pixels.";
            return std::optional<double>();
        }
        return std::optional<double>(mean_luminance_in_crop(frame, width, height, panel_free_card_box(width, height)));
    };

    const std::optional<double> emissive_mean = capture_card_mean(EMISSIVE_CARD_MODEL, "emissive");
    ASSERT_TRUE(emissive_mean.has_value());
    const std::optional<double> dark_mean = capture_card_mean(dark_twin_path.c_str(), "non-emissive twin");
    ASSERT_TRUE(dark_mean.has_value());

    GTEST_LOG_(INFO) << "card-region mean luminance: emissive " << *emissive_mean << ", non-emissive twin "
                     << *dark_mean;

    // The twin differs in emission alone; the metallic card, the control until 2026-11-21, brightened with N1's gain.
    constexpr double MARGIN = 10.0;
    EXPECT_GT(*emissive_mean, *dark_mean + MARGIN)
      << "The emissive card's own region is not meaningfully brighter than the non-emissive control; "
         "material.emission is likely not reaching this shading path.";
}

// An 8-bit UNORM G-buffer clips emission to 1.0; the debug scene, since the rig's sky diverges on its own.
TEST(GoldenRender, EmissiveStrengthSurvivesTheDeferredGBuffer)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = false;
    harness.settings.scene.shadows_enabled = true;

    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    const auto added = harness.renderer->addModel(EMISSIVE_STRENGTH_CARD_MODEL, panel_free_card_placement());
    ASSERT_TRUE(added.has_value()) << "adding the emissive-strength card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> forward = harness.capture_frame(width, height);
    ASSERT_FALSE(forward.empty()) << "Forward capture returned no pixels.";

    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> deferred = harness.capture_frame(width, height);
    ASSERT_FALSE(deferred.empty()) << "Deferred capture returned no pixels.";

    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_EQ(forward.size(), deferred.size());

    double abs_diff_sum = 0.0;
    for (size_t i = 0; i < forward.size(); ++i) {
        abs_diff_sum += std::abs(static_cast<int>(forward[i]) - static_cast<int>(deferred[i]));
    }
    const double mean_abs_diff = abs_diff_sum / static_cast<double>(forward.size());
    GTEST_LOG_(INFO) << "emissive-strength deferred-vs-forward mean abs channel diff: " << mean_abs_diff;

    if (const auto dump = Kataglyphis::environmentVariable("KATAGLYPHIS_EMISSIVE_DUMP")) {
        const std::string base = *dump;
        const auto stride = static_cast<int>(width) * 4;
        stbi_write_png((base + "-forward.png").c_str(), int(width), int(height), 4, forward.data(), stride);
        stbi_write_png((base + "-deferred.png").c_str(), int(width), int(height), 4, deferred.data(), stride);
    }

    // Measured: sits between the shipped float format and an eR8G8B8A8Unorm revert.
    constexpr double MEAN_ABS_DIFF_LIMIT = 0.7;
    EXPECT_LT(mean_abs_diff, MEAN_ABS_DIFF_LIMIT)
      << "Deferred diverges from forward on the emissive-strength card; the G-buffer attachment is likely "
         "clipping a strength>1 emitter again.";
}

// Sponza is the release default scene, so all its textures must fit under MAX_TEXTURE_COUNT.
TEST(GoldenRender, SponzaBindsEveryTextureSlot)
{
    SKIP_WITHOUT_GPU();

    const std::string sponza = sceneConfig::resolveModelPath("Models/crytek-sponza/sponza_triag.obj");
    if (!std::filesystem::exists(sponza)) { GTEST_SKIP() << "Sponza model not present"; }

    ScopedModelOverride sponza_override(sponza.c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);

    EXPECT_LE(harness.scene->getTextureCount(0), static_cast<uint32_t>(MAX_TEXTURE_COUNT))
      << "Sponza needs more texture slots than MAX_TEXTURE_COUNT provides.";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while binding Sponza's textures.";
    ASSERT_FALSE(frame.empty()) << "Frame capture returned no pixels.";
}

// OxidANT drops a corrupt image's slot and renumbers the later textureIDs, so none indexes past the descriptor array.
TEST(GoldenRender, CorruptEmbeddedImageKeepsTextureSlotsAligned)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride corrupt_override(CORRUPT_EMBEDDED_IMAGE_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost())
      << "Device lost loading a model with a corrupt embedded image - the failed decode was not handled.";

    EXPECT_EQ(harness.scene->getTextureCount(0), 1U) << "the corrupt image has no slot, the valid one its own";
}

// A 50/50 checkerboard changes about half its bounding box when discard works; KATAGLYPHIS_MASK_DUMP dumps PNGs.
TEST(GoldenRender, MaskCardDiscardsCutoutTexelsVisually)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // A: the base scene, before the card is added.
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    // Upper right, clear of the ImGui panel and the card's floor shadow; MASK_X/Y/Z/SCALE tune it without a rebuild.
    const auto env_f = [](const char *name, float fallback) {
        const auto value = Kataglyphis::environmentVariable(name);
        return value ? std::strtof(value->c_str(), nullptr) : fallback;
    };
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(env_f("MASK_X", 2.5F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
      * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
    const auto added = harness.renderer->addModel(MASK_CARD_MODEL, placement);
    ASSERT_TRUE(added.has_value()) << "adding the mask card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    // B: with the card.
    uint32_t w2 = 0;
    uint32_t h2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(w2, h2);
    ASSERT_FALSE(after.empty());
    ASSERT_EQ(width, w2);
    ASSERT_EQ(height, h2);

    const auto changed_at = [&](uint32_t x, uint32_t y) {
        const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
        return std::abs(static_cast<int>(before[b]) - static_cast<int>(after[b])) > 12
               || std::abs(static_cast<int>(before[b + 1U]) - static_cast<int>(after[b + 1U])) > 12
               || std::abs(static_cast<int>(before[b + 2U]) - static_cast<int>(after[b + 2U])) > 12;
    };

    // The FPS text and the floor shadow would otherwise pollute the bounding box.
    const uint32_t scan_x0 = (width * 68U) / 100U;
    const uint32_t scan_y1 = (height * 62U) / 100U;

    uint32_t minx = width;
    uint32_t miny = height;
    uint32_t maxx = 0;
    uint32_t maxy = 0;
    size_t changed_total = 0;
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            if (changed_at(x, y)) {
                ++changed_total;
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }

    if (const auto dump = Kataglyphis::environmentVariable("KATAGLYPHIS_MASK_DUMP")) {
        const std::string base = *dump;
        std::vector<uint8_t> diff(before.size(), 0U);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
                const bool in_scan = (x >= scan_x0 && y < scan_y1);
                const uint8_t v = changed_at(x, y) ? 255U : 0U;
                diff[b] = v;
                diff[b + 1U] = in_scan ? v : static_cast<uint8_t>(v / 3U);// dim outside the scan box
                diff[b + 2U] = in_scan ? v : static_cast<uint8_t>(v / 3U);
                diff[b + 3U] = 255U;
            }
        }
        const auto stride = static_cast<int>(width) * 4;
        stbi_write_png((base + "-before.png").c_str(), int(width), int(height), 4, before.data(), stride);
        stbi_write_png((base + "-after.png").c_str(), int(width), int(height), 4, after.data(), stride);
        stbi_write_png((base + "-diff.png").c_str(), int(width), int(height), 4, diff.data(), stride);
    }

    ASSERT_GT(changed_total, 200U)
      << "the card barely changed the upper-right region - not visible there (framing/culling)";

    const double box_area = static_cast<double>(maxx - minx + 1U) * static_cast<double>(maxy - miny + 1U);
    const double changed_fraction = static_cast<double>(changed_total) / box_area;
    GTEST_LOG_(INFO) << "mask card: changed " << changed_total << " px in upper-right, bbox [" << minx << "," << miny
                     << ".." << maxx << "," << maxy << "] fraction-in-box " << changed_fraction;

    // Measured between discard on and off; grey cut-out texels keep "off" below 1.0.
    EXPECT_LT(changed_fraction, 0.55)
      << "the card footprint changed too fully - cut-out texels are NOT being discarded";
    EXPECT_GT(changed_fraction, 0.20) << "the changed pixels are too sparse to be the checkerboard - check framing";
}

// Any of FORCE_OPAQUE on either ray or an eOpaque BLAS skips any-hit, so all three must stay relaxed.
TEST(GoldenRender, MaskCardDiscardsCutoutTexelsInRaytracing)
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
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // A: the base scene, before the card is added.
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    // Same env-tunable placement as the forward discard golden.
    const auto env_f = [](const char *name, float fallback) {
        const auto value = Kataglyphis::environmentVariable(name);
        return value ? std::strtof(value->c_str(), nullptr) : fallback;
    };
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(env_f("MASK_X", 2.5F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
      * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
    const auto added = harness.renderer->addModel(MASK_CARD_MODEL, placement);
    ASSERT_TRUE(added.has_value()) << "adding the mask card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    // B: with the card.
    uint32_t w2 = 0;
    uint32_t h2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(w2, h2);
    ASSERT_FALSE(after.empty());
    ASSERT_EQ(width, w2);
    ASSERT_EQ(height, h2);

    const auto changed_at = [&](uint32_t x, uint32_t y) {
        const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
        return std::abs(static_cast<int>(before[b]) - static_cast<int>(after[b])) > 12
               || std::abs(static_cast<int>(before[b + 1U]) - static_cast<int>(after[b + 1U])) > 12
               || std::abs(static_cast<int>(before[b + 2U]) - static_cast<int>(after[b + 2U])) > 12;
    };

    // Same GUI-free upper-right scan region as the forward golden.
    const uint32_t scan_x0 = (width * 68U) / 100U;
    const uint32_t scan_y1 = (height * 62U) / 100U;

    uint32_t minx = width;
    uint32_t miny = height;
    uint32_t maxx = 0;
    uint32_t maxy = 0;
    size_t changed_total = 0;
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            if (changed_at(x, y)) {
                ++changed_total;
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }

    if (const auto dump = Kataglyphis::environmentVariable("KATAGLYPHIS_MASK_DUMP")) {
        const std::string base = *dump;
        std::vector<uint8_t> diff(before.size(), 0U);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
                const bool in_scan = (x >= scan_x0 && y < scan_y1);
                const uint8_t v = changed_at(x, y) ? 255U : 0U;
                diff[b] = v;
                diff[b + 1U] = in_scan ? v : static_cast<uint8_t>(v / 3U);
                diff[b + 2U] = in_scan ? v : static_cast<uint8_t>(v / 3U);
                diff[b + 3U] = 255U;
            }
        }
        const auto stride = static_cast<int>(width) * 4;
        stbi_write_png((base + "-before.png").c_str(), int(width), int(height), 4, before.data(), stride);
        stbi_write_png((base + "-after.png").c_str(), int(width), int(height), 4, after.data(), stride);
        stbi_write_png((base + "-diff.png").c_str(), int(width), int(height), 4, diff.data(), stride);
    }

    ASSERT_GT(changed_total, 200U)
      << "the card barely changed the upper-right region - not visible there (framing/culling)";

    const double box_area = static_cast<double>(maxx - minx + 1U) * static_cast<double>(maxy - miny + 1U);
    const double changed_fraction = static_cast<double>(changed_total) / box_area;
    GTEST_LOG_(INFO) << "RT mask card: changed " << changed_total << " px in upper-right, bbox [" << minx << "," << miny
                     << ".." << maxx << "," << maxy << "] fraction-in-box " << changed_fraction;

    // RTX 2080, 2026-10-01: 0.479 with any-hit discarding, 0.941 with the primary ray forced opaque.
    EXPECT_LT(changed_fraction, 0.70)
      << "the card footprint changed too fully in RT - cut-out texels are NOT being discarded";
    EXPECT_GT(changed_fraction, 0.15) << "the changed pixels are too sparse to be the checkerboard - check framing";
}

// A ray query has no any-hit stage, so path_tracing.slang alpha-tests each candidate itself.
TEST(GoldenRender, PathTracedMaskCardShowsItsCutout)
{
    SKIP_WITHOUT_GPU();

    // The fixture leaves metallicFactor at glTF's default 1, which the Lambertian path tracer renders black.
    const auto card = write_gltf_variant(MASK_CARD_MODEL,
      "kataglyphis_dielectric_mask_card.gltf",
      [](nlohmann::json &gltf) { gltf["materials"][0]["pbrMetallicRoughness"]["metallicFactor"] = 0.0; });
    ASSERT_TRUE(card.has_value()) << "could not write the dielectric twin of " << MASK_CARD_MODEL;

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
    // Both captures get the same accumulated history, or PT noise alone changes pixels across the card's band.
    const int history = path_traced_history(60, 24);
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES + history);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // A: the base scene, before the card is added.
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    // Same env-tunable placement as the forward/RT discard goldens.
    const auto env_f = [](const char *name, float fallback) {
        const auto value = Kataglyphis::environmentVariable(name);
        return value ? std::strtof(value->c_str(), nullptr) : fallback;
    };
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(env_f("MASK_X", 2.5F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
      * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
    const auto added = harness.renderer->addModel(card->string(), placement);
    ASSERT_TRUE(added.has_value()) << "adding the mask card failed";
    harness.render_frames(SETTLE_FRAMES + history);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    // B: with the card.
    uint32_t w2 = 0;
    uint32_t h2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(w2, h2);
    ASSERT_FALSE(after.empty());
    ASSERT_EQ(width, w2);
    ASSERT_EQ(height, h2);

    const auto changed_at = [&](uint32_t x, uint32_t y) {
        const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
        return std::abs(static_cast<int>(before[b]) - static_cast<int>(after[b])) > 12
               || std::abs(static_cast<int>(before[b + 1U]) - static_cast<int>(after[b + 1U])) > 12
               || std::abs(static_cast<int>(before[b + 2U]) - static_cast<int>(after[b + 2U])) > 12;
    };

    // Same GUI-free upper-right scan region as the forward/RT goldens.
    const uint32_t scan_x0 = (width * 68U) / 100U;
    const uint32_t scan_y1 = (height * 62U) / 100U;

    // The card's checker cells survive a 3x3 erosion; the noise in the slab's interreflection-lit shadow does not.
    std::vector<bool> changed_mask(static_cast<size_t>(width) * height, false);
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            changed_mask[static_cast<size_t>(y) * width + x] = changed_at(x, y);
        }
    }
    const std::vector<bool> solid = eroded(changed_mask, width, height);
    uint32_t minx = width;
    uint32_t miny = height;
    uint32_t maxx = 0;
    uint32_t maxy = 0;
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            if (solid[static_cast<size_t>(y) * width + x]) {
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }
    size_t changed_total = 0;
    for (uint32_t y = miny; y <= maxy && minx <= maxx; ++y) {
        for (uint32_t x = minx; x <= maxx; ++x) {
            if (changed_mask[static_cast<size_t>(y) * width + x]) { ++changed_total; }
        }
    }

    if (const auto dump = Kataglyphis::environmentVariable("KATAGLYPHIS_MASK_DUMP")) {
        const std::string base = *dump;
        std::vector<uint8_t> diff(before.size(), 0U);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
                const bool in_scan = (x >= scan_x0 && y < scan_y1);
                const uint8_t v = changed_at(x, y) ? 255U : 0U;
                diff[b] = v;
                diff[b + 1U] = in_scan ? v : static_cast<uint8_t>(v / 3U);
                diff[b + 2U] = in_scan ? v : static_cast<uint8_t>(v / 3U);
                diff[b + 3U] = 255U;
            }
        }
        const auto stride = static_cast<int>(width) * 4;
        stbi_write_png((base + "-before.png").c_str(), int(width), int(height), 4, before.data(), stride);
        stbi_write_png((base + "-after.png").c_str(), int(width), int(height), 4, after.data(), stride);
        stbi_write_png((base + "-diff.png").c_str(), int(width), int(height), 4, diff.data(), stride);
    }

    ASSERT_GT(changed_total, 200U)
      << "the card barely changed the upper-right region - not visible there (framing/culling)";

    const double box_area = static_cast<double>(maxx - minx + 1U) * static_cast<double>(maxy - miny + 1U);
    const double changed_fraction = static_cast<double>(changed_total) / box_area;
    GTEST_LOG_(INFO) << "PT mask card: changed " << changed_total << " px in upper-right, bbox [" << minx << "," << miny
                     << ".." << maxx << "," << maxy << "] fraction-in-box " << changed_fraction;

    // RTX 2080, 2026-10-01: 0.346 with the ray query's alpha test, 0.627 with the query forced opaque.
    EXPECT_LT(changed_fraction, 0.49)
      << "the card footprint changed too fully in PT - cut-out texels are NOT being discarded";
    EXPECT_GT(changed_fraction, 0.15) << "the changed pixels are too sparse to be the checkerboard - check framing";
}

// The card shows its back face, so it is visible only if doubleSided reaches the per-draw cull mode.
TEST(GoldenRender, MaskCardDoubleSidedRendersFromBehind)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    const auto env_f = [](const char *name, float fallback) {
        const auto value = Kataglyphis::environmentVariable(name);
        return value ? std::strtof(value->c_str(), nullptr) : fallback;
    };
    // Rotated 180 degrees about Y so the back face faces the camera.
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(env_f("MASK_X", 2.5F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
      * glm::rotate(glm::mat4(1.0F), glm::radians(180.0F), glm::vec3(0.0F, 1.0F, 0.0F))
      * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
    const auto added = harness.renderer->addModel(MASK_CARD_MODEL, placement);
    ASSERT_TRUE(added.has_value()) << "adding the mask card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t w2 = 0;
    uint32_t h2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(w2, h2);
    ASSERT_FALSE(after.empty());
    ASSERT_EQ(width, w2);
    ASSERT_EQ(height, h2);

    const auto changed_at = [&](uint32_t x, uint32_t y) {
        const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
        return std::abs(static_cast<int>(before[b]) - static_cast<int>(after[b])) > 12
               || std::abs(static_cast<int>(before[b + 1U]) - static_cast<int>(after[b + 1U])) > 12
               || std::abs(static_cast<int>(before[b + 2U]) - static_cast<int>(after[b + 2U])) > 12;
    };

    const uint32_t scan_x0 = (width * 68U) / 100U;
    const uint32_t scan_y1 = (height * 62U) / 100U;
    uint32_t minx = width;
    uint32_t miny = height;
    uint32_t maxx = 0;
    uint32_t maxy = 0;
    size_t changed_total = 0;
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            if (changed_at(x, y)) {
                ++changed_total;
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }

    // Single-sided culling would leave changed_total near 0.
    ASSERT_GT(changed_total, 1500U) << "the back-facing doubleSided card is not visible - back-face culled, so "
                                       "the doubleSided flag is not reaching the per-draw cull mode";

    const double box_area = static_cast<double>(maxx - minx + 1U) * static_cast<double>(maxy - miny + 1U);
    const double changed_fraction = static_cast<double>(changed_total) / box_area;
    GTEST_LOG_(INFO) << "doubleSided card (from behind): changed " << changed_total << " px, fraction-in-box "
                     << changed_fraction;
    // And it is the cut-out checkerboard, not a solid blob or artifact.
    EXPECT_GT(changed_fraction, 0.20);
    EXPECT_LT(changed_fraction, 0.80);
}

// Culled out of the G-buffer, the card would never reach the lighting pass.
TEST(GoldenRender, MaskCardDoubleSidedRendersFromBehindDeferred)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    const auto env_f = [](const char *name, float fallback) {
        const auto value = Kataglyphis::environmentVariable(name);
        return value ? std::strtof(value->c_str(), nullptr) : fallback;
    };
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(env_f("MASK_X", 2.5F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
      * glm::rotate(glm::mat4(1.0F), glm::radians(180.0F), glm::vec3(0.0F, 1.0F, 0.0F))
      * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
    const auto added = harness.renderer->addModel(MASK_CARD_MODEL, placement);
    ASSERT_TRUE(added.has_value()) << "adding the mask card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t w2 = 0;
    uint32_t h2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(w2, h2);
    ASSERT_FALSE(after.empty());
    ASSERT_EQ(width, w2);
    ASSERT_EQ(height, h2);

    const auto changed_at = [&](uint32_t x, uint32_t y) {
        const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
        return std::abs(static_cast<int>(before[b]) - static_cast<int>(after[b])) > 12
               || std::abs(static_cast<int>(before[b + 1U]) - static_cast<int>(after[b + 1U])) > 12
               || std::abs(static_cast<int>(before[b + 2U]) - static_cast<int>(after[b + 2U])) > 12;
    };

    const uint32_t scan_x0 = (width * 68U) / 100U;
    const uint32_t scan_y1 = (height * 62U) / 100U;
    size_t changed_total = 0;
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            if (changed_at(x, y)) { ++changed_total; }
        }
    }
    GTEST_LOG_(INFO) << "doubleSided card (deferred, from behind): changed " << changed_total << " px";
    ASSERT_GT(changed_total, 1500U) << "the back-facing doubleSided card is not in the deferred G-buffer - the "
                                       "geometry pass back-face culled it despite doubleSided";
}

// Sampling repeats, so a 4x UV scale tiles the 8x8 checkerboard to 32x32, far more edges per area.
TEST(GoldenRender, KhrTextureTransformTilesTheTexture)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    const auto env_f = [](const char *name, float fallback) {
        const auto value = Kataglyphis::environmentVariable(name);
        return value ? std::strtof(value->c_str(), nullptr) : fallback;
    };
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(env_f("MASK_X", 2.5F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
      * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
    const auto added = harness.renderer->addModel(UV_TRANSFORM_MODEL, placement);
    ASSERT_TRUE(added.has_value()) << "adding the uv-transform card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t w2 = 0;
    uint32_t h2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(w2, h2);
    ASSERT_FALSE(after.empty());
    ASSERT_EQ(width, w2);
    ASSERT_EQ(height, h2);

    // Locate the card: bounding box of changed pixels in the GUI-free upper-right.
    const auto changed_at = [&](uint32_t x, uint32_t y) {
        const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
        return std::abs(static_cast<int>(before[b]) - static_cast<int>(after[b])) > 12
               || std::abs(static_cast<int>(before[b + 1U]) - static_cast<int>(after[b + 1U])) > 12
               || std::abs(static_cast<int>(before[b + 2U]) - static_cast<int>(after[b + 2U])) > 12;
    };
    const uint32_t scan_x0 = (width * 68U) / 100U;
    const uint32_t scan_y1 = (height * 62U) / 100U;
    uint32_t minx = width;
    uint32_t miny = height;
    uint32_t maxx = 0;
    uint32_t maxy = 0;
    size_t changed_total = 0;
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            if (changed_at(x, y)) {
                ++changed_total;
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }
    ASSERT_GT(changed_total, 500U) << "the uv-transform card is not visible in the upper-right (framing)";

    // A finely tiled checkerboard is nearly all edges; an untransformed one is not.
    const double detail = detail_fraction(after, width, height, Crop{ minx, maxx + 1U, miny, maxy + 1U });
    GTEST_LOG_(INFO) << "uv-transform card: box [" << minx << "," << miny << ".." << maxx << "," << maxy
                     << "] detail-fraction " << detail;

    // The untransformed 8x8 card measures far lower.
    EXPECT_GT(detail, 0.15)
      << "the card is not finely tiled - KHR_texture_transform scale is not reaching the sampled UV";
}

// Two half-transparent cards over an opaque one, seen from both sides, so each order is right once and wrong once.
TEST(GoldenRender, BlendCardsCompositeInDepthOrderFromBothSides)
{
    SKIP_WITHOUT_GPU();

    const auto unlit = [](std::array<double, 4> rgba, bool blend) {
        nlohmann::json material;
        material["pbrMetallicRoughness"] = { { "baseColorFactor", rgba } };
        material["doubleSided"] = true;
        material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
        if (blend) { material["alphaMode"] = "BLEND"; }
        return material;
    };
    // Red and green overlap at x in [-0.25, 0.25] before grey backdrops; at y = 8 a red pair stands against the sky.
    const auto scene = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_blend_cards.gltf", [&](auto &gltf) {
        make_card_scene(gltf,
          { unlit({ 0.5, 0.5, 0.5, 1.0 }, false),
            unlit({ 1.0, 0.0, 0.0, 0.5 }, true),
            unlit({ 0.0, 1.0, 0.0, 0.5 }, true),
            unlit({ 1.0, 0.0, 0.0, 1.0 }, false) },
          { { 0, { { "translation", { 0.0, 0.0, -3.0 } }, { "scale", { 8.0, 8.0, 1.0 } } } },
            { 0, { { "translation", { 0.0, 0.0, 3.0 } }, { "scale", { 8.0, 8.0, 1.0 } } } },
            { 1, { { "translation", { -0.25, 0.0, 0.0 } } } },
            { 2, { { "translation", { 0.25, 0.0, 0.5 } } } },
            { 1, { { "translation", { -0.6, 8.0, 0.0 } } } },
            { 3, { { "translation", { 0.6, 8.0, 0.0 } } } } });
        gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_materials_unlit" });
    });
    ASSERT_TRUE(scene.has_value()) << "could not write the blend cards";

    ScopedModelOverride cards(scene->string().c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    // The disabled sky is one flat colour, so the card over it has one expected value.
    scene_vars.skybox_enabled = false;
    harness.render_frames(WARMUP_FRAMES);

    struct View
    {
        const char *name;
        float z;
        float yaw;
        // The colour channel of the nearer card; the other card's is 1 - near.
        int near_channel;
    };
    // Looking +Z mirrors x, so the farther card is on the left from both sides and the overlap stays centred.
    constexpr std::array<View, 2> VIEWS = { { { "front", 1.8F, -90.0F, 1 }, { "back", -1.3F, 90.0F, 0 } } };
    constexpr std::array<RasterizationMode, 2> MODES = { RasterizationMode::Forward, RasterizationMode::Deferred };

    for (const RasterizationMode mode : MODES) {
        renderer_vars.rasterizationMode = mode;
        const char *mode_name = mode == RasterizationMode::Forward ? "forward" : "deferred";
        for (const View &view : VIEWS) {
            harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, view.z));
            harness.camera->set_orientation(view.yaw, 0.0F);
            harness.render_frames(SETTLE_FRAMES);
            uint32_t width = 0;
            uint32_t height = 0;
            const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
            ASSERT_FALSE(harness.renderer->hasDeviceLost());
            ASSERT_FALSE(radiance.empty());

            // Near card 1.3 away, far card 1.8: only the far one covers x -0.55 at 1.8, only the near one 0.47 at 1.3.
            const uint32_t y = height / 2U;
            const uint32_t far_x = card_column(-0.55F, 1.8F, width, height);
            const uint32_t near_x = card_column(0.47F, 1.3F, width, height);
            const glm::dvec4 overlap = mean_hdr_rgba_in_square(radiance, width, width / 2U, y, 12U);
            const glm::dvec4 far_only = mean_hdr_rgba_in_square(radiance, width, far_x, y, 12U);
            const glm::dvec4 near_only = mean_hdr_rgba_in_square(radiance, width, near_x, y, 12U);
            const int near_channel = view.near_channel;
            const int far_channel = 1 - view.near_channel;
            GTEST_LOG_(INFO) << mode_name << " " << view.name << ": overlap (" << overlap.r << ", " << overlap.g << ", "
                             << overlap.b << ", " << overlap.a << "), far only (" << far_only.r << ", " << far_only.g
                             << ", " << far_only.b << "), near only (" << near_only.r << ", " << near_only.g << ", "
                             << near_only.b << ")";

            // Linear over: one card over the 0.5 grey gives 0.75 / 0.25; near over far over grey 0.625 / 0.375.
            constexpr double TOLERANCE = 0.03;
            EXPECT_NEAR(far_only[far_channel], 0.75, TOLERANCE)
              << mode_name << " " << view.name << ": the far card alone does not blend over the grey";
            EXPECT_NEAR(far_only.b, 0.25, TOLERANCE)
              << mode_name << " " << view.name << ": the grey does not show through the far card";
            EXPECT_NEAR(near_only[near_channel], 0.75, TOLERANCE)
              << mode_name << " " << view.name << ": the near card alone does not blend over the grey";
            EXPECT_NEAR(overlap[near_channel], 0.625, TOLERANCE)
              << mode_name << " " << view.name << ": the nearer card does not lie on top - the BLEND sort is off";
            EXPECT_NEAR(overlap[far_channel], 0.375, TOLERANCE)
              << mode_name << " " << view.name << ": the farther card does not show through the nearer one";
            EXPECT_NEAR(overlap.b, 0.125, TOLERANCE)
              << mode_name << " " << view.name << ": the grey does not show through both cards";
            EXPECT_GT(overlap.a, 0.99) << mode_name << " " << view.name
                                       << ": blending over an opaque pixel lowered its coverage, so post would "
                                          "composite the sky through it";
        }

        // Over the sky as over the grey: linear over the radiance sky.slang wrote, the navy it untonemaps.
        harness.camera->set_camera_position(glm::vec3(0.0F, 8.0F, 1.8F));
        harness.camera->set_orientation(-90.0F, 0.0F);
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        ASSERT_FALSE(radiance.empty());
        const uint32_t y = height / 2U;
        const glm::dvec4 blend =
          mean_hdr_rgba_in_square(radiance, width, card_column(-0.6F, 1.8F, width, height), y, 8U);
        const glm::dvec4 opaque =
          mean_hdr_rgba_in_square(radiance, width, card_column(0.6F, 1.8F, width, height), y, 8U);
        const auto navy = static_cast<double>(Kataglyphis::acesTonemapInverse(Kataglyphis::kSkyboxDisabledRadiance.z));
        GTEST_LOG_(INFO) << mode_name << " over the sky: BLEND card (" << blend.r << ", " << blend.g << ", " << blend.b
                         << ", " << blend.a << "), opaque card red " << opaque.r << ", sky blue " << navy;
        constexpr double SKY_TOLERANCE = 0.03;
        ASSERT_NEAR(opaque.r, 1.0, SKY_TOLERANCE)
          << mode_name << ": the opaque red card is not in view - check the placement";
        EXPECT_NEAR(blend.r, 0.5, SKY_TOLERANCE)
          << mode_name << ": a half-transparent card over the sky is not half its red";
        // Before the sky drew into the HDR target, the card blended over nothing here: blue 0 at alpha 0.5.
        EXPECT_NEAR(blend.b, 0.5 * navy, SKY_TOLERANCE) << mode_name << ": the sky's radiance does not show through";
        EXPECT_GT(blend.a, 0.99) << mode_name << ": the card over the sky is not opaque in the HDR target";
    }
}

// A 2x1 emissive texture, white then black, nearest-filtered, so each half of a card shows which UV set it sampled.
constexpr const char *WHITE_BLACK_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAIAAAB7QOjdAAAAD0lEQVR4nGP4//8/AwMDAA74Av7Ji4P1AAAAAElFTkSuQmCC";
// The emissive card's four TEXCOORD_0 values with u mirrored, as float32 VEC2: (1,1) (0,1) (0,0) (1,0).
constexpr const char *MIRRORED_U_TEXCOORDS =
  "data:application/octet-stream;base64,AACAPwAAgD8AAAAAAACAPwAAAAAAAAAAAACAPwAAAAA=";

// TEXCOORD_1 mirrors u, so an emissive texture on set 1 lights the other half of the card in every mode.
TEST(GoldenRender, EmissiveOnTexCoord1FollowsTheSecondUvSetInEveryMode)
{
    SKIP_WITHOUT_GPU();

    const auto emissive = [](nlohmann::json texture_info) {
        nlohmann::json material;
        material["pbrMetallicRoughness"] = {
            { "baseColorFactor", { 0.0, 0.0, 0.0, 1.0 } }, { "metallicFactor", 0.0 }, { "roughnessFactor", 1.0 }
        };
        material["emissiveFactor"] = { 1.0, 1.0, 1.0 };
        material["emissiveTexture"] = texture_info;
        material["doubleSided"] = true;
        return material;
    };
    nlohmann::json on_set1;
    on_set1["index"] = 0;
    on_set1["texCoord"] = 1;
    nlohmann::json override_to_set1;
    override_to_set1["index"] = 0;
    override_to_set1["extensions"]["KHR_texture_transform"] = { { "texCoord", 1 } };
    // Left: the TEXCOORD_0 control; centre: texCoord 1; right: texCoord 0 that KHR_texture_transform moves to 1.
    constexpr std::array<float, 3> CARD_X = { -1.2F, 0.0F, 1.2F };
    const auto scene = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_uv1_emissive.gltf", [&](auto &gltf) {
        make_card_scene(gltf,
          { emissive({ { "index", 0 } }), emissive(on_set1), emissive(override_to_set1) },
          { { 0, { { "translation", { CARD_X[0], 0.0F, 0.0F } } } },
            { 1, { { "translation", { CARD_X[1], 0.0F, 0.0F } } } },
            { 2, { { "translation", { CARD_X[2], 0.0F, 0.0F } } } } });
        nlohmann::json buffer;
        buffer["uri"] = MIRRORED_U_TEXCOORDS;
        buffer["byteLength"] = 32;
        gltf["buffers"].push_back(buffer);
        nlohmann::json view;
        view["buffer"] = gltf["buffers"].size() - 1U;
        view["byteOffset"] = 0;
        view["byteLength"] = 32;
        view["target"] = 34962;
        gltf["bufferViews"].push_back(view);
        nlohmann::json accessor;
        accessor["bufferView"] = gltf["bufferViews"].size() - 1U;
        accessor["componentType"] = 5126;
        accessor["count"] = 4;
        accessor["type"] = "VEC2";
        gltf["accessors"].push_back(accessor);
        for (auto &mesh : gltf["meshes"]) {
            mesh["primitives"][0]["attributes"]["TEXCOORD_1"] = gltf["accessors"].size() - 1U;
        }
        gltf["images"] = nlohmann::json::array({ { { "uri", WHITE_BLACK_PNG } } });
        gltf["samplers"] = nlohmann::json::array(
          { { { "magFilter", 9728 }, { "minFilter", 9728 }, { "wrapS", 33071 }, { "wrapT", 33071 } } });
        gltf["textures"] = nlohmann::json::array({ { { "source", 0 }, { "sampler", 0 } } });
        gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_texture_transform" });
    });
    ASSERT_TRUE(scene.has_value()) << "could not write the TEXCOORD_1 cards";

    ScopedModelOverride cards(scene->string().c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    // Emission only: a lit black card would add a highlight to either half.
    scene_vars.directional_light_radiance = 0.0F;
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    constexpr float DISTANCE = 3.0F;
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, DISTANCE));
    harness.camera->set_orientation(-90.0F, 0.0F);
    harness.render_frames(WARMUP_FRAMES);

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
        bool path_tracing;
    };
    std::vector<Mode> modes = { { "forward", RasterizationMode::Forward, false, false },
        { "deferred", RasterizationMode::Deferred, false, false } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back({ "ray traced", RasterizationMode::Forward, true, false });
        modes.push_back({ "path traced", RasterizationMode::Forward, false, true });
    } else {
        GTEST_LOG_(INFO) << "Hardware raytracing unsupported; checking the raster modes only.";
    }

    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = mode.path_tracing;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());
        ASSERT_FALSE(radiance.empty());

        // u < 0.5 is the white texel: the -x half on TEXCOORD_0, the +x half on the mirrored TEXCOORD_1.
        std::array<double, 3> white_minus_black{};
        for (size_t card = 0; card < CARD_X.size(); ++card) {
            const uint32_t minus_x = card_column(CARD_X[card] - 0.25F, DISTANCE, width, height);
            const uint32_t plus_x = card_column(CARD_X[card] + 0.25F, DISTANCE, width, height);
            const double minus_half = mean_hdr_in_square(radiance, width, minus_x, height / 2U, 16U);
            const double plus_half = mean_hdr_in_square(radiance, width, plus_x, height / 2U, 16U);
            white_minus_black[card] = card == 0 ? minus_half - plus_half : plus_half - minus_half;
            GTEST_LOG_(INFO) << mode.name << " card " << card << ": -x half " << minus_half << ", +x half "
                             << plus_half;
        }
        EXPECT_GT(white_minus_black[0], 0.5)
          << mode.name << ": the TEXCOORD_0 control does not glow on its -x half - check the placement";
        EXPECT_GT(white_minus_black[1], 0.5)
          << mode.name << ": an emissive texture on texCoord 1 samples TEXCOORD_0 - the second UV set is ignored";
        EXPECT_GT(white_minus_black[2], 0.5) << mode.name
                                             << ": KHR_texture_transform's texCoord 1 does not move the emissive "
                                                "slot onto the second UV set";
    }
}

// Standard deviation of the mean of red, green and blue over a square of an RGBA float capture around (cx, cy).
static double hdr_stddev_in_square(const std::vector<float> &rgba, uint32_t w, uint32_t cx, uint32_t cy, uint32_t half)
{
    const double mean = mean_hdr_in_square(rgba, w, cx, cy, half);
    double sum = 0.0;
    size_t count = 0;
    for (uint32_t y = cy - half; y < cy + half; ++y) {
        for (uint32_t x = cx - half; x < cx + half; ++x) {
            const size_t base = (static_cast<size_t>(y) * w + x) * 4U;
            const double value = static_cast<double>(rgba[base] + rgba[base + 1U] + rgba[base + 2U]) / 3.0;
            sum += (value - mean) * (value - mean);
            ++count;
        }
    }
    return count > 0U ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
}

// glTF's LINEAR min filter reads level 0 alone, so a far, fine checkerboard aliases where a mipmapped one is grey.
TEST(GoldenRender, AMipmaplessMinFilterSamplesOnlyTheBaseLevel)
{
    SKIP_WITHOUT_GPU();

    // Unlit, so a pixel is its texel; 64 tiles of the 8x8 checkerboard put 512 texels across each 1-unit card.
    const auto checker = [](int texture) {
        nlohmann::json texture_info;
        texture_info["index"] = texture;
        texture_info["extensions"]["KHR_texture_transform"] = { { "scale", { 64.0, 64.0 } } };
        nlohmann::json material;
        material["pbrMetallicRoughness"] = { { "baseColorTexture", texture_info } };
        material["doubleSided"] = true;
        material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
        return material;
    };
    // Left: LINEAR (9729), glTF's mipmap-less min filter; right: LINEAR_MIPMAP_LINEAR (9987), the control.
    constexpr std::array<float, 2> CARD_X = { -0.6F, 0.6F };
    const auto scene = write_gltf_variant(UV_TRANSFORM_MODEL, "kataglyphis_mipmapless_checker.gltf", [&](auto &gltf) {
        make_card_scene(gltf,
          { checker(0), checker(1) },
          { { 0, { { "translation", { CARD_X[0], 0.0F, 0.0F } } } },
            { 1, { { "translation", { CARD_X[1], 0.0F, 0.0F } } } } });
        gltf["samplers"] = nlohmann::json::array(
          { { { "magFilter", 9729 }, { "minFilter", 9729 } }, { { "magFilter", 9729 }, { "minFilter", 9987 } } });
        gltf["textures"] =
          nlohmann::json::array({ { { "source", 0 }, { "sampler", 0 } }, { { "source", 0 }, { "sampler", 1 } } });
        gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_texture_transform", "KHR_materials_unlit" });
    });
    ASSERT_TRUE(scene.has_value()) << "could not write the checkerboard cards";

    ScopedModelOverride cards(scene->string().c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    auto &renderer_vars = harness.useForwardRaster();
    harness.settings.scene.shadows_enabled = false;
    // About 4.4 texels a pixel: the mipmapped card samples levels 2 and 3, which are flat grey.
    constexpr float DISTANCE = 8.0F;
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, DISTANCE));
    harness.camera->set_orientation(-90.0F, 0.0F);
    harness.render_frames(WARMUP_FRAMES);

    for (const RasterizationMode mode : { RasterizationMode::Forward, RasterizationMode::Deferred }) {
        renderer_vars.rasterizationMode = mode;
        const char *mode_name = mode == RasterizationMode::Forward ? "forward" : "deferred";
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());
        ASSERT_FALSE(radiance.empty());

        std::array<double, 2> spread{};
        for (size_t card = 0; card < CARD_X.size(); ++card) {
            const uint32_t x = card_column(CARD_X[card], DISTANCE, width, height);
            spread[card] = hdr_stddev_in_square(radiance, width, x, height / 2U, 24U);
            GTEST_LOG_(INFO) << mode_name << " card " << card << ": mean "
                             << mean_hdr_in_square(radiance, width, x, height / 2U, 24U) << ", spread " << spread[card];
        }
        EXPECT_LT(spread[1], 0.03) << mode_name << ": the mipmapped control is not flat grey - check the framing";
        EXPECT_GT(spread[0], 0.08) << mode_name
                                   << ": the LINEAR min filter averaged the far checkerboard to grey, so it sampled "
                                      "the mip chain glTF's mipmap-less filter must not";
    }
}

// OxidANT's cube whose only base colour is a KHR_texture_basisu UASTC ramp: red rises with u, blue falls with u + v.
std::string basisu_cube_model() { return Kataglyphis::TestSupport::oxidantGltfFixture("cube_basisu.gltf"); }

namespace {

// Red minus blue over the centre crop's left and right fifths: about -200 and +100 on the ramp, 0 on a flat colour.
double cube_ramp_spread(EngineHarness &harness, const char *mode)
{
    harness.render_frames(SETTLE_FRAMES);
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    EXPECT_FALSE(harness.renderer->hasDeviceLost());
    if (frame.empty()) { return 0.0; }
    const auto mean_red_minus_blue = [&](uint32_t x0, uint32_t x1) {
        double sum = 0.0;
        size_t count = 0;
        for (uint32_t y = height / 2U - 60U; y < height / 2U + 60U; ++y) {
            for (uint32_t x = x0; x < x1; ++x) {
                const size_t base = (static_cast<size_t>(y) * width + x) * 4U;
                sum += static_cast<double>(frame[base]) - static_cast<double>(frame[base + 2U]);
                ++count;
            }
        }
        return sum / static_cast<double>(count);
    };
    const double left = mean_red_minus_blue(width / 2U - 150U, width / 2U - 90U);
    const double right = mean_red_minus_blue(width / 2U + 90U, width / 2U + 150U);
    GTEST_LOG_(INFO) << mode << ": red minus blue " << left << " left, " << right << " right";
    return std::abs(right - left);
}

// The startup cube, framed, shows its ramp in forward, deferred and, where the device traces, ray traced frames.
void expect_ramp_in_every_mode(EngineHarness &harness, const char *lost)
{
    auto &renderer_vars = harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.6F, 2.2F));
    harness.camera->set_orientation(-90.0F, -15.0F);
    EXPECT_GT(cube_ramp_spread(harness, "forward"), 80.0) << "forward: the cube shows no ramp - " << lost;
    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    EXPECT_GT(cube_ramp_spread(harness, "deferred"), 80.0) << "deferred: the cube shows no ramp - " << lost;
    if (harness.renderer->supportsHardwareRaytracing()) {
        renderer_vars.raytracing = true;
        EXPECT_GT(cube_ramp_spread(harness, "ray traced"), 80.0) << "ray traced: the cube shows no ramp - " << lost;
    }
}

}// namespace

// A KTX2-only texture drew the cube white: the bridge had no encoded image for stb, so the slot was dropped.
TEST(GoldenRender, BasisuOnlyTextureDrawsItsRampInEveryMode)
{
    SKIP_WITHOUT_GPU();

    const std::string model = basisu_cube_model();
    ScopedModelOverride cube(model.c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    expect_ramp_in_every_mode(harness, "the KTX2 is lost");
}

// The same ramp as a lossless WebP through EXT_texture_webp, the cube's only source: stb reads no WebP.
TEST(GoldenRender, WebpOnlyTextureDrawsItsRampInEveryMode)
{
    SKIP_WITHOUT_GPU();

    const std::string model = Kataglyphis::TestSupport::oxidantGltfFixture("cube_webp.gltf");
    ScopedModelOverride cube(model.c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    expect_ramp_in_every_mode(harness, "the WebP is lost");
}

// With a red PNG as its core source too, the WebP draws: a red cube's red minus blue is the same on both sides.
TEST(GoldenRender, WebpTextureDrawsInsteadOfItsPngFallback)
{
    SKIP_WITHOUT_GPU();

    const std::string model = Kataglyphis::TestSupport::oxidantGltfFixture("cube_webp_fallback.gltf");
    ScopedModelOverride cube(model.c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    expect_ramp_in_every_mode(harness, "the PNG fallback drew instead of the WebP");
}

// The KTX2 arrives as the blocks its device samples, transcoded once, and draws as its RGBA8 chain does in each family.
TEST(GoldenRender, BasisTextureUploadsAsTheDevicesBlocksAndDrawsAsItsRgba8Chain)
{
    SKIP_WITHOUT_GPU();

    const std::string model = basisu_cube_model();
    ScopedModelOverride cube(model.c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);
    const Kataglyphis::BlockSupport device_blocks = harness.renderer->sharedDevice()->getBlockSupport();

    // Load n sits 50 units along x, framed as the startup cube is, so a frame sees that cube alone.
    constexpr float kSpacing = 50.0F;
    const auto crop_of = [&](uint32_t index) {
        harness.camera->set_camera_position(glm::vec3(kSpacing * static_cast<float>(index), 0.6F, 2.2F));
        harness.camera->set_orientation(-90.0F, -15.0F);
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        std::vector<int> crop;
        if (frame.empty()) { return crop; }
        for (uint32_t y = height / 2U - 60U; y < height / 2U + 60U; ++y) {
            for (uint32_t x = width / 2U - 150U; x < width / 2U + 150U; ++x) {
                const size_t base = (static_cast<size_t>(y) * width + x) * 4U;
                for (size_t channel = 0; channel < 3U; ++channel) { crop.push_back(frame[base + channel]); }
            }
        }
        return crop;
    };
    // Red minus blue, right fifth against left: the ramp's spread, near 0 on an untextured cube.
    const auto ramp_spread = [](const std::vector<int> &crop) {
        double left = 0.0;
        double right = 0.0;
        for (size_t row = 0; row < 120U; ++row) {
            for (size_t x = 0; x < 60U; ++x) {
                const size_t l = (row * 300U + x) * 3U;
                const size_t r = (row * 300U + 240U + x) * 3U;
                left += crop[l] - crop[l + 2U];
                right += crop[r] - crop[r + 2U];
            }
        }
        return std::abs(right - left) / (120.0 * 60.0);
    };
    const auto mean_difference = [](const std::vector<int> &a, const std::vector<int> &b) {
        double sum = 0.0;
        for (size_t i = 0; i < a.size(); ++i) { sum += std::abs(a[i] - b[i]); }
        return sum / static_cast<double>(a.size());
    };
    const auto format_of = [&](uint32_t index) { return harness.scene->getTextures(index).at(0).getFormat(); };
    const auto load_with = [&](const Kataglyphis::BlockSupport &blocks, uint32_t index) {
        harness.scene->setBlockSupport(blocks);
        const glm::mat4 placed = glm::translate(glm::mat4(1.0F), glm::vec3(kSpacing * static_cast<float>(index), 0, 0));
        return harness.renderer->addModel(model, placed) == std::optional<uint32_t>(index);
    };

    const vk::Format device_format = device_blocks.bc     ? vk::Format::eBc7SrgbBlock
                                     : device_blocks.etc2 ? vk::Format::eEtc2R8G8B8A8SrgbBlock
                                     : device_blocks.astc ? vk::Format::eAstc4x4SrgbBlock
                                                          : vk::Format::eR8G8B8A8Srgb;
    EXPECT_EQ(format_of(0), device_format) << "the startup load takes the device's best block format";
    const std::vector<int> startup = crop_of(0);

    ASSERT_TRUE(load_with({}, 1)) << "the RGBA8 reference did not load";
    EXPECT_EQ(format_of(1), vk::Format::eR8G8B8A8Srgb) << "no block family sampled: RGBA8";
    const std::vector<int> rgba8 = crop_of(1);
    ASSERT_FALSE(startup.empty() || rgba8.empty());
    EXPECT_GT(ramp_spread(rgba8), 80.0) << "the RGBA8 reference shows no ramp";
    EXPECT_GT(ramp_spread(startup), 80.0) << "the startup cube shows no ramp";
    const double startup_difference = mean_difference(startup, rgba8);
    GTEST_LOG_(INFO) << "startup (" << vk::to_string(format_of(0)) << ") against RGBA8: " << startup_difference;
    EXPECT_LT(startup_difference, 3.0) << "the startup cube's blocks draw unlike the RGBA8 chain";

    struct Family
    {
        bool sampled;
        Kataglyphis::BlockSupport alone;
        vk::Format format;
    };
    const uint32_t side = device_blocks.maxSide;
    const std::array<Family, 3> families{ {
      { device_blocks.bc, { .bc = true, .maxSide = side }, vk::Format::eBc7SrgbBlock },
      { device_blocks.etc2, { .etc2 = true, .maxSide = side }, vk::Format::eEtc2R8G8B8A8SrgbBlock },
      { device_blocks.astc, { .astc = true, .maxSide = side }, vk::Format::eAstc4x4SrgbBlock },
    } };
    uint32_t next = 2;
    for (const Family &family : families) {
        if (!family.sampled) {
            GTEST_LOG_(INFO) << vk::to_string(family.format) << ": not sampled by this device";
            continue;
        }
        ASSERT_TRUE(load_with(family.alone, next)) << vk::to_string(family.format) << " did not load";
        EXPECT_EQ(format_of(next), family.format);
        const std::vector<int> drawn = crop_of(next);
        ASSERT_FALSE(drawn.empty());
        const double difference = mean_difference(drawn, rgba8);
        GTEST_LOG_(INFO) << vk::to_string(family.format) << " against RGBA8: " << difference;
        EXPECT_LT(difference, 3.0) << vk::to_string(family.format) << " draws unlike the RGBA8 chain";
        EXPECT_GT(ramp_spread(drawn), 80.0) << vk::to_string(family.format) << " shows no ramp";
        ++next;
    }
    EXPECT_FALSE(harness.renderer->hasDeviceLost());
}

// glTF 3.9.2: a doubleSided back face reverses its whole tangent frame; flipping only N and B mirrored the relief.
TEST(GoldenRender, NormalMappedBackFaceFlipsItsWholeTangentFrame)
{
    SKIP_WITHOUT_GPU();

    // A white, rough dielectric whose normal map tilts every texel 45 degrees toward +T, +X on an unrotated card.
    const auto tilted =
      write_gltf_variant(NORMAL_MAP_CARD_MODEL, "kataglyphis_tilted_normal_card.gltf", [](auto &gltf) {
          auto &material = gltf["materials"][0];
          material["normalTexture"] = { { "index", 0 } };
          material["pbrMetallicRoughness"] = {
              { "baseColorFactor", { 1.0, 1.0, 1.0, 1.0 } }, { "metallicFactor", 0.0 }, { "roughnessFactor", 1.0 }
          };
          gltf["images"][0]["uri"] = TILTED_NORMAL_PNG;
      });
    ASSERT_TRUE(tilted.has_value()) << "could not write the tilted normal card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    // From +X and the camera's side: the front card's mapped normal (0.71, 0, 0.71) faces it almost squarely.
    scene_vars.directional_light_direction[0] = 1.0F;
    scene_vars.directional_light_direction[1] = 0.3F;
    scene_vars.directional_light_direction[2] = 1.0F;
    renderer_vars.pathTracingSamplesPerPixel = 4;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    // Turned about Y, the right card shows its back face; reversed whole, its mapped normal points as the left one's.
    const glm::mat4 front =
      glm::translate(glm::mat4(1.0F), glm::vec3(-2.5F, 4.0F, 15.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    const glm::mat4 back = glm::translate(glm::mat4(1.0F), glm::vec3(2.5F, 4.0F, 15.0F))
                           * glm::rotate(glm::mat4(1.0F), glm::pi<float>(), glm::vec3(0.0F, 1.0F, 0.0F))
                           * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    ASSERT_TRUE(harness.renderer->addModel(tilted->string(), front).has_value()) << "adding the front card failed";
    ASSERT_TRUE(harness.renderer->addModel(tilted->string(), back).has_value()) << "adding the back card failed";

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
        bool pathTracing;
        int frames;
    };
    std::vector<Mode> modes = { Mode{ "forward", RasterizationMode::Forward, false, false, SETTLE_FRAMES },
        Mode{ "deferred", RasterizationMode::Deferred, false, false, SETTLE_FRAMES } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back(Mode{ "ray traced", RasterizationMode::Forward, true, false, SETTLE_FRAMES });
        modes.push_back(Mode{ "path traced", RasterizationMode::Forward, false, true, 12 });
    }
    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = mode.pathTracing;
        harness.render_frames(mode.frames);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
        ASSERT_FALSE(frame.empty()) << mode.name;

        // The cards' centres project 207 px either side of the middle column.
        const uint32_t offset = (width * 207U) / WINDOW_WIDTH;
        const double front_luma = mean_luminance_in_square(frame, width, width / 2U - offset, height / 2U, 30U);
        const double back_luma = mean_luminance_in_square(frame, width, width / 2U + offset, height / 2U, 30U);
        GTEST_LOG_(INFO) << mode.name << ": front face " << front_luma << ", back face " << back_luma;

        ASSERT_GT(front_luma, 80.0) << mode.name << ": the front card is unlit - check the light or the placement";
        // A back face that kept the front's tangent turns its relief away from this light: N.L 0 against 0.98.
        EXPECT_GT(back_luma, 0.85 * front_luma)
          << mode.name << ": the back face's normal map is mirrored along its tangent - only N and B were flipped";
    }
}

// gltf_material.slang's tangent_space_normal: a texel with blue 0 (BC5) has its z rebuilt, so it shades as the full texel.
TEST(GoldenRender, TwoChannelNormalMapShadesAsItsThreeChannelTwin)
{
    SKIP_WITHOUT_GPU();

    const auto card = [](const char *file_name, const char *png) {
        return write_gltf_variant(NORMAL_MAP_CARD_MODEL, file_name, [png](auto &gltf) {
            auto &material = gltf["materials"][0];
            material["normalTexture"] = { { "index", 0 } };
            material["pbrMetallicRoughness"] = {
                { "baseColorFactor", { 1.0, 1.0, 1.0, 1.0 } }, { "metallicFactor", 0.0 }, { "roughnessFactor", 1.0 }
            };
            gltf["images"][0]["uri"] = png;
        });
    };
    const auto full = card("kataglyphis_three_channel_normal_card.gltf", TILTED_NORMAL_PNG);
    const auto two = card("kataglyphis_two_channel_normal_card.gltf", TILTED_TWO_CHANNEL_NORMAL_PNG);
    ASSERT_TRUE(full.has_value() && two.has_value()) << "could not write the normal cards";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    // As NormalMappedBackFaceFlipsItsWholeTangentFrame: the mapped normal (0.71, 0, 0.71) faces this light.
    scene_vars.directional_light_direction[0] = 1.0F;
    scene_vars.directional_light_direction[1] = 0.3F;
    scene_vars.directional_light_direction[2] = 1.0F;
    renderer_vars.pathTracingSamplesPerPixel = 4;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    const auto placed = [](float x) {
        return glm::translate(glm::mat4(1.0F), glm::vec3(x, 4.0F, 15.0F))
               * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    };
    ASSERT_TRUE(harness.renderer->addModel(full->string(), placed(-2.5F)).has_value()) << "adding the full card failed";
    ASSERT_TRUE(harness.renderer->addModel(two->string(), placed(2.5F)).has_value()) << "adding the BC5 card failed";

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
        bool pathTracing;
        int frames;
    };
    std::vector<Mode> modes = { Mode{ "forward", RasterizationMode::Forward, false, false, SETTLE_FRAMES },
        Mode{ "deferred", RasterizationMode::Deferred, false, false, SETTLE_FRAMES } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back(Mode{ "ray traced", RasterizationMode::Forward, true, false, SETTLE_FRAMES });
        modes.push_back(Mode{ "path traced", RasterizationMode::Forward, false, true, 12 });
    }
    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = mode.pathTracing;
        harness.render_frames(mode.frames);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
        ASSERT_FALSE(frame.empty()) << mode.name;

        const uint32_t offset = (width * 207U) / WINDOW_WIDTH;
        const double full_luma = mean_luminance_in_square(frame, width, width / 2U - offset, height / 2U, 30U);
        const double two_luma = mean_luminance_in_square(frame, width, width / 2U + offset, height / 2U, 30U);
        GTEST_LOG_(INFO) << mode.name << ": three-channel card " << full_luma << ", two-channel card " << two_luma;

        ASSERT_GT(full_luma, 80.0) << mode.name
                                   << ": the three-channel card is unlit - check the light or the placement";
        // Without the rebuild the texel decodes to z = -1, a normal behind the surface that this light cannot reach.
        EXPECT_NEAR(two_luma, full_luma, 0.1 * full_luma)
          << mode.name << ": the two-channel normal map did not rebuild z from x and y";
    }
}

// glTF 3.7.4: a mirroring model matrix reverses the winding and the tangent frame's handedness, not the relief.
TEST(GoldenRender, MirroredModelKeepsItsWindingAndNormalMapRelief)
{
    SKIP_WITHOUT_GPU();

    // A white, rough, single-sided dielectric whose normal map tilts every texel 45 degrees toward -B, +Y on this card.
    const auto tilted =
      write_gltf_variant(NORMAL_MAP_CARD_MODEL, "kataglyphis_bitangent_tilted_card.gltf", [](auto &gltf) {
          auto &material = gltf["materials"][0];
          material["normalTexture"] = { { "index", 0 } };
          material["pbrMetallicRoughness"] = {
              { "baseColorFactor", { 1.0, 1.0, 1.0, 1.0 } }, { "metallicFactor", 0.0 }, { "roughnessFactor", 1.0 }
          };
          material["doubleSided"] = false;
          gltf["images"][0]["uri"] = TILTED_AGAINST_BITANGENT_PNG;
      });
    ASSERT_TRUE(tilted.has_value()) << "could not write the tilted normal card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    // From above and behind the camera, in the mirror plane: the mapped normal (0, 0.71, 0.71) faces it squarely.
    scene_vars.directional_light_direction[0] = 0.0F;
    scene_vars.directional_light_direction[1] = 1.0F;
    scene_vars.directional_light_direction[2] = 1.0F;
    renderer_vars.pathTracingSamplesPerPixel = 4;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    // The right card is the left one scaled by -1 in x: same facing, same relief, reversed winding.
    const glm::mat4 upright =
      glm::translate(glm::mat4(1.0F), glm::vec3(-2.5F, 4.0F, 15.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    const glm::mat4 mirrored = glm::translate(glm::mat4(1.0F), glm::vec3(2.5F, 4.0F, 15.0F))
                               * glm::scale(glm::mat4(1.0F), glm::vec3(-2.0F, 2.0F, 2.0F));
    ASSERT_TRUE(harness.renderer->addModel(tilted->string(), upright).has_value()) << "adding the upright card failed";
    ASSERT_TRUE(harness.renderer->addModel(tilted->string(), mirrored).has_value())
      << "adding the mirrored card failed";

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
        bool pathTracing;
        int frames;
    };
    std::vector<Mode> modes = { Mode{ "forward", RasterizationMode::Forward, false, false, SETTLE_FRAMES },
        Mode{ "deferred", RasterizationMode::Deferred, false, false, SETTLE_FRAMES } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back(Mode{ "ray traced", RasterizationMode::Forward, true, false, SETTLE_FRAMES });
        modes.push_back(Mode{ "path traced", RasterizationMode::Forward, false, true, 12 });
    }
    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = mode.pathTracing;
        harness.render_frames(mode.frames);
        uint32_t width = 0;
        uint32_t height = 0;
        // HDR: the tonemap compresses the sun's share, and the path tracer's sky bounce alone reached 0.92 of it.
        const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
        ASSERT_FALSE(radiance.empty()) << mode.name;

        // The cards' centres project 207 px either side of the middle column.
        const uint32_t offset = (width * 207U) / WINDOW_WIDTH;
        const double upright_mean = mean_hdr_in_square(radiance, width, width / 2U - offset, height / 2U, 30U);
        const double mirrored_mean = mean_hdr_in_square(radiance, width, width / 2U + offset, height / 2U, 30U);
        GTEST_LOG_(INFO) << mode.name << ": upright card " << upright_mean << ", mirrored card " << mirrored_mean;

        ASSERT_GT(upright_mean, 0.05) << mode.name << ": the upright card is unlit - check the light or the placement";
        // Culled, the mirrored card shows what lies behind it; with its bitangent reversed, N.L is 0 against 1.
        EXPECT_NEAR(mirrored_mean, upright_mean, 0.1 * upright_mean)
          << mode.name << ": the mirrored card is culled by its reversed winding or its normal map's relief inverts";
    }
}

namespace {

// What one engine run of the material pipelines drew, picked and logged.
struct MaterialPipelineRun
{
    std::vector<std::vector<uint8_t>> frames;
    Kataglyphis::MaterialTextureIndexing indexing = Kataglyphis::MaterialTextureIndexing::NonUniform;
    int validationErrors = 0;
    bool skipped = false;
};

// The rig, then a MASK and a BLEND card, each one material per draw; shadowed forward with the prepass, then deferred.
MaterialPipelineRun render_material_pipelines(const std::string &blendCard, bool forceUniform)
{
    MaterialPipelineRun run;
    ScopedEnvVar force("KATAGLYPHIS_FORCE_UNIFORM_TEXTURE_INDEX", forceUniform ? "1" : "");
    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    // Before the harness, so creating the six material pipelines counts too.
    ScopedValidationErrorCounter validation;
    {
        EngineHarness harness;
        if (!Kataglyphis::TestSupport::frameCaptureAvailable(harness)) {
            run.skipped = true;
            return run;
        }
        run.indexing = harness.renderer->sharedDevice()->materialTextureIndexing();
        auto &renderer_vars = harness.useForwardRaster();
        // fs_depth_prepass's pipeline records only with SSAO on.
        renderer_vars.ssao_strength = 0.7F;
        harness.render_frames(WARMUP_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        run.frames.push_back(harness.capture_frame(width, height));

        const glm::mat4 blend_placement =
          glm::translate(glm::mat4(1.0F), glm::vec3(-5.0F, 4.0F, 15.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
        EXPECT_TRUE(harness.renderer->addModel(MASK_CARD_MODEL, panel_free_card_placement()).has_value())
          << "adding the MASK card failed";
        EXPECT_TRUE(harness.renderer->addModel(blendCard, blend_placement).has_value())
          << "adding the BLEND card failed";
        for (const RasterizationMode mode : { RasterizationMode::Forward, RasterizationMode::Deferred }) {
            renderer_vars.rasterizationMode = mode;
            harness.render_frames(SETTLE_FRAMES);
            run.frames.push_back(harness.capture_frame(width, height));
        }
        EXPECT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost drawing the material pipelines.";
    }
    run.validationErrors = validation.errorCount();
    return run;
}

}// namespace

// llvmpipe has dynamic indexing, so the twins are valid there; one material per draw keeps each slot uniform.
TEST(GoldenRender, UniformIndexTwinsDrawTheDefaultFramesWhenEachDrawHasOneMaterial)
{
    SKIP_WITHOUT_GPU();
    using Kataglyphis::MaterialTextureIndexing;

    const auto blend_card =
      write_gltf_variant(MASK_CARD_MODEL, "kataglyphis_blend_texture_card.gltf", [](nlohmann::json &gltf) {
          // A texel of its own, so a twin that sampled the MASK card's slot would draw the wrong colour.
          gltf["images"][0]["uri"] = TILTED_NORMAL_PNG;
          gltf["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = { 1.0, 1.0, 1.0, 0.5 };
          gltf["materials"][0]["alphaMode"] = "BLEND";
          gltf["materials"][0].erase("alphaCutoff");
      });
    ASSERT_TRUE(blend_card.has_value()) << "could not write the BLEND twin of " << MASK_CARD_MODEL;

    const MaterialPipelineRun defaults = render_material_pipelines(blend_card->string(), false);
    if (defaults.skipped) { GTEST_SKIP() << "frame capture is unsupported here"; }
    if (defaults.indexing != MaterialTextureIndexing::NonUniform) {
        GTEST_SKIP() << "this device has no default material shaders to compare the twins against";
    }
    const MaterialPipelineRun twins = render_material_pipelines(blend_card->string(), true);
    ASSERT_FALSE(twins.skipped);
    ASSERT_EQ(twins.indexing, MaterialTextureIndexing::Uniform)
      << "KATAGLYPHIS_FORCE_UNIFORM_TEXTURE_INDEX did not pick the *_uniform_index twins";
    EXPECT_EQ(defaults.validationErrors, 0) << "the default material pipelines logged an error";
    EXPECT_EQ(twins.validationErrors, 0) << "creating or drawing the twins' pipelines logged an error";

    constexpr std::array<const char *, 3> kFrames = { "forward before the cards", "forward", "deferred" };
    ASSERT_EQ(defaults.frames.size(), kFrames.size());
    ASSERT_EQ(twins.frames.size(), kFrames.size());
    const auto differing = [](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
        std::size_t count = 0;
        for (std::size_t pixel = 0; pixel < a.size() / 4U; ++pixel) {
            if (channel_delta(a, b, pixel) != 0) { ++count; }
        }
        return count;
    };
    for (std::size_t frame = 0; frame < kFrames.size(); ++frame) {
        ASSERT_FALSE(defaults.frames[frame].empty()) << kFrames[frame];
        ASSERT_EQ(twins.frames[frame].size(), defaults.frames[frame].size()) << kFrames[frame];
        EXPECT_EQ(differing(twins.frames[frame], defaults.frames[frame]), 0U)
          << kFrames[frame] << ": the twins drew pixels unlike the default material shaders";
    }
    // Control: the textured cards reach the compared pixels, so a twin that samples nothing would show.
    EXPECT_GT(differing(defaults.frames[0], defaults.frames[1]), 200U) << "the cards did not change the frame";
}
