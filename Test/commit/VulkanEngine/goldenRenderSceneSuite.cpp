// Goldens: added, reloaded and multi-primitive models draw with their own meshes and textures; the editor drives them.

#include "GoldenRenderHarness.hpp"

import kataglyphis.vulkan.renderer;

using namespace Kataglyphis::TestSupport::GoldenRender;

namespace {

using Kataglyphis::Test::GoldenMetrics::card_crop;

// A unit cube, metallic 0: a dielectric the library lists, so the editor's reload can pick it.
constexpr const char *DIELECTRIC_CUBE_MODEL = "Models/GltfTest/cube_textured.gltf";
// One mid-grey texel (128, 128, 128), as a glTF data URI.
constexpr const char *GREY_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mNoaGgAAAMEAYF1LgG8AAAAAElFTkSuQmCC";

}// namespace

// With one model objectIndex is always 0; this proves a second one renders, not the index arithmetic.
TEST(GoldenRender, SecondModelLoadsAndRenders)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.frustum_culling_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    const unsigned int meshes_with_one_model = harness.feedback.visibility.meshes_total;
    ASSERT_GT(meshes_with_one_model, 0U);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    // The large untextured rig cannot fail to change the frame if it renders.
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 2.0F, 12.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(0.15F));
    const std::optional<uint32_t> second = harness.renderer->addModel("Models/ShadowTest/shadow_rig.obj", placement);

    ASSERT_TRUE(second.has_value()) << "the second model failed to load";
    EXPECT_EQ(*second, 1U) << "the second model must be index 1 - this is the objectIndex the raster path pushes";

    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost after adding a model.";

    // The raster path must now consider more meshes than before.
    EXPECT_GT(harness.feedback.visibility.meshes_total, meshes_with_one_model)
      << "adding a model did not change how many meshes the raster path considered; "
         "the new model never reached the draw loop";

    const std::vector<uint8_t> after = harness.capture_frame(width, height);
    ASSERT_EQ(after.size(), before.size());

    size_t changed = 0;
    for (size_t pixel = 0; pixel + 3 < after.size(); pixel += 4) {
        if (std::abs(luminance_of(after, pixel / 4) - luminance_of(before, pixel / 4)) > 4.0) { ++changed; }
    }
    EXPECT_GT(changed, 500U) << "the second model loaded and was counted, but changed only " << changed
                             << " pixels - it is being drawn somewhere invisible, or not drawn at all";
}

// Two meshes, and each draws its own quad: a zeroed index re-base still builds two but feeds the second wrong vertices.
TEST(GoldenRender, MultiPrimitiveGltfLoadsAsMultipleMeshes)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride model_override(TWO_PRIMITIVE_MODEL);

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.frustum_culling_enabled = false;// count every mesh, framing aside
    harness.settings.scene.shadows_enabled = false;
    // Face-on to the two quads, red on x in [-1, 0] and green on [0, 1], across the frame's middle.
    constexpr float DISTANCE = 3.0F;
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, DISTANCE));
    harness.camera->set_orientation(-90.0F, 0.0F);

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost())
      << "Device lost rendering the two-primitive glTF - the split produced an invalid mesh.";

    EXPECT_EQ(harness.feedback.visibility.meshes_total, 2U)
      << "the two-primitive glTF must build two meshes (one per primitive), not one flattened mesh";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(radiance.empty()) << "the two-primitive glTF captured no frame";

    const glm::dvec4 left =
      mean_hdr_rgba_in_square(radiance, width, card_column(-0.5F, DISTANCE, width, height), height / 2U, 12U);
    const glm::dvec4 right =
      mean_hdr_rgba_in_square(radiance, width, card_column(0.5F, DISTANCE, width, height), height / 2U, 12U);
    GTEST_LOG_(INFO) << "left quad (" << left.r << ", " << left.g << ", " << left.b << "), right quad (" << right.r
                     << ", " << right.g << ", " << right.b << ")";
    // Dominance, not values: the metallic quads show only the sun's specular, whose radiance varies by driver.
    EXPECT_GT(left.r, 2.0 * std::max(left.g, left.b)) << "the left quad, material 0, does not draw red";
    EXPECT_GT(right.g, 2.0 * std::max(right.r, right.b)) << "the right quad, material 1, does not draw green";
}

