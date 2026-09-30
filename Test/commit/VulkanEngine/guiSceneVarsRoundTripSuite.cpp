// The renderer reads Scene's copy of GUISceneSharedVars, not the GUI's; this proves a field arrives, not its use.

#include <gtest/gtest.h>

#include "common/host_device_shared_vars.hpp"

#include <cstddef>
#include <cstring>
#include <iterator>
#include <new>
#include <string>
#include <type_traits>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.gui_scene_shared_vars;

using Kataglyphis::shadowResolutionForIndex;

namespace {

// Size when makeNonDefaultVars() was last checked against the struct.
constexpr std::size_t GUI_SCENE_SHARED_VARS_EXPECTED_SIZE = 184;

// Must stay exhaustive: a field forgotten here goes untested.
GUISceneSharedVars makeNonDefaultVars()
{
    // Value-initialised; necessary but not sufficient for the memcmp, see AllFieldsSurviveTheRoundTrip.
    GUISceneSharedVars vars{};
    vars.directional_light_radiance = 3.5F;
    vars.directional_light_color[0] = 0.25F;
    vars.directional_light_color[1] = 0.5F;
    vars.directional_light_color[2] = 0.75F;
    vars.directional_light_direction[0] = -0.1F;
    vars.directional_light_direction[1] = -0.2F;
    vars.directional_light_direction[2] = -0.3F;
    vars.shadow_map_res_index = 1;
    vars.shadow_resolution_changed = true;
    vars.num_shadow_cascades = 2;
    vars.pcf_radius = 4;
    vars.cascaded_shadow_intensity = 0.125F;
    vars.shadow_distance = 42.0F;
    vars.cascade_split_lambda = 0.75F;
    vars.cloud_num_march_steps = 16;
    vars.cloud_num_march_steps_to_light = 5;
    vars.cloud_density_multiplier = 0.11F;
    vars.cloud_coverage_threshold = 0.22F;
    vars.cloud_pillowness = 0.33F;
    vars.cloud_cirrus_effect = 0.44F;
    vars.cloud_powder_effect = false;
    vars.clouds_enabled = true;
    vars.cloud_mesh_scale[0] = 10.0F;
    vars.cloud_mesh_scale[1] = 11.0F;
    vars.cloud_mesh_scale[2] = 12.0F;
    vars.cloud_mesh_offset[0] = 13.0F;
    vars.cloud_mesh_offset[1] = 14.0F;
    vars.cloud_mesh_offset[2] = 15.0F;
    vars.shadows_enabled = false;
    vars.skybox_enabled = false;
    vars.camera_fov = 77.0F;
    vars.selected_model_index = 7;
    vars.model_reload_requested = true;
    vars.model_transform_changed = true;
    vars.model_position[0] = 1.5F;
    vars.model_position[1] = 2.5F;
    vars.model_position[2] = 3.5F;
    vars.model_rotation[0] = 4.5F;
    vars.model_rotation[1] = 5.5F;
    vars.model_rotation[2] = 6.5F;
    return vars;
}

}// namespace

// Scene::update_user_input's assignment; a member that does not copy cleanly feeds the renderer a wrong value.
TEST(GuiSceneVarsRoundTrip, AllFieldsSurviveTheRoundTrip)
{
    // memcmp reads padding that neither copy nor value-init must write (clang skips it), so zero the storage first.
    static_assert(std::is_trivially_copyable_v<GUISceneSharedVars>,
      "this test compares object representations; that is only meaningful for a trivially copyable type");

    alignas(GUISceneSharedVars) unsigned char source_storage[sizeof(GUISceneSharedVars)]{};
    alignas(GUISceneSharedVars) unsigned char destination_storage[sizeof(GUISceneSharedVars)]{};

    const GUISceneSharedVars *source = new (source_storage) GUISceneSharedVars{ makeNonDefaultVars() };
    GUISceneSharedVars *destination = new (destination_storage) GUISceneSharedVars{};

    *destination = *source;// exactly what Scene::update_user_input does

    EXPECT_EQ(std::memcmp(source, destination, sizeof(GUISceneSharedVars)), 0)
      << "GUISceneSharedVars did not copy cleanly";
}

// A field missing from makeNonDefaultVars() compares equal at its default; only a pinned size catches it.
TEST(GuiSceneVarsRoundTrip, StructSizeIsPinnedSoNewFieldsCannotBeForgotten)
{
    // On failure, set the new field in makeNonDefaultVars(), then update the number.
    EXPECT_EQ(sizeof(GUISceneSharedVars), GUI_SCENE_SHARED_VARS_EXPECTED_SIZE)
      << "GUISceneSharedVars changed size: add the new field to makeNonDefaultVars(), "
         "then update GUI_SCENE_SHARED_VARS_EXPECTED_SIZE";
}

