// OxidANT's render scale through the bridge: the editor's defaults, the scene extent and the app's Auto steering.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>

#include "oxidant_bridge/render_scale.h"

import kataglyphis.vulkan.gui_renderer_shared_vars;
import kataglyphis.vulkan.render_scale;

namespace {

namespace ox = kataglyphis::oxidant::render_scale;
using Kataglyphis::AutoRenderScale;
using Kataglyphis::VulkanRendererInternals::FrontendShared::GpuTimedPass;
using Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars;
using Kataglyphis::VulkanRendererInternals::FrontendShared::RendererFeedback;

GUIRendererSharedVars scaledAt(float scale, bool steered = false)
{
    GUIRendererSharedVars settings{};
    settings.render_scale = scale;
    settings.render_scale_auto = steered;
    return settings;
}

// A frame the GPU timed at `mainMs` in Main, of which `forwardMs` in its nested Forward.
RendererFeedback gpuTimed(float mainMs, float forwardMs = 0.0F)
{
    RendererFeedback feedback{};
    feedback.gpuTimings.supported = true;
    feedback.gpuTimings.pass_ms[static_cast<std::size_t>(GpuTimedPass::Main)] = mainMs;
    feedback.gpuTimings.pass_ms[static_cast<std::size_t>(GpuTimedPass::Forward)] = forwardMs;
    return feedback;
}

}// namespace

// cxx gives the bridged structs no member defaults, so the Rust values come from a call.
TEST(RenderScaleDrift, TheEditorsDefaultsAndRangesAreOxidants)
{
    const GUIRendererSharedVars defaults{};
    const ox::RenderScale rust = ox::default_render_scale();
    EXPECT_EQ(defaults.render_scale, rust.scale);
    EXPECT_EQ(defaults.render_scale_auto, rust.auto_scale);
    EXPECT_EQ(defaults.render_scale_target_fps, rust.target_fps);

    const ox::RenderScaleRanges bridged = ox::render_scale_ranges();
    const Kataglyphis::RenderScaleRanges ranges = Kataglyphis::renderScaleRanges();
    EXPECT_EQ(ranges.minScale, bridged.scale.min);
    EXPECT_EQ(ranges.maxScale, bridged.scale.max);
    EXPECT_EQ(ranges.minTargetFps, bridged.target_fps.min);
    EXPECT_EQ(ranges.maxTargetFps, bridged.target_fps.max);
    EXPECT_EQ(ranges.step, bridged.step);
    // The Rust editor's sliders; a crossed mapping above would still match the bridge field it read.
    EXPECT_LT(ranges.minScale, ranges.maxScale);
    EXPECT_EQ(ranges.maxScale, 1.0F) << "a scene target never outgrows the output";
    EXPECT_LT(ranges.minTargetFps, ranges.maxTargetFps);
}

TEST(RenderScaleUnit, TheSceneExtentScalesEachSideAndNeverReachesZero)
{
    const Kataglyphis::ScaledExtent half = Kataglyphis::renderScaleExtent(scaledAt(0.5F), 1928, 1207);
    EXPECT_EQ(half.width, 964U);
    EXPECT_EQ(half.height, 604U);
    EXPECT_EQ(half.scale, 0.5F);

    const Kataglyphis::ScaledExtent full = Kataglyphis::renderScaleExtent(GUIRendererSharedVars{}, 1928, 1207);
    EXPECT_EQ(full.width, 1928U);
    EXPECT_EQ(full.height, 1207U);
    EXPECT_EQ(full.scale, 1.0F);

    const Kataglyphis::ScaledExtent one = Kataglyphis::renderScaleExtent(scaledAt(0.5F), 1, 1);
    EXPECT_EQ(one.width, 1U);
    EXPECT_EQ(one.height, 1U);

    const Kataglyphis::ScaledExtent tiny = Kataglyphis::renderScaleExtent(scaledAt(0.01F), 100, 100);
    EXPECT_EQ(tiny.width, 25U) << "clamped to a quarter";
    EXPECT_EQ(tiny.scale, 0.25F);

    const Kataglyphis::ScaledExtent above = Kataglyphis::renderScaleExtent(scaledAt(3.0F), 640, 360);
    EXPECT_EQ(above.width, 640U) << "a scene target never outgrows the output";

    const Kataglyphis::ScaledExtent broken = Kataglyphis::renderScaleExtent(scaledAt(std::nanf("")), 640, 360);
    EXPECT_EQ(broken.width, 640U);
    EXPECT_EQ(broken.height, 360U);
    EXPECT_EQ(broken.scale, 1.0F);
}