// textureIDs are model-local, so a second model needs its offset or it samples the first model's slots.
TEST(GoldenRender, SecondModelShadesWithItsOwnTextures)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);

    // Sponza is the one bundled model with texture files; the others would sample the white default.
    const std::string sponza = sceneConfig::resolveModelPath("Models/crytek-sponza/sponza_triag.obj");
    if (!std::filesystem::exists(sponza)) { GTEST_SKIP() << "textured second model not present"; }

    // Sponza spans about +-1800 units; 0.01 fits it next to the rig inside the frame.
    const auto added = harness.renderer->addModel(sponza, glm::scale(glm::mat4(1.0F), glm::vec3(0.01F)));
    ASSERT_TRUE(added.has_value()) << "adding the second model failed";
    harness.render_frames(SETTLE_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(frame.empty());

    // Detail, not colour: the bricks are near-greyscale, but a flat white default has no detail.
    const double detail = detail_fraction(frame, width, height, card_crop(width, height));
    GTEST_LOG_(INFO) << "second-model texture-detail fraction (panel-free crop): " << detail;

    EXPECT_GT(detail, 0.02) << "The added model shows no texture detail - it is sampling the first "
                               "model's (flat white) texture slots.";
}

// RT/PT see only the acceleration structure, so addModel must rebuild it or the model is invisible to them.
TEST(GoldenRender, AddedModelAppearsInPathTracing)
{
    SKIP_WITHOUT_GPU();

    // The fixture leaves metallicFactor at glTF's default 1, which the Lambertian path tracer renders black.
    const auto card = write_gltf_variant(UV_TRANSFORM_MODEL,
      "kataglyphis_dielectric_uv_card.gltf",
      [](nlohmann::json &gltf) { gltf["materials"][0]["pbrMetallicRoughness"]["metallicFactor"] = 0.0; });
    ASSERT_TRUE(card.has_value()) << "could not write the dielectric twin of " << UV_TRANSFORM_MODEL;

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
    // The control gets the same accumulated history as the capture after the add, so noise reads alike in both.
    const int history = path_traced_history(60, 24);
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES + history);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    const auto added = harness.renderer->addModel(card->string(), panel_free_card_placement());
    ASSERT_TRUE(added.has_value()) << "adding the card failed";
    // The AS rebuild resets history; deepen it so noise does not swamp the detail.
    harness.render_frames(SETTLE_FRAMES);
    harness.render_frames(history);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(frame.empty());
    ASSERT_EQ(frame.size(), before.size());

    if (const auto dump = Kataglyphis::environmentVariable("KATAGLYPHIS_MASK_DUMP")) {
        const std::string base = *dump;
        stbi_write_png(
          (base + "-ptadd.png").c_str(), int(width), int(height), 4, frame.data(), static_cast<int>(width) * 4);
    }

    const Crop box = panel_free_card_box(width, height);
    // Under the dim cube map the slab's shadow is lit by interreflection alone; its noise steps stay under 48 levels.
    constexpr double CHECKER_STEP = 48.0;
    const double detail_before = detail_fraction(before, width, height, box, CHECKER_STEP);
    const double detail = detail_fraction(frame, width, height, box, CHECKER_STEP);
    GTEST_LOG_(INFO) << "added-model PT card-box detail fraction: before the add " << detail_before << ", after "
                     << detail;

    // Before the add stands in for a missing AS rebuild. Steps of 6 levels: RTX 2080 0.037 before, 0.328 after (2026-10-01).
    constexpr double DETAIL_THRESHOLD = 0.1;
    EXPECT_LT(detail_before, DETAIL_THRESHOLD) << "the card box already has detail without the card - check framing";
    EXPECT_GT(detail, DETAIL_THRESHOLD)
      << "the runtime-added card is not visible in path tracing - the AS was not rebuilt to include it";
}

