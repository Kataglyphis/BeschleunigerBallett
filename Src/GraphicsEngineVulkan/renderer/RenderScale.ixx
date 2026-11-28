module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

export module kataglyphis.vulkan.render_scale;

import kataglyphis.vulkan.gui_renderer_shared_vars;

// OxidANT's render scale through the bridge, whose generated header only this module's .cpp includes.
export namespace Kataglyphis {

// The scene targets' size for one output, and the scale they were taken at after OxidANT's clamp.
struct ScaledExtent
{
    uint32_t width = 0;
    uint32_t height = 0;
    float scale = 1.0F;
};

// Each side of the output scaled and rounded, at least 1; a non-finite scale renders at full size.
[[nodiscard]] ScaledExtent renderScaleExtent(
  const VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &settings,
  uint32_t width,
  uint32_t height);

// The editor's ranges and Auto's step, OxidANT's.
struct RenderScaleRanges
{
    float minScale = 0.0F;
    float maxScale = 0.0F;
    float minTargetFps = 0.0F;
    float maxTargetFps = 0.0F;
    float step = 0.0F;
};

[[nodiscard]] RenderScaleRanges renderScaleRanges();

// The viewport chip's size, Rust's extent_text: `1928 × 1207`, or below full scale the scene's, `1446 × 905 (75 %)`.
[[nodiscard]] std::string renderScaleChipText(
  const VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &settings,
  uint32_t width,
  uint32_t height);

// The Renderer tab's line under Rust's rows: `Renders 1200 × 675 of 1600 × 900.`
[[nodiscard]] std::string renderScaleSummary(
  const VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &settings,
  uint32_t width,
  uint32_t height);

// The frame's GPU time as Rust's frame_ms(): the smoothed top-level passes, a nested one inside its parent's.
[[nodiscard]] float gpuFrameMs(const VulkanRendererInternals::FrontendShared::GpuTimings &timings);

// Auto on the app's side, as the Rust app steers: OxidANT's AutoScaler over the GPU's pass time, else the median frame.
class AutoRenderScale
{
  public:
    // The Rust app's AUTO_SCALE_WINDOW.
    static constexpr std::size_t kFrameWindow = 15;

    AutoRenderScale();
    AutoRenderScale(const AutoRenderScale &) = delete;
    AutoRenderScale &operator=(const AutoRenderScale &) = delete;
    AutoRenderScale(AutoRenderScale &&) noexcept;
    AutoRenderScale &operator=(AutoRenderScale &&) noexcept;
    ~AutoRenderScale();

    // Once a frame at its clock; raster frames only, none while a model loads; the caller writes a new scale back.
    [[nodiscard]] std::optional<float> update(
      const VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &settings,
      const VulkanRendererInternals::FrontendShared::RendererFeedback &feedback,
      bool loading,
      double nowS);

    // The median of the last kFrameWindow frame times; none before the first.
    [[nodiscard]] std::optional<float> medianFrameMs() const;

  private:
    void recordFrame(double nowS);

    struct Scaler;
    std::unique_ptr<Scaler> scaler;
    std::optional<double> lastNowS;
    std::array<float, kFrameWindow> recentFrameMs{};
    std::size_t nextFrame = 0;
    std::size_t frameCount = 0;
};

}// namespace Kataglyphis