TEST(AutoRenderScaleUnit, TheGpuFrameTimeIsTheTopLevelPassesAlone)
{
    EXPECT_EQ(Kataglyphis::gpuFrameMs(gpuTimed(10.0F, 9.0F).gpuTimings), 10.0F) << "Forward is inside Main";
    RendererFeedback feedback = gpuTimed(10.0F, 9.0F);
    feedback.gpuTimings.pass_ms[static_cast<std::size_t>(GpuTimedPass::Post)] = 2.0F;
    EXPECT_EQ(Kataglyphis::gpuFrameMs(feedback.gpuTimings), 12.0F);
    // Every other pass keeps its "no sample" -1.
    EXPECT_EQ(Kataglyphis::gpuFrameMs(RendererFeedback{}.gpuTimings), 0.0F);
    feedback.gpuTimings.supported = false;
    EXPECT_EQ(Kataglyphis::gpuFrameMs(feedback.gpuTimings), 0.0F);
}

// The Rust app's rule: Auto steers raster frames only, none while a model loads, and only while it is on.
TEST(AutoRenderScaleUnit, OnlyARasterFrameWithAutoOnAndNoLoadIsSteered)
{
    const RendererFeedback over_budget = gpuTimed(28.0F);
    EXPECT_FALSE(AutoRenderScale{}.update(scaledAt(1.0F), over_budget, false, 0.0).has_value()) << "Auto is off";
    EXPECT_FALSE(AutoRenderScale{}.update(scaledAt(1.0F, true), over_budget, true, 0.0).has_value())
      << "a model is loading";

    GUIRendererSharedVars traced = scaledAt(1.0F, true);
    traced.pathTracing = true;
    RendererFeedback tracing = over_budget;
    tracing.tracingReady = true;
    EXPECT_FALSE(AutoRenderScale{}.update(traced, tracing, false, 0.0).has_value()) << "a path-traced frame";
    tracing.tracingReady = false;
    // Without a TLAS the path-tracing pick draws raster, which Auto steers.
    const std::optional<float> raster = AutoRenderScale{}.update(traced, tracing, false, 0.0);
    ASSERT_TRUE(raster.has_value());
    // 28 ms against 16.7: the side shrinks by sqrt(0.6) = 0.77, down to the step under it.
    EXPECT_FLOAT_EQ(*raster, 0.75F);
}

// Without timestamps the cost is the median of the last frames, which a single spike does not move.
TEST(AutoRenderScaleUnit, WithoutTimestampsTheMedianFrameIsTheCost)
{
    AutoRenderScale scaler;
    const RendererFeedback untimed{};
    const GUIRendererSharedVars off = scaledAt(1.0F);
    double now = 0.0;
    EXPECT_FALSE(scaler.update(off, untimed, false, now).has_value());
    EXPECT_FALSE(scaler.medianFrameMs().has_value()) << "the first frame has no time";
    for (int frame = 0; frame < 14; ++frame) {
        now += 0.016;
        EXPECT_FALSE(scaler.update(off, untimed, false, now).has_value());
    }
    now += 0.5;
    EXPECT_FALSE(scaler.update(off, untimed, false, now).has_value());
    ASSERT_TRUE(scaler.medianFrameMs().has_value());
    EXPECT_FLOAT_EQ(*scaler.medianFrameMs(), 16.0F) << "one spike in fifteen frames";
    now += 0.016;
    EXPECT_FALSE(scaler.update(scaledAt(1.0F, true), untimed, false, now).has_value()) << "16 ms meets a 60 FPS target";

    AutoRenderScale slow;
    now = 0.0;
    for (int frame = 0; frame < 16; ++frame) {
        std::ignore = slow.update(off, untimed, false, now);
        now += 0.028;
    }
    const std::optional<float> lowered = slow.update(scaledAt(1.0F, true), untimed, false, now);
    ASSERT_TRUE(lowered.has_value()) << "fifteen 28 ms frames miss a 60 FPS target";
    EXPECT_FLOAT_EQ(*lowered, 0.75F);
}

