// applyRendererFeedback is the only writer of the editor's requests besides the editor; the renderer gets them const.

#include <gtest/gtest.h>

#include <type_traits>

import kataglyphis.vulkan.render_settings;

namespace {

using Kataglyphis::applyRendererFeedback;
using Kataglyphis::RendererFeedback;
using Kataglyphis::RenderSettings;

// The renderer keeps a copy of the last frame's settings, so they must stay cheap and safe to copy.
static_assert(std::is_trivially_copyable_v<RenderSettings>, "the renderer copies RenderSettings every frame");

RenderSettings settingsWithEveryRequestRaised()
{
    RenderSettings settings;
    settings.scene.shadow_resolution_changed = true;
    settings.scene.model_transform_changed = true;
    settings.scene.model_reload_requested = true;
    settings.renderer.shader_hot_reload_triggered = true;
    settings.scene.selected_model_index = 3;
    settings.scene.model_position[0] = 1.0F;
    settings.scene.model_position[2] = -2.0F;
    settings.scene.model_rotation[1] = 45.0F;
    settings.scene.shadow_map_res_index = 1;
    settings.scene.num_shadow_cascades = 2;
    return settings;
}

}// namespace

// The renderer acts on a request in the update it sees it, so a request left raised would act again next frame.
TEST(RenderSettingsUnit, ApplyingFeedbackSpendsEveryOneShotRequest)
{
    RenderSettings settings = settingsWithEveryRequestRaised();
    applyRendererFeedback(settings, RendererFeedback{});

    EXPECT_FALSE(settings.scene.shadow_resolution_changed);
    EXPECT_FALSE(settings.scene.model_transform_changed);
    EXPECT_FALSE(settings.scene.model_reload_requested);
    EXPECT_FALSE(settings.renderer.shader_hot_reload_triggered);
}

// Without a load, the editor's selection and transform are the user's and must survive.
TEST(RenderSettingsUnit, FeedbackWithoutALoadKeepsTheSelectionAndTheTransform)
{
    RenderSettings settings = settingsWithEveryRequestRaised();
    applyRendererFeedback(settings, RendererFeedback{});

    EXPECT_EQ(settings.scene.selected_model_index, 3);
    EXPECT_FLOAT_EQ(settings.scene.model_position[0], 1.0F);
    EXPECT_FLOAT_EQ(settings.scene.model_position[2], -2.0F);
    EXPECT_FLOAT_EQ(settings.scene.model_rotation[1], 45.0F);
    EXPECT_EQ(settings.scene.shadow_map_res_index, 1);
    EXPECT_EQ(settings.scene.num_shadow_cascades, 2);
}

// A load places the model at the identity, so the inspector must restart there on the row the load names.
TEST(RenderSettingsUnit, APublishedLoadMovesTheSelectionAndResetsTheTransform)
{
    RenderSettings settings = settingsWithEveryRequestRaised();
    RendererFeedback feedback;
    feedback.publishedModelIndex = 5;
    applyRendererFeedback(settings, feedback);

    EXPECT_EQ(settings.scene.selected_model_index, 5);
    for (const float axis : settings.scene.model_position) { EXPECT_FLOAT_EQ(axis, 0.0F); }
    for (const float axis : settings.scene.model_rotation) { EXPECT_FLOAT_EQ(axis, 0.0F); }
    EXPECT_FALSE(settings.scene.model_transform_changed);
    EXPECT_EQ(settings.scene.shadow_map_res_index, 1) << "a load must not touch unrelated settings";
}

// A model the library does not list (an absolute KATAGLYPHIS_MODEL_OVERRIDE) selects no row.
TEST(RenderSettingsUnit, AnUnlistedLoadSelectsNoRow)
{
    RenderSettings settings = settingsWithEveryRequestRaised();
    RendererFeedback feedback;
    feedback.publishedModelIndex = -1;
    applyRendererFeedback(settings, feedback);

    EXPECT_EQ(settings.scene.selected_model_index, -1);
}

// A glTF's first directional light becomes the editor's Sun: its direction and colour, never its lux.
TEST(RenderSettingsUnit, APublishedSunSetsTheSunsDirectionAndColourButNotItsRadiance)
{
    RenderSettings settings = settingsWithEveryRequestRaised();
    settings.scene.directional_light_radiance = 7.5F;
    RendererFeedback feedback;
    feedback.publishedModelIndex = 2;
    feedback.publishedSun = Kataglyphis::VulkanRendererInternals::FrontendShared::PublishedSun{ { 0.48F, -0.8F, 0.36F },
        { 1.0F, 0.7F, 0.4F } };
    applyRendererFeedback(settings, feedback);

    EXPECT_FLOAT_EQ(settings.scene.directional_light_direction[0], 0.48F);
    EXPECT_FLOAT_EQ(settings.scene.directional_light_direction[1], -0.8F);
    EXPECT_FLOAT_EQ(settings.scene.directional_light_direction[2], 0.36F);
    EXPECT_FLOAT_EQ(settings.scene.directional_light_color[0], 1.0F);
    EXPECT_FLOAT_EQ(settings.scene.directional_light_color[1], 0.7F);
    EXPECT_FLOAT_EQ(settings.scene.directional_light_color[2], 0.4F);
    EXPECT_FLOAT_EQ(settings.scene.directional_light_radiance, 7.5F) << "the radiance is the editor's alone";
}

// A load without a Sun, and every frame without a load, leave the editor's Sun as the user set it.
TEST(RenderSettingsUnit, FeedbackWithoutASunKeepsTheEditorsSun)
{
    RenderSettings settings = settingsWithEveryRequestRaised();
    settings.scene.directional_light_direction[0] = 0.25F;
    settings.scene.directional_light_color[2] = 0.5F;
    RendererFeedback feedback;
    feedback.publishedModelIndex = 4;
    applyRendererFeedback(settings, feedback);
    applyRendererFeedback(settings, RendererFeedback{});

    EXPECT_FLOAT_EQ(settings.scene.directional_light_direction[0], 0.25F);
    EXPECT_FLOAT_EQ(settings.scene.directional_light_color[2], 0.5F);
}

// Owner 2026-11-19: VSync on by default, as the Rust renderer and the browser present; a feedback never turns it off.
TEST(RenderSettingsUnit, VsyncIsOnByDefaultAndSurvivesFeedback)
{
    RenderSettings settings;
    EXPECT_TRUE(settings.renderer.vsync);
    applyRendererFeedback(settings, RendererFeedback{});
    EXPECT_TRUE(settings.renderer.vsync);
}
