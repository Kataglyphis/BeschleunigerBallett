// Both renderers' defaults, bounds and clamps lived twice, each side tested only against its own literals.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

#include "common/SceneUboMarshal.hpp"
#include "common/host_device_shared_vars.hpp"
#include "oxidant_bridge/settings.h"
#include "scene/atmospheric_effects/clouds/CloudDispatch.hpp"

import kataglyphis.vulkan.gui_scene_shared_vars;

namespace {

namespace ox = kataglyphis::oxidant::settings;

}// namespace

// cxx gives the bridged structs no member defaults, so every Rust value here comes from a call.
TEST(SceneSettingsDrift, DefaultsMatchOxidantsFieldByField)
{
    const GUISceneSharedVars defaults{};
    const ox::SceneSettings rust = ox::default_scene_settings();

    EXPECT_EQ(defaults.clouds_enabled, rust.clouds.enabled);
    EXPECT_EQ(defaults.cloud_num_march_steps, rust.clouds.march_steps);
    EXPECT_EQ(defaults.cloud_num_march_steps_to_light, rust.clouds.light_steps);
    EXPECT_EQ(defaults.cloud_density_multiplier, rust.clouds.density);
    EXPECT_EQ(defaults.cloud_coverage_threshold, rust.clouds.coverage);
    EXPECT_EQ(defaults.cloud_pillowness, rust.clouds.pillowness);
    EXPECT_EQ(defaults.cloud_cirrus_effect, rust.clouds.cirrus);
    EXPECT_EQ(defaults.cloud_powder_effect, rust.clouds.powder);
    EXPECT_EQ(defaults.cloud_half_resolution, rust.clouds.half_resolution);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        EXPECT_EQ(defaults.cloud_mesh_scale[axis], rust.clouds.scale[axis]) << "scale axis " << axis;
        EXPECT_EQ(defaults.cloud_mesh_offset[axis], rust.clouds.offset[axis]) << "offset axis " << axis;
        EXPECT_EQ(defaults.cloud_wind[axis], rust.clouds.wind[axis]) << "wind axis " << axis;
    }

    EXPECT_EQ(defaults.shadows_enabled, rust.shadows.enabled);
    // The editor stores an index into kShadowMapResolutions, OxidANT the texels.
    EXPECT_EQ(Kataglyphis::shadowResolutionForIndex(defaults.shadow_map_res_index), rust.shadows.resolution);
    EXPECT_EQ(static_cast<std::uint32_t>(defaults.num_shadow_cascades), rust.shadows.cascade_count);
    EXPECT_EQ(static_cast<std::uint32_t>(defaults.pcf_radius), rust.shadows.pcf_radius);
    EXPECT_EQ(defaults.cascaded_shadow_intensity, rust.shadows.strength);
    EXPECT_EQ(defaults.shadow_distance, rust.shadows.distance);
    EXPECT_EQ(defaults.cascade_split_lambda, rust.shadows.split_lambda);
    EXPECT_EQ(defaults.punctual_shadows_enabled, rust.shadows.punctual);
}

TEST(SceneSettingsDrift, RangesMatchTheEditorsBounds)
{
    const ox::SettingRanges rust = ox::setting_ranges();
    const auto expectRange = [](const ox::FloatRange &range, float min, float max, const char *what) {
        EXPECT_EQ(range.min, min) << what;
        EXPECT_EQ(range.max, max) << what;
    };

    EXPECT_EQ(rust.clouds.march_steps.min, Kataglyphis::kMinCloudMarchSteps);
    EXPECT_EQ(rust.clouds.march_steps.max, Kataglyphis::kMaxCloudMarchSteps);
    EXPECT_EQ(rust.clouds.light_steps.min, Kataglyphis::kMinCloudLightMarchSteps);
    EXPECT_EQ(rust.clouds.light_steps.max, Kataglyphis::kMaxCloudLightMarchSteps);
    expectRange(rust.clouds.density, Kataglyphis::kMinCloudDensityMultiplier, kMaxCloudDensityMultiplier, "density");
    expectRange(rust.clouds.coverage, kMinUnitSetting, kMaxUnitSetting, "coverage");
    expectRange(rust.clouds.pillowness, kMinUnitSetting, kMaxUnitSetting, "pillowness");
    expectRange(rust.clouds.cirrus, kMinUnitSetting, kMaxUnitSetting, "cirrus");
    expectRange(rust.clouds.scale, Kataglyphis::kMinCloudMeshExtent, kMaxCloudMeshExtent, "scale");
    expectRange(rust.clouds.offset, kMinCloudMeshOffset, kMaxCloudMeshOffset, "offset");
    expectRange(rust.clouds.wind, -kMaxCloudWindSpeed, kMaxCloudWindSpeed, "wind");

    ASSERT_EQ(rust.shadows.resolutions.size(), static_cast<std::size_t>(kShadowMapResolutionCount));
    for (std::size_t i = 0; i < rust.shadows.resolutions.size(); ++i) {
        EXPECT_EQ(rust.shadows.resolutions[i], kShadowMapResolutions[i]) << "resolution " << i;
    }
    // The editor's cascade and PCF controls offer 1..MAX_CASCADES and 0..MAX_PCF_RADIUS.
    EXPECT_EQ(rust.shadows.cascade_count.min, 1U);
    EXPECT_EQ(rust.shadows.cascade_count.max, static_cast<std::uint32_t>(MAX_CASCADES));
    EXPECT_EQ(rust.shadows.pcf_radius.min, 0U);
    EXPECT_EQ(rust.shadows.pcf_radius.max, static_cast<std::uint32_t>(MAX_PCF_RADIUS));
    expectRange(rust.shadows.strength, kMinUnitSetting, kMaxUnitSetting, "strength");
    expectRange(rust.shadows.distance, kMinShadowDistance, kMaxShadowDistance, "distance");
    expectRange(rust.shadows.split_lambda, kMinUnitSetting, kMaxUnitSetting, "split lambda");
}