// OxidANT's own case, through the bridge: GPU headroom raises the scale a step after two seconds, up to full size.
TEST(AutoRenderScaleUnit, GpuHeadroomRaisesTheScaleAStepAtATimeUpToFullSize)
{
    AutoRenderScale scaler;
    GUIRendererSharedVars settings = scaledAt(0.5F, true);
    const RendererFeedback headroom = gpuTimed(5.0F);
    double now = 0.0;
    const auto run = [&](double seconds) {
        const auto frames = static_cast<int>(seconds * 60.0);
        for (int frame = 0; frame < frames; ++frame) {
            now += 1.0 / 60.0;
            if (const std::optional<float> scale = scaler.update(settings, headroom, false, now)) {
                settings.render_scale = *scale;
            }
        }
    };
    run(2.5);
    EXPECT_NEAR(settings.render_scale, 0.55F, 1e-4F);
    run(60.0);
    EXPECT_EQ(settings.render_scale, 1.0F);
}

// The frame time is the clock's, as Rust's FrameHistory records it: a 300 ms frame is 300, not the camera's 100 ms cap.
TEST(AutoRenderScaleUnit, ALongFrameCountsItsWholeTime)
{
    AutoRenderScale scaler;
    GUIRendererSharedVars settings = scaledAt(0.6F, true);
    settings.render_scale_target_fps = 20.0F;
    const RendererFeedback untimed{};
    EXPECT_FALSE(scaler.update(settings, untimed, false, 3.0).has_value()) << "no frame time before the second frame";
    EXPECT_FALSE(scaler.update(settings, untimed, false, 3.0).has_value()) << "a clock that stood records nothing";
    EXPECT_FALSE(scaler.medianFrameMs().has_value());
    const std::optional<float> lowered = scaler.update(settings, untimed, false, 3.3);
    ASSERT_TRUE(scaler.medianFrameMs().has_value());
    EXPECT_NEAR(*scaler.medianFrameMs(), 300.0F, 1e-3F);
    ASSERT_TRUE(lowered.has_value()) << "300 ms against 50 ms";
    // 0.6 x sqrt(50 / 300) is under the floor of one step down, 0.25; at 100 ms it would be 0.40.
    EXPECT_FLOAT_EQ(*lowered, 0.35F);
}

// Rust's chip and Renderer-tab line, its own case (tests/quality.rs) and extent_text's documented one.
TEST(RenderScaleUnit, TheChipAndTheSummaryReadAsRusts)
{
    EXPECT_EQ(Kataglyphis::renderScaleChipText(GUIRendererSharedVars{}, 1600, 900), "1600 \xC3\x97 900");
    EXPECT_EQ(Kataglyphis::renderScaleChipText(scaledAt(0.75F), 1600, 900), "1200 \xC3\x97 675 (75 %)");
    EXPECT_EQ(Kataglyphis::renderScaleChipText(scaledAt(0.75F), 1928, 1207), "1446 \xC3\x97 905 (75 %)");
    EXPECT_EQ(Kataglyphis::renderScaleChipText(scaledAt(0.55F), 1000, 500), "550 \xC3\x97 275 (55 %)");
    EXPECT_EQ(Kataglyphis::renderScaleSummary(scaledAt(0.75F, true), 1600, 900),
      "Renders 1200 \xC3\x97 675 of 1600 \xC3\x97 900.");
    EXPECT_EQ(Kataglyphis::renderScaleSummary(GUIRendererSharedVars{}, 1600, 900),
      "Renders 1600 \xC3\x97 900 of 1600 \xC3\x97 900.");
}