namespace {

struct CheckerFractions
{
    double green = 0.0;
    double magenta = 0.0;
};

// cube_textured.gltf's green and magenta cells over the centre crop its front face fills from 2.2 units away.
CheckerFractions cube_checker_fractions(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height)
{
    size_t green = 0;
    size_t magenta = 0;
    size_t total = 0;
    for (uint32_t y = height / 2U - 150U; y < height / 2U + 150U; ++y) {
        for (uint32_t x = width / 2U - 150U; x < width / 2U + 150U; ++x) {
            const size_t base = (static_cast<size_t>(y) * width + x) * 4U;
            const int red = rgba[base];
            const int grn = rgba[base + 1U];
            if (grn > red + 40) { ++green; }
            if (red > grn + 40) { ++magenta; }
            ++total;
        }
    }
    return { static_cast<double>(green) / static_cast<double>(total),
        static_cast<double>(magenta) / static_cast<double>(total) };
}

}// namespace

// Drivers tolerate a descriptor on the destroyed TLAS, so validation errors are the primary oracle.
TEST(GoldenRender, ReloadedModelIsVisibleInPathTracing)
{
    SKIP_WITHOUT_GPU();

    // A small scene known good in PT; the dinosaur's large BLAS is not what this measures.
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
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    // A dielectric, so the frame measures the new model shaded; a metal card went black here, the rig still vanished.
    const int reload_index = sceneConfig::modelIndexOf(sceneConfig::getAvailableModelPaths(), DIELECTRIC_CUBE_MODEL);
    ASSERT_GE(reload_index, 0) << DIELECTRIC_CUBE_MODEL << " not found among the scanned models";

    ScopedValidationErrorCounter validation_errors;
    // The same flags the editor's library sets.
    scene_vars.selected_model_index = reload_index;
    scene_vars.model_reload_requested = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    // The cube sits at the origin with the identity; from here its front face fills the image's centre.
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.6F, 2.2F));
    harness.camera->set_orientation(-90.0F, -15.0F);
    harness.render_frames(30);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "the reload logged a Vulkan validation error - almost certainly the "
         "raytracing descriptor set still referencing the TLAS the AS rebuild destroyed";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> after = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(after.empty());

    // A stale TLAS shows the rig's grey, an empty one sky.
    const CheckerFractions checker = cube_checker_fractions(after, width, height);
    GTEST_LOG_(INFO) << "reloaded cube, centre crop: " << checker.green << " green and " << checker.magenta
                     << " magenta";

    EXPECT_GT(checker.green, 0.05) << "no green checker texels: the reloaded cube is not shaded in path tracing";
    EXPECT_GT(checker.magenta, 0.05) << "no magenta checker texels: the reloaded cube is not shaded in path tracing";
}

