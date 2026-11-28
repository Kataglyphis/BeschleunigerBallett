module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "spdlog/spdlog.h"

#include "oxidant_bridge/render_scale.h"

module kataglyphis.vulkan.render_scale;

import kataglyphis.vulkan.gui_renderer_shared_vars;

namespace {

namespace ox = kataglyphis::oxidant::render_scale;
namespace FrontendShared = Kataglyphis::VulkanRendererInternals::FrontendShared;

ox::RenderScale bridged(const FrontendShared::GUIRendererSharedVars &settings)
{ return ox::RenderScale{ settings.render_scale, settings.render_scale_auto, settings.render_scale_target_fps }; }

// `W × H`, the multiplication sign as UTF-8, which ImGui draws.
std::string sizeText(uint32_t width, uint32_t height)
{ return std::to_string(width) + " \xC3\x97 " + std::to_string(height); }

}// namespace

struct Kataglyphis::AutoRenderScale::Scaler
{ rust::Box<ox::AutoScaler> state = ox::new_auto_scaler(); };

auto Kataglyphis::renderScaleExtent(const FrontendShared::GUIRendererSharedVars &settings,
  uint32_t width,
  uint32_t height) -> ScaledExtent
{
    const ox::ScaledExtent scaled = ox::render_scale_extent(bridged(settings), width, height);
    // A panic in the bridge returns zeros; the output's size is the safe answer.
    if (scaled.width == 0U || scaled.height == 0U) { return ScaledExtent{ width, height, 1.0F }; }
    return ScaledExtent{ scaled.width, scaled.height, scaled.scale };
}

auto Kataglyphis::renderScaleRanges() -> RenderScaleRanges
{
    const ox::RenderScaleRanges ranges = ox::render_scale_ranges();
    return RenderScaleRanges{
        ranges.scale.min, ranges.scale.max, ranges.target_fps.min, ranges.target_fps.max, ranges.step
    };
}

auto Kataglyphis::renderScaleChipText(const FrontendShared::GUIRendererSharedVars &settings,
  uint32_t width,
  uint32_t height) -> std::string
{
    const ScaledExtent scaled = renderScaleExtent(settings, width, height);
    if (scaled.scale >= 1.0F) { return sizeText(width, height); }
    return sizeText(scaled.width, scaled.height) + " (" + std::to_string(std::lround(scaled.scale * 100.0F)) + " %)";
}

auto Kataglyphis::renderScaleSummary(const FrontendShared::GUIRendererSharedVars &settings,
  uint32_t width,
  uint32_t height) -> std::string
{
    const ScaledExtent scaled = renderScaleExtent(settings, width, height);
    return "Renders " + sizeText(scaled.width, scaled.height) + " of " + sizeText(width, height) + ".";
}

float Kataglyphis::gpuFrameMs(const FrontendShared::GpuTimings &timings)
{
    if (!timings.supported) { return 0.0F; }
    float total = 0.0F;
    for (std::size_t pass = 0; pass < timings.pass_ms.size(); ++pass) {
        const bool top_level = FrontendShared::GPU_TIMED_PASS_PARENTS[pass] == FrontendShared::GpuTimedPass::Count;
        if (top_level && timings.pass_ms[pass] >= 0.0F) { total += timings.pass_ms[pass]; }
    }
    return total;
}

Kataglyphis::AutoRenderScale::AutoRenderScale() : scaler(std::make_unique<Scaler>()) {}

Kataglyphis::AutoRenderScale::AutoRenderScale(AutoRenderScale &&) noexcept = default;

auto Kataglyphis::AutoRenderScale::operator=(AutoRenderScale &&) noexcept -> AutoRenderScale & = default;

Kataglyphis::AutoRenderScale::~AutoRenderScale() = default;

auto Kataglyphis::AutoRenderScale::medianFrameMs() const -> std::optional<float>
{
    if (frameCount == 0U) { return std::nullopt; }
    std::array<float, kFrameWindow> newest = recentFrameMs;
    const auto end = newest.begin() + static_cast<std::ptrdiff_t>(frameCount);
    const auto middle = newest.begin() + static_cast<std::ptrdiff_t>(frameCount / 2U);
    std::nth_element(newest.begin(), middle, end);
    return *middle;
}

void Kataglyphis::AutoRenderScale::recordFrame(double nowS)
{
    // A non-finite reading would poison every later frame time, so it is dropped.
    if (!std::isfinite(nowS)) { return; }
    // Rust's FrameHistory::tick: raw, not the camera's capped step; the first call and a clock that stood record none.
    const std::optional<double> last = std::exchange(lastNowS, nowS);
    if (!last.has_value() || !(nowS > *last)) { return; }
    recentFrameMs[nextFrame] = static_cast<float>((nowS - *last) * 1000.0);
    nextFrame = (nextFrame + 1U) % kFrameWindow;
    frameCount = std::min(frameCount + 1U, kFrameWindow);
}

auto Kataglyphis::AutoRenderScale::update(const FrontendShared::GUIRendererSharedVars &settings,
  const FrontendShared::RendererFeedback &feedback,
  bool loading,
  double nowS) -> std::optional<float>
{
    recordFrame(nowS);
    const bool raster =
      FrontendShared::drawnRenderMode(settings.raytracing, settings.pathTracing, feedback.tracingReady) == 0;
    if (!settings.render_scale_auto || !raster || loading || !scaler) { return std::nullopt; }

    ox::FrameCost cost{ true, gpuFrameMs(feedback.gpuTimings) };
    if (cost.ms <= 0.0F) {
        const std::optional<float> median = medianFrameMs();
        if (!median.has_value()) { return std::nullopt; }
        cost = ox::FrameCost{ false, *median };
    }
    const ox::ScaleChange change = scaler->state->update(bridged(settings), cost, nowS);
    if (!change.changed) { return std::nullopt; }
    spdlog::info("Auto render scale {:.2f} -> {:.2f} ({} {:.1f} ms)",
      settings.render_scale,
      change.scale,
      cost.gpu_timed ? "GPU" : "frame",
      cost.ms);
    return change.scale;
}
