module;

#include <algorithm>
#include <iterator>

export module kataglyphis.vulkan.render_settings;

export import kataglyphis.vulkan.gui_renderer_shared_vars;
export import kataglyphis.vulkan.gui_scene_shared_vars;

export namespace Kataglyphis {

using VulkanRendererInternals::FrontendShared::RendererFeedback;

/// Owned by the app: the editor writes it, the renderer reads it by const reference every frame.
struct RenderSettings
{
    GUISceneSharedVars scene;
    VulkanRendererInternals::FrontendShared::GUIRendererSharedVars renderer;
    /// Seconds the app has run, which moves the clouds' wind; tests leave it at 0 to stay deterministic.
    double time_seconds = 0.0;
};

/// Called after VulkanRenderer::updateStateDueToUserInput: the one-shot requests it acted on are spent, and a model
/// that landed resets the inspector to the identity the load placed it at.
inline void applyRendererFeedback(RenderSettings &settings, const RendererFeedback &feedback)
{
    settings.scene.shadow_resolution_changed = false;
    settings.scene.model_transform_changed = false;
    settings.scene.model_reload_requested = false;
    settings.renderer.shader_hot_reload_triggered = false;
    settings.renderer.pathTracingRestartRequested = false;
    if (feedback.publishedModelIndex.has_value()) {
        settings.scene.selected_model_index = *feedback.publishedModelIndex;
        std::fill(std::begin(settings.scene.model_position), std::end(settings.scene.model_position), 0.0F);
        std::fill(std::begin(settings.scene.model_rotation), std::end(settings.scene.model_rotation), 0.0F);
    }
    if (feedback.publishedShadowDistance.has_value()) {
        settings.scene.shadow_distance =
          std::clamp(*feedback.publishedShadowDistance, kMinShadowDistance, kMaxShadowDistance);
    }
    if (feedback.publishedFov.has_value()) {
        settings.scene.camera_fov = std::clamp(*feedback.publishedFov, kMinCameraFov, kMaxCameraFov);
    }
    // The radiance stays the editor's: glTF gives lux, and the renderer has no exposure to turn them into radiance.
    if (feedback.publishedSun.has_value()) {
        std::copy(feedback.publishedSun->direction.begin(),
          feedback.publishedSun->direction.end(),
          std::begin(settings.scene.directional_light_direction));
        std::copy(feedback.publishedSun->color.begin(),
          feedback.publishedSun->color.end(),
          std::begin(settings.scene.directional_light_color));
    }
}

}// namespace Kataglyphis