// A full rebuild counts two builds for the add and replaces the rig's BLAS; BlasReuse.hpp decides what stays.
TEST(GoldenRender, AddedModelBuildsOnlyItsOwnBlas)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    const std::vector<vk::AccelerationStructureKHR> loaded = harness.renderer->bottomLevelHandles();
    ASSERT_EQ(loaded.size(), 1U) << "the loaded rig has no BLAS";
    EXPECT_EQ(harness.renderer->bottomLevelBuildCount(), 1U);

    ScopedValidationErrorCounter validation_errors;
    const glm::mat4 beside = glm::translate(glm::mat4(1.0F), glm::vec3(3.0F, 1.0F, 8.0F));
    ASSERT_TRUE(harness.renderer->addModel(DIELECTRIC_CUBE_MODEL, beside).has_value()) << "adding the cube failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    const std::vector<vk::AccelerationStructureKHR> added = harness.renderer->bottomLevelHandles();
    ASSERT_EQ(added.size(), 2U);
    EXPECT_EQ(harness.renderer->bottomLevelBuildCount(), 2U)
      << "adding one model rebuilt the BLAS of the model already in the scene";
    EXPECT_EQ(added[0], loaded[0]) << "the rig's BLAS was replaced by an addModel";

    // A reload replaces model 0, so neither BLAS may stay: one build, for the reloaded model.
    const int reload_index = sceneConfig::modelIndexOf(sceneConfig::getAvailableModelPaths(), DIELECTRIC_CUBE_MODEL);
    ASSERT_GE(reload_index, 0) << DIELECTRIC_CUBE_MODEL << " not found among the scanned models";
    scene_vars.selected_model_index = reload_index;
    scene_vars.model_reload_requested = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    const std::vector<vk::AccelerationStructureKHR> reloaded = harness.renderer->bottomLevelHandles();
    ASSERT_EQ(reloaded.size(), 1U) << "the reload kept a BLAS of a model the scene no longer holds";
    EXPECT_EQ(harness.renderer->bottomLevelBuildCount(), 3U);

    // The next add appends to the reloaded scene, not to the BLAS the reload discarded.
    ASSERT_TRUE(harness.renderer->addModel(MASK_CARD_MODEL, beside).has_value()) << "adding the card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    const std::vector<vk::AccelerationStructureKHR> appended = harness.renderer->bottomLevelHandles();
    ASSERT_EQ(appended.size(), 2U);
    EXPECT_EQ(harness.renderer->bottomLevelBuildCount(), 4U);
    EXPECT_EQ(appended[0], reloaded[0]) << "the reloaded cube's BLAS was replaced by an addModel";
    EXPECT_EQ(validation_errors.errorCount(), 0) << "a traced frame or an AS build logged an error";
}

// Model 0 has two meshes and one texture, so the second model's hits need its mesh base and its texture offset.
TEST(GoldenRender, TracedSecondModelResolvesItsOwnMeshAndTextures)
{
    SKIP_WITHOUT_GPU();

    // The second primitive makes the cube's mesh base 2 against an instance index of 1; the grey texel fills slot 0.
    const auto first = write_gltf_variant(DIELECTRIC_CUBE_MODEL, "kataglyphis_grey_two_mesh_cube.gltf", [](auto &gltf) {
        gltf["images"][0]["uri"] = GREY_PNG;
        gltf["meshes"][0]["primitives"].push_back(gltf["meshes"][0]["primitives"][0]);
    });
    ASSERT_TRUE(first.has_value()) << "could not write the grey two-mesh cube";

    ScopedModelOverride first_model(first->string().c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.pathTracingSamplesPerPixel = 2;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_EQ(harness.scene->getMeshCountPerModel(), (std::vector<uint32_t>{ 2U }))
      << "the first cube is not two meshes";
    ASSERT_EQ(harness.scene->getTextureCount(0), 1U) << "the first cube must own texture slot 0";

    // Far enough along +X that the first cube stays out of the frame.
    const glm::mat4 placement = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 0.0F, 0.0F));
    ASSERT_TRUE(harness.renderer->addModel(DIELECTRIC_CUBE_MODEL, placement).has_value()) << "adding the cube failed";
    harness.camera->set_camera_position(glm::vec3(10.0F, 0.6F, 2.2F));
    harness.camera->set_orientation(-90.0F, -15.0F);

    const auto measure = [&](const char *mode, int frames) {
        harness.render_frames(frames);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        EXPECT_FALSE(harness.renderer->hasDeviceLost());
        if (frame.empty()) { return CheckerFractions{}; }
        const CheckerFractions checker = cube_checker_fractions(frame, width, height);
        GTEST_LOG_(INFO) << mode << ": second cube, centre crop: " << checker.green << " green and " << checker.magenta
                         << " magenta";
        return checker;
    };

    const CheckerFractions forward = measure("forward", SETTLE_FRAMES);
    ASSERT_GT(forward.green, 0.05) << "the forward frame shows no green checker - check the framing";
    ASSERT_GT(forward.magenta, 0.05) << "the forward frame shows no magenta checker - check the framing";

    renderer_vars.raytracing = true;
    const CheckerFractions traced = measure("ray traced", SETTLE_FRAMES);
    EXPECT_GT(traced.green, 0.05) << "ray traced: the second cube shades with the first's grey - its object index "
                                     "lost InstanceID()'s mesh base, or its texture its offset";
    EXPECT_GT(traced.magenta, 0.05) << "ray traced: the second cube's magenta checker cells are missing";

    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    const CheckerFractions path_traced = measure("path traced", 8);
    EXPECT_GT(path_traced.green, 0.05) << "path traced: the second cube shades with the first's grey - its object "
                                          "index lost CommittedInstanceID()'s mesh base, or its texture its offset";
    EXPECT_GT(path_traced.magenta, 0.05) << "path traced: the second cube's magenta checker cells are missing";
}