// Named individually so a failure names the broken control.
TEST(GuiSceneVarsRoundTrip, ShadowControlsArrive)
{
    const GUISceneSharedVars source = makeNonDefaultVars();
    GUISceneSharedVars destination{};
    destination = source;

    EXPECT_EQ(destination.shadows_enabled, source.shadows_enabled);
    EXPECT_EQ(destination.num_shadow_cascades, source.num_shadow_cascades);
    EXPECT_EQ(destination.pcf_radius, source.pcf_radius);
    EXPECT_FLOAT_EQ(destination.cascaded_shadow_intensity, source.cascaded_shadow_intensity);
    EXPECT_FLOAT_EQ(destination.shadow_distance, source.shadow_distance);
    EXPECT_FLOAT_EQ(destination.cascade_split_lambda, source.cascade_split_lambda);
    EXPECT_EQ(destination.shadow_map_res_index, source.shadow_map_res_index);
}

// Shared storage would retroactively change what the renderer already read this frame.
TEST(GuiSceneVarsRoundTrip, TheCopyIsIndependentOfItsSource)
{
    GUISceneSharedVars source = makeNonDefaultVars();
    GUISceneSharedVars destination{};
    destination = source;

    source.cascaded_shadow_intensity = 0.9F;
    source.shadows_enabled = true;
    source.model_position[1] = 99.0F;

    EXPECT_FLOAT_EQ(destination.cascaded_shadow_intensity, 0.125F) << "the copy aliases its source";
    EXPECT_FALSE(destination.shadows_enabled);
    EXPECT_FLOAT_EQ(destination.model_position[1], 2.5F);
}

// Raising cascade_split_lambda worsens the shipped scene; too many cascades overrun the SceneUBO array.
TEST(GuiSceneVarsRoundTrip, ShippedDefaultsAreTheMeasuredOnes)
{
    const GUISceneSharedVars defaults;

    EXPECT_FLOAT_EQ(defaults.cascade_split_lambda, 0.0F)
      << "lambda defaults to uniform splits deliberately; raising it needs a measurement, not a hunch";
    EXPECT_FLOAT_EQ(defaults.shadow_distance, 60.0F)
      << "a non-positive shadow distance silently disables the clamp";
    EXPECT_GT(defaults.num_shadow_cascades, 0);
    EXPECT_LE(defaults.num_shadow_cascades, MAX_CASCADES) << "must stay <= MAX_CASCADES (SceneUBO array size)";
    EXPECT_TRUE(defaults.shadows_enabled) << "the debug scene exists to show shadows";
}

// The slider offers exactly 1..MAX_CASCADES.
TEST(GuiSceneVarsRoundTrip, CascadeCountDefaultIsWithinTheSliderRange)
{
    const GUISceneSharedVars defaults;

    EXPECT_LE(defaults.num_shadow_cascades, MAX_CASCADES);
}

// The GUI and Camera each hold a fov default, which would otherwise drift apart silently.
TEST(GuiSceneVarsRoundTrip, CameraFovDefaultMatchesTheCameraAndSitsInTheSliderRange)
{
    const GUISceneSharedVars defaults;
    const Camera camera;

    EXPECT_FLOAT_EQ(defaults.camera_fov, camera.get_fov());
    EXPECT_GE(defaults.camera_fov, 20.0F);
    EXPECT_LE(defaults.camera_fov, 110.0F);
}

// The combo labels are text, so each must be pinned to the pixel count it promises.
TEST(ShadowResolutionUnit, EveryComboLabelMatchesThePixelCount)
{
    const GUISceneSharedVars defaults;
    for (int i = 0; i < kShadowMapResolutionCount; ++i) {
        EXPECT_EQ(std::stoul(defaults.available_shadow_map_resolutions[i]), shadowResolutionForIndex(i))
          << "label/pixel-count mismatch at index " << i;
    }
}

TEST(ShadowResolutionUnit, OutOfRangeIndicesClampInsteadOfSilentlyPicking512)
{
    EXPECT_EQ(shadowResolutionForIndex(-1), 512U);
    EXPECT_EQ(shadowResolutionForIndex(kShadowMapResolutionCount),
      kShadowMapResolutions[kShadowMapResolutionCount - 1]);
}

// Two separately initialised arrays; nothing else stops their lengths drifting.
TEST(ShadowResolutionUnit, TheLabelTableAndThePixelTableAreTheSameLength)
{
    const GUISceneSharedVars defaults;
    EXPECT_EQ(std::size(defaults.available_shadow_map_resolutions), std::size(kShadowMapResolutions));
}

// VulkanRenderer::init allocates the GUI's default index at startup.
TEST(ShadowResolutionUnit, TheDefaultIndexIsWhatStartupAllocates)
{
    const GUISceneSharedVars defaults;
    EXPECT_EQ(shadowResolutionForIndex(defaults.shadow_map_res_index), 2048U);
}
