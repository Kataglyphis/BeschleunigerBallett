module;

#include <cstddef>
#include <cstdint>
#include <iterator>

export module kataglyphis.vulkan.gui_scene_shared_vars;

// The one table of shadow-map resolutions; the combo's labels
// (GUISceneSharedVars::available_shadow_map_resolutions) and every consumer
// of shadow_map_res_index must derive from this array instead of re-typing
// their own copy, which is what let the GUI's label and the renderer's
// startup allocation drift apart (GUI claimed 4096, renderer built 2048).
export constexpr uint32_t kShadowMapResolutions[] = { 512, 1024, 2048, 4096 };

export inline constexpr int kShadowMapResolutionCount = static_cast<int>(std::size(kShadowMapResolutions));

// Range for the "Shadow distance" slider (GUI.cpp). One owner for the bounds,
// the same way kShadowMapResolutions is the one owner of the resolution combo.
export inline constexpr float kMinShadowDistance = 5.0F;
export inline constexpr float kMaxShadowDistance = 500.0F;

// Clamps out-of-range indices into [0, kShadowMapResolutionCount - 1] rather
// than silently falling through to 512 the way the old if/else chain did.
//
// In `Kataglyphis`, where the rest of the engine's API lives, and NOT at global
// scope - this was the only exported free function in the codebase that sat
// outside a namespace, and that is not merely a style point. Doxygen 1.15
// indexes an exported entity under both its .ixx file compound and its C++20
// module compound; it deduplicates namespace members, but for a global-scope
// free function Breathe is left with two IDENTICAL candidates and refuses to
// resolve either ("Unable to resolve function shadowResolutionForIndex with
// arguments (int) - Potential matches:" listing the same signature twice).
// Under Sphinx's -W that failed the entire docs build. Measured 2026-08-06,
// same tree, one variable: global scope 1 warning, namespaced 0.
//
// Callers inside Kataglyphis:: (VulkanRenderer's members) need no change -
// unqualified lookup finds it in the enclosing namespace.
export namespace Kataglyphis {

constexpr uint32_t shadowResolutionForIndex(int index)
{
    if (index < 0) index = 0;
    if (index >= kShadowMapResolutionCount) index = kShadowMapResolutionCount - 1;
    return kShadowMapResolutions[index];
}

}// namespace Kataglyphis

export struct GUISceneSharedVars
{
    float directional_light_radiance = 10.f;
    float directional_light_color[3] = { 1.f, 1.f, 1.f };
    // Angled rather than near-vertical: a straight-down sun drops every shadow
    // directly underneath its caster, where it is barely visible. This throws
    // the dinosaurs' shadows sideways across their ground plane, which is what
    // makes the cascaded shadow maps read at a glance.
    float directional_light_direction[3] = { -0.55f, -1.f, -0.35f };

    // Shadows
    // Index 2 ("2048") matches the pixel count VulkanRenderer::init actually
    // allocates at startup (via shadowResolutionForIndex below) without
    // needing a measurement of the 4x memory jump index 3 would cost.
    int shadow_map_res_index = 2;
    bool shadow_resolution_changed = false;
    int num_shadow_cascades = 3;// must stay <= MAX_CASCADES (SceneUBO array size)
    int pcf_radius = 2;
    float cascaded_shadow_intensity = 0.65f;
    // How far shadows are fitted, independent of the camera far plane. The
    // camera sees 150 units in debug but the scene ends at ~36, and fitting
    // cascades to the far plane spread the shadow map over empty space
    // (measured 3.80 cm/texel over the subject, vs 3.04 at this distance).
    // Geometry past this is unshadowed, which is the intended trade.
    float shadow_distance = 60.f;
    // Blend between logarithmic (1) and uniform (0) cascade splits. Defaults
    // OFF because measurement did not support turning it on for the scenes we
    // ship: the win here came entirely from shadow_distance above. Raise it
    // only for a camera that sits close to its subject, where it does help
    // (1.52 -> 1.01 cm/texel at 0.35). See the table in CascadedShadowMap.cpp.
    float cascade_split_lambda = 0.f;
    // Labels of kShadowMapResolutions, in the same order; index i's label
    // must always read as the pixel count shadowResolutionForIndex(i)
    // returns (pinned by ShadowResolutionUnit.EveryComboLabelMatchesThePixelCount).
    // clang-format off: BuildIntegrity's member parser reads `const char* name`, not `const char *name`.
    const char* available_shadow_map_resolutions[kShadowMapResolutionCount] = { "512", "1024", "2048", "4096" };
    // clang-format on
    static_assert(sizeof(available_shadow_map_resolutions) / sizeof(const char *)
                    == static_cast<std::size_t>(kShadowMapResolutionCount),
      "available_shadow_map_resolutions must have one label per kShadowMapResolutions entry");

    // Clouds
    int cloud_num_march_steps = 8;
    int cloud_num_march_steps_to_light = 3;
    float cloud_density_multiplier = 0.63f;
    float cloud_coverage_threshold = 0.493f;
    float cloud_pillowness = 0.966f;
    float cloud_cirrus_effect = 0.034f;
    bool cloud_powder_effect = true;
    bool clouds_enabled = false;
    float cloud_mesh_scale[3] = { 1000.f, 5.f, 1000.f };
    float cloud_mesh_offset[3] = { -0.364f, 367.f, -18.351f };

    // Shadows
    bool shadows_enabled = true;

    // Skybox
    bool skybox_enabled = true;

    // Camera
    // 45 mirrors Camera's own default (Camera.cpp's NDEBUG and debug
    // initialisers both use 45.F) - the two must not drift apart. Scope:
    // FOV only. near_plane/far_plane stay unexposed here because their
    // Camera defaults are configuration-dependent (150 debug / 4000
    // release), so a single GUI default would contradict one of them;
    // that is a separate decision, not this one.
    float camera_fov = 45.0F;

    // Model selection
    int selected_model_index = -1;
    bool model_reload_requested = false;
    bool model_transform_changed = false;
    float model_position[3] = { 0.f, 0.f, 0.f };
    float model_rotation[3] = { 0.f, 0.f, 0.f };
};