// The library marks what the startup load opened; it once marked the default scene whatever the override said.
TEST(GoldenRender, EditorMarksTheModelTheSceneLoaded)
{
    SKIP_WITHOUT_GPU();

    {
        ScopedModelOverride rig(SHADOW_RIG_MODEL);
        EngineHarness harness;
        // Visible, so the editor runs the code that marks the loaded row.
        harness.gui->setOverlayVisible(true);
        harness.render_frames(SETTLE_FRAMES);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());

        const int rig_row = sceneConfig::modelIndexOf(sceneConfig::getAvailableModelPaths(), SHADOW_RIG_MODEL);
        ASSERT_GE(rig_row, 0) << SHADOW_RIG_MODEL << " is not in the model library";
        EXPECT_EQ(harness.settings.scene.selected_model_index, rig_row)
          << "KATAGLYPHIS_MODEL_OVERRIDE loaded the shadow rig, but the library marks another row";
        EXPECT_EQ(harness.gui->loadedModelName(), std::string_view("ShadowTest/shadow_rig"))
          << "the library row's name";
    }
    {
        // A failed load leaves the scene empty, so no row may claim to be loaded.
        ScopedModelOverride missing("Models/ShadowTest/no_such_model.obj");
        EngineHarness harness;
        harness.gui->setOverlayVisible(true);
        harness.render_frames(SETTLE_FRAMES);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());

        EXPECT_EQ(harness.scene->getModelCount(), 0U);
        EXPECT_EQ(harness.settings.scene.selected_model_index, -1)
          << "nothing loaded, yet the library marks a row as loaded";
        EXPECT_TRUE(harness.gui->loadedModelName().empty()) << "nothing loaded, yet the editor names a model";
    }
}

// An absolute override loads a model the library lists under no row; the inspector once disabled its transform.
TEST(GoldenRender, EditorTransformsAModelTheLibraryDoesNotList)
{
    SKIP_WITHOUT_GPU();

    std::error_code absolute_error;
    const std::string unlisted =
      std::filesystem::absolute(sceneConfig::resolveModelPath(SHADOW_RIG_MODEL), absolute_error).string();
    ASSERT_FALSE(absolute_error) << "could not make the shadow rig's path absolute";
    ScopedModelOverride rig(unlisted.c_str());
    EngineHarness harness;
    harness.gui->setOverlayVisible(true);
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(harness.scene->getModelCount(), 1U) << "the absolute override " << unlisted << " did not load";

    auto &scene_vars = harness.settings.scene;
    EXPECT_EQ(scene_vars.selected_model_index, -1) << "no library row holds an absolute path";
    // The inspector enables Position, Rotation and Reset exactly when the editor names a model.
    EXPECT_EQ(harness.gui->loadedModelName(), std::string_view("shadow_rig"))
      << "the editor names no model for the scene's unlisted one, so its transform controls stay disabled";

    // What those controls write; the renderer must move the unlisted model with it.
    const std::array<float, 3> position = { 1.0F, 2.0F, 3.0F };
    std::copy(position.begin(), position.end(), std::begin(scene_vars.model_position));
    scene_vars.model_transform_changed = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    EXPECT_EQ(harness.scene->getModelMatrix(0),
      Kataglyphis::makeGuiModelTransform(
        std::span<const float, 3>(scene_vars.model_position), std::span<const float, 3>(scene_vars.model_rotation)))
      << "the transform did not reach the model the library does not list";
}