// Only what both clamp is compared; NaN differs (Rust's max takes the floor, std::max keeps NaN), an open decision.
TEST(SceneSettingsDrift, SanitizeClampsWhatTheEngineClamps)
{
    for (const int steps : { -5, 0, 3, 4, 77, 128, 129, 100000 }) {
        ox::SceneSettings settings = ox::default_scene_settings();
        settings.clouds.march_steps = steps;
        settings.clouds.light_steps = steps;
        const ox::SceneSettings sanitized = ox::sanitize(settings);
        EXPECT_EQ(sanitized.clouds.march_steps, Kataglyphis::clampCloudMarchSteps(steps)) << steps << " march steps";
        EXPECT_EQ(sanitized.clouds.light_steps, Kataglyphis::clampCloudLightMarchSteps(steps))
          << steps << " light steps";
    }

    for (const float value : { -3.0F, 0.0F, 1e-4F, 1e-3F, 0.5F, 630.0F }) {
        ox::SceneSettings settings = ox::default_scene_settings();
        settings.clouds.scale = { value, 3.15F, value };
        settings.clouds.density = value;
        const ox::SceneSettings sanitized = ox::sanitize(settings);
        const glm::vec4 clamped = Kataglyphis::clampCloudMeshScale(glm::vec3(value, 3.15F, value), value);
        EXPECT_EQ(sanitized.clouds.scale[0], clamped.x) << value;
        EXPECT_EQ(sanitized.clouds.scale[1], clamped.y) << value;
        EXPECT_EQ(sanitized.clouds.scale[2], clamped.z) << value;
        EXPECT_EQ(sanitized.clouds.density, clamped.w) << value;
    }

    for (const int radius : { 0, 2, 5, 6, 40 }) {
        ox::SceneSettings settings = ox::default_scene_settings();
        settings.shadows.pcf_radius = static_cast<std::uint32_t>(radius);
        EXPECT_EQ(ox::sanitize(settings).shadows.pcf_radius, Kataglyphis::clampPcfRadius(radius)) << radius;
    }

    // Every resolution the editor can pick survives OxidANT's clamp to a listed one.
    for (int index = -1; index <= kShadowMapResolutionCount; ++index) {
        ox::SceneSettings settings = ox::default_scene_settings();
        settings.shadows.resolution = Kataglyphis::shadowResolutionForIndex(index);
        EXPECT_EQ(ox::sanitize(settings).shadows.resolution, Kataglyphis::shadowResolutionForIndex(index)) << index;
    }
}

// Not bridged: the C++ Sun moves with its ambient term (BACKLOG "One set of defaults"), then compare it here.
TEST(SceneSettingsDrift, TheSunIsTheNamedDifferenceUntilOneSetOfDefaults)
{
    const GUISceneSharedVars defaults{};
    constexpr const char *kWhy =
      "the C++ Sun differs from OxidANT's until BACKLOG.md \"One set of defaults: the Sun, "
      "the wind and F5\"; once that lands, compare it with OxidANT's here";
    EXPECT_EQ(defaults.directional_light_radiance, 10.0F) << kWhy;
    EXPECT_EQ(
      glm::vec3(
        defaults.directional_light_color[0], defaults.directional_light_color[1], defaults.directional_light_color[2]),
      glm::vec3(1.0F))
      << kWhy;
    EXPECT_EQ(glm::vec3(defaults.directional_light_direction[0],
                defaults.directional_light_direction[1],
                defaults.directional_light_direction[2]),
      glm::vec3(-0.55F, -1.0F, -0.35F))
      << kWhy;
}