// A load places the model at the identity, so the inspector must too; it once kept the previous model's transform.
TEST(GoldenRender, ReloadResetsTheInspectorTransform)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    harness.render_frames(WARMUP_FRAMES);

    const std::array<float, 3> position = { 1.0F, 2.0F, 3.0F };
    const std::array<float, 3> rotation = { 10.0F, 20.0F, 30.0F };
    std::copy(position.begin(), position.end(), std::begin(scene_vars.model_position));
    std::copy(rotation.begin(), rotation.end(), std::begin(scene_vars.model_rotation));
    scene_vars.model_transform_changed = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_NE(harness.scene->getModelMatrix(0), glm::mat4(1.0F)) << "the transform never reached the model";

    const int reload_row = sceneConfig::modelIndexOf(sceneConfig::getAvailableModelPaths(), UV_TRANSFORM_MODEL);
    ASSERT_GE(reload_row, 0) << UV_TRANSFORM_MODEL << " is not in the model library";
    scene_vars.selected_model_index = reload_row;
    scene_vars.model_reload_requested = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(harness.scene->getModelCount(), 1U) << "the reload failed";

    const glm::mat4 shown = Kataglyphis::makeGuiModelTransform(
      std::span<const float, 3>(scene_vars.model_position), std::span<const float, 3>(scene_vars.model_rotation));
    EXPECT_EQ(shown, harness.scene->getModelMatrix(0))
      << "the inspector shows position (" << scene_vars.model_position[0] << ", " << scene_vars.model_position[1]
      << ", " << scene_vars.model_position[2] << ") for a model the reload placed elsewhere";
    EXPECT_EQ(scene_vars.selected_model_index, reload_row) << "the library does not mark the reloaded model";
}

namespace {

// Fixed-seed PRNG so a failing sweep iteration can be re-hit exactly.
struct SweepRng
{
    uint64_t state;
    uint32_t next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(state >> 32U);
    }
    float uf(float lo, float hi) { return lo + (hi - lo) * (static_cast<float>(next()) / 4294967296.0F); }
    int ui(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
    bool ub() { return (next() & 1U) != 0U; }
};

}// namespace

// Random combinations within GUI.cpp's slider limits; no pixels asserted, since every state differs.
TEST(GoldenRender, GuiInputSweepNeverCrashesOrLosesTheDevice)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    const bool rt_supported = harness.renderer->supportsHardwareRaytracing();

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    auto &s = harness.settings.scene;
    auto &r = harness.settings.renderer;

    // Random sampling almost never hits the all-maximum corner, the state most likely to exhaust the GPU.
    if (rt_supported) {
        r.pathTracing = true;
        r.raytracing = false;
    }
    r.rasterizationMode = RasterizationMode::Deferred;
    r.pathTracingSamplesPerPixel = 64;
    r.pathTracingMaxBounces = 16;
    r.frustum_culling_enabled = false;
    r.fxaaMode = FxaaMode::Always;
    s.skybox_enabled = true;
    s.directional_light_radiance = 50.0F;
    s.shadows_enabled = true;
    s.num_shadow_cascades = 8;
    s.shadow_map_res_index = 3;// 4096
    s.pcf_radius = MAX_PCF_RADIUS;
    s.cascaded_shadow_intensity = 1.0F;
    s.shadow_distance = 200.0F;
    s.shadow_resolution_changed = true;
    s.clouds_enabled = true;
    s.cloud_num_march_steps = 128;
    s.cloud_num_march_steps_to_light = 128;
    s.cloud_density_multiplier = 1.0F;
    s.cloud_coverage_threshold = 1.0F;
    s.cloud_pillowness = 1.0F;
    s.cloud_cirrus_effect = 1.0F;
    s.cloud_powder_effect = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost on the all-maximum GUI state";
    {
        uint32_t bw = 0;
        uint32_t bh = 0;
        const std::vector<uint8_t> heavy_frame = harness.capture_frame(bw, bh);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost capturing the all-maximum GUI state";
        ASSERT_FALSE(heavy_frame.empty()) << "Empty frame on the all-maximum GUI state";
    }

    SweepRng rng{ 0x9E3779B97F4A7C15ULL };// fixed seed -> deterministic sweep

    constexpr int SWEEP_ITERATIONS = 16;
    for (int iter = 0; iter < SWEEP_ITERATIONS; ++iter) {
        // Modes are mutually exclusive, as the engine drives them.
        const int mode = rng.ui(0, rt_supported ? 3 : 1);
        r.raytracing = (mode == 2);
        r.pathTracing = (mode == 3);
        r.rasterizationMode = (mode == 1) ? RasterizationMode::Deferred : RasterizationMode::Forward;
        r.frustum_culling_enabled = rng.ub();
        r.pathTracingSamplesPerPixel = rng.ui(1, 64);
        r.pathTracingMaxBounces = rng.ui(1, 16);
        r.fxaaMode = static_cast<FxaaMode>(rng.ui(0, 2));

        s.skybox_enabled = rng.ub();
        s.directional_light_radiance = rng.uf(0.0F, 50.0F);
        for (float &c : s.directional_light_color) { c = rng.uf(0.0F, 1.0F); }
        for (float &d : s.directional_light_direction) { d = rng.uf(-1.0F, 1.0F); }

        s.shadows_enabled = rng.ub();
        s.num_shadow_cascades = rng.ui(1, 8);// slider allows 1..8 (clamped to CASCADE_COUNT in use)
        s.shadow_map_res_index = rng.ui(0, 3);// combo: 512 / 1024 / 2048 / 4096
        s.pcf_radius = rng.ui(1, MAX_PCF_RADIUS);
        s.cascaded_shadow_intensity = rng.uf(0.0F, 1.0F);
        s.shadow_distance = rng.uf(1.0F, 200.0F);
        // Force the shadow map to honour the new cascade count / resolution.
        s.shadow_resolution_changed = true;

        s.clouds_enabled = rng.ub();
        s.cloud_num_march_steps = rng.ui(1, 128);
        s.cloud_num_march_steps_to_light = rng.ui(1, 128);
        s.cloud_density_multiplier = rng.uf(0.0F, 1.0F);
        s.cloud_coverage_threshold = rng.uf(0.0F, 1.0F);
        s.cloud_pillowness = rng.uf(0.0F, 1.0F);
        s.cloud_cirrus_effect = rng.uf(0.0F, 1.0F);
        s.cloud_powder_effect = rng.ub();
        s.cloud_half_resolution = rng.ub();

        const std::string desc = "iter " + std::to_string(iter) + ": mode=" + std::to_string(mode)
                                 + " shadows=" + std::to_string(static_cast<int>(s.shadows_enabled))
                                 + " cascades=" + std::to_string(s.num_shadow_cascades)
                                 + " res=" + std::to_string(s.shadow_map_res_index)
                                 + " clouds=" + std::to_string(static_cast<int>(s.clouds_enabled))
                                 + " radiance=" + std::to_string(s.directional_light_radiance);

        // A few frames let a shadow-map re-init settle.
        harness.render_frames(SETTLE_FRAMES);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while rendering " << desc;

        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while capturing " << desc;
        ASSERT_FALSE(frame.empty()) << "Empty frame for " << desc;
    }
}
