module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

export module kataglyphis.vulkan.gui_renderer_shared_vars;

export namespace Kataglyphis::VulkanRendererInternals::FrontendShared {
enum class RasterizationMode { Forward, Deferred };

// Which frames FXAA smooths after the tonemap: raster frames have no other anti-aliasing, traced ones may accumulate.
enum class FxaaMode : std::uint8_t { Off, Raster, Always };

// The editor's labels, in FxaaMode's order.
inline constexpr std::array FXAA_MODE_LABELS = std::to_array<const char *>({ "Off", "Raster", "Always" });

// A stage that cannot run (no set or pipeline) leaves the swapchain image to post, or nothing would write it.
constexpr bool fxaaFilters(FxaaMode mode, bool tracedFrame, bool stageReady)
{
    if (mode == FxaaMode::Off || !stageReady) { return false; }
    return mode == FxaaMode::Always || !tracedFrame;
}

// The post chain's defaults and editor ranges, the Rust renderer's (its RendererSettings and the Renderer tab's POST).
inline constexpr float kDefaultExposureEv = 0.0F;
inline constexpr float kMinExposureEv = -4.0F;
inline constexpr float kMaxExposureEv = 4.0F;
inline constexpr float kDefaultAutoExposureSpeed = 3.0F;
inline constexpr float kMinAutoExposureSpeed = 0.1F;
inline constexpr float kMaxAutoExposureSpeed = 10.0F;
inline constexpr float kDefaultBloomStrength = 0.6F;
inline constexpr float kMaxBloomStrength = 2.0F;
inline constexpr float kDefaultSsaoStrength = 0.7F;
inline constexpr float kMaxSsaoStrength = 1.0F;

// The editor's range; NaN and the infinities take the default, as every other setting the renderer reads.
inline float sanitizePostSetting(float value, float lowest, float highest, float fallback)
{ return std::isfinite(value) ? std::clamp(value, lowest, highest) : fallback; }

// Passes bracketed by GPU timestamp pairs in VulkanRenderer::record_commands.
// "Main" is forward/deferred rasterization or, in its place, the ray-/path-
// tracing stage, and times the one of Forward, GBuffer + DeferredLighting,
// RayTrace or PathTrace that drew inside it, the sky included, as Rust's
// scopes do. The GUI draws in the last pass that writes the swapchain image:
// "Fxaa" while FXAA filters the frame, else "Post".
enum class GpuTimedPass : int {
    Clouds = 0,
    ShadowCascades,
    Main,
    Forward,
    GBuffer,
    DeferredLighting,
    RayTrace,
    PathTrace,
    Post,
    CloudUpsample,
    PunctualShadows,
    Fxaa,
    Histogram,
    ExposureReduce,
    Bloom,
    Ssao,
    Count
};

// Single source of truth for the pass count: bumping this table is the only
// edit needed to add a pass, and the static_asserts below catch every other
// table that must stay in step with it.
inline constexpr std::array GPU_TIMED_PASS_NAMES = std::to_array<const char *>({
  "Clouds (compute)",
  "Shadow cascades",
  "Main (raster/RT)",
  "  Forward",
  "  G-buffer",
  "  Deferred lighting",
  "  Ray trace",
  "  Path trace",
  "Post (+ GUI)",
  "Clouds upsample",
  "Point/spot shadows",
  "FXAA + GUI",
  "Histogram",
  "Exposure",
  "Bloom",
  "  SSAO",
});
inline constexpr int GPU_TIMED_PASS_COUNT = static_cast<int>(GPU_TIMED_PASS_NAMES.size());

// Machine-readable identifiers for the same passes, used as JSON keys by the
// KATAGLYPHIS_GPU_TIMING_JSON export. Separate from the display names above:
// those may be reworded for the GUI at will, while these are diffed between
// runs by tooling and must stay stable. A pass the Rust renderer times too
// takes its TimedPass::name() (BuildIntegrity.GpuTimingMatchesTheRustRenderersTaxonomy).
inline constexpr std::array GPU_TIMED_PASS_EXPORT_NAMES = std::to_array<const char *>({
  "Clouds",
  "ShadowCascades",
  "Main",
  "Forward",
  "GBuffer",
  "DeferredLighting",
  "RayTrace",
  "PathTrace",
  "Post",
  "CloudUpsample",
  "PunctualShadows",
  "Fxaa",
  "Histogram",
  "ExposureReduce",
  "Bloom",
  "Ssao",
});

// The pass whose scope encloses each one, Count for a top-level pass: a frame sum adds only the top level.
inline constexpr std::array GPU_TIMED_PASS_PARENTS = std::to_array<GpuTimedPass>({
  GpuTimedPass::Count,
  GpuTimedPass::Count,
  GpuTimedPass::Count,
  GpuTimedPass::Main,
  GpuTimedPass::Main,
  GpuTimedPass::Main,
  GpuTimedPass::Main,
  GpuTimedPass::Main,
  GpuTimedPass::Count,
  GpuTimedPass::Count,
  GpuTimedPass::Count,
  GpuTimedPass::Count,
  GpuTimedPass::Count,
  GpuTimedPass::Count,
  GpuTimedPass::Count,
  GpuTimedPass::Main,
});

static_assert(GPU_TIMED_PASS_EXPORT_NAMES.size() == GPU_TIMED_PASS_NAMES.size(),
  "display and export name tables must stay parallel");
static_assert(GPU_TIMED_PASS_PARENTS.size() == GPU_TIMED_PASS_NAMES.size(),
  "the parent table must stay parallel to the name tables");
static_assert(static_cast<int>(GpuTimedPass::Count) == GPU_TIMED_PASS_COUNT,
  "GpuTimedPass::Count must track the pass name table");

struct GpuTimings
{
    // false when the graphics queue family reports timestampValidBits == 0.
    bool supported = false;
    /// Smoothed milliseconds per pass; < 0 means no sample yet or the pass is
    /// currently disabled/not recorded. Every element starts at the -1.0F
    /// "no sample" sentinel, for any GPU_TIMED_PASS_COUNT.
    ///
    /// \hideinitializer
    /// The initializer is an immediately-invoked lambda - valid C++ that
    /// Sphinx's C++ expression parser cannot read ("Error in postfix
    /// expression, expected primary expression or type"), which under -W
    /// failed the whole docs build. Hiding the initializer from Doxygen keeps
    /// the member documented and its meaning stated above in prose, without
    /// handing the doc toolchain an expression it has no grammar for.
    std::array<float, GPU_TIMED_PASS_COUNT> pass_ms = [] {
        std::array<float, GPU_TIMED_PASS_COUNT> a{};
        a.fill(-1.0F);
        return a;
    }();
};
static_assert(std::tuple_size_v<decltype(GpuTimings::pass_ms)> == GPU_TIMED_PASS_NAMES.size(),
  "GpuTimings::pass_ms must have one slot per timed pass");

struct VisibilityStats
{
    /// Meshes actually drawn by the raster path last frame.
    unsigned int meshes_drawn = 0;
    /// Meshes the raster path considered, culled or not.
    unsigned int meshes_total = 0;
    /// Shadow casters actually drawn by the cascade pass last frame. Culled
    /// against the UNION of the cascade frusta, not the camera frustum - a
    /// caster visible to any cascade counts as drawn.
    unsigned int shadow_casters_drawn = 0;
    /// Shadow casters the cascade pass considered, culled or not.
    unsigned int shadow_casters_total = 0;
};

/// The part of the swapchain image the docked GUI leaves to the scene, in fractions (0..1) of its width and
/// height, written by the GUI every frame. The renderer centres the projection on it; the default is the whole
/// image, which is also what a hidden GUI reports.
struct SceneViewport
{
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 1.0F;
    float y1 = 1.0F;
};

/// What the editor sets for the renderer; part of the app-owned RenderSettings, read by the renderer each frame.
struct GUIRendererSharedVars
{
    RasterizationMode rasterizationMode = RasterizationMode::Forward;

    SceneViewport scene_viewport;

    // The selection; what a frame draws is drawnRenderMode() below.
    bool raytracing = false;
    bool pathTracing = false;

    bool shader_hot_reload_triggered = false;
    /// CPU frustum culling. A switch rather than a constant because culling
    /// is the one optimisation that can silently delete visible geometry -
    /// when something goes missing, being able to turn it off tells you in
    /// one click whether culling is the cause. Gates the camera-frustum cull
    /// used by the two raster paths (VulkanRenderer::record_commands) AND
    /// the cascade-frustum cull in the shadow pass
    /// (CascadedShadowMap::recordCommands), so the checkbox answers "is
    /// culling why this is missing" for shadows too.
    bool frustum_culling_enabled = true;

    // Always by default: forward has no MSAA yet, so FXAA is the only anti-aliasing a raster frame gets.
    FxaaMode fxaaMode = FxaaMode::Always;

    // On, as the Rust renderer and the browser present; a toggle rebuilds the swapchain, whose present mode is fixed.
    bool vsync = true;

    // Path-tracing quality controls. Both were hardcoded in the kernel (8/8,
    // with stale comments claiming 64/32); changing either mid-run resets the
    // accumulation history - a mean over two different estimators is biased.
    int pathTracingSamplesPerPixel = 8;
    int pathTracingMaxBounces = 8;
    // The Rust editor's White furnace: one white surface under a uniform sky of radiance 1, without the sun.
    bool pathTracingFurnace = false;
    // One-shot, spent by applyRendererFeedback: the next path-traced frame starts the running mean over.
    bool pathTracingRestartRequested = false;

    // In stops before the tonemap; auto exposure overrides it.
    float exposure_ev = kDefaultExposureEv;
    // Meters the frame's luminance histogram on the GPU and adapts towards middle grey.
    bool auto_exposure = false;
    // The adaptation's rate per second; higher settles faster.
    float auto_exposure_speed = kDefaultAutoExposureSpeed;
    // How much of the half-resolution glow around radiance past white post adds; 0 turns the pass off.
    float bloom_strength = kDefaultBloomStrength;
    // How far SSAO darkens the ambient and IBL light, never the direct light; 0 turns it and its prepass off.
    float ssao_strength = kDefaultSsaoStrength;
};

// The manual EV a frame applies: the editor's range, the default for NaN.
inline float manualExposureEv(const GUIRendererSharedVars &vars)
{ return sanitizePostSetting(vars.exposure_ev, kMinExposureEv, kMaxExposureEv, kDefaultExposureEv); }

// The bloom strength a frame applies: the editor's range, the default for NaN; 0 records no bloom pass.
inline float frameBloomStrength(const GUIRendererSharedVars &vars)
{ return sanitizePostSetting(vars.bloom_strength, 0.0F, kMaxBloomStrength, kDefaultBloomStrength); }

// The SSAO strength a frame applies: the editor's range, the default for NaN; 0 records no SSAO pass.
inline float frameSsaoStrength(const GUIRendererSharedVars &vars)
{ return sanitizePostSetting(vars.ssao_strength, 0.0F, kMaxSsaoStrength, kDefaultSsaoStrength); }

/// A loaded glTF's first directional light: the unit direction towards it, as the editor stores it, and its colour.
struct PublishedSun
{
    std::array<float, 3> direction{ 0.0F, 1.0F, 0.0F };
    std::array<float, 3> color{ 1.0F, 1.0F, 1.0F };
};

/// What the renderer reports back for the editor to show, copied out of each per-frame call.
struct RendererFeedback
{
    /// A TLAS exists, so an RT/PT selection owns the frame; until then raster draws.
    bool tracingReady = false;
    /// The scene's model as the load named it, empty while it holds none.
    std::string loadedModelPath;

    // Per-pass GPU timings.
    GpuTimings gpuTimings;

    /// Meshes submitted vs considered by the last recorded frame.
    ///
    /// Culling is otherwise invisible: it has no visual signature when it is
    /// working and none when it is wrong either - geometry simply is not
    /// there. These two numbers are what turn "something is missing" into
    /// "culling dropped it", and they are also the only way to see whether
    /// culling is doing anything at all in a given scene.
    VisibilityStats visibility;

    /// Set by the update a load landed in: the library row now showing, -1 when the library does not list it.
    std::optional<int> publishedModelIndex;

    /// Set with publishedModelIndex when the loaded model has a Sun, which becomes the editor's: OxidANT's rule.
    std::optional<PublishedSun> publishedSun;

    /// The loaded model's credit, which its licence asks the About window to show; empty for none.
    std::string loadedModelCredit;
    /// Set by the update a load moved the camera in: the framing's shadow distance becomes the editor's.
    std::optional<float> publishedShadowDistance;
    /// Set with publishedShadowDistance: the framing's field of view becomes the Camera tab's, which the camera reads.
    std::optional<float> publishedFov;

    /// Samples per pixel in the path tracer's running mean after the last frame it traced.
    uint64_t pathTracingAccumulatedSamples = 0;
};

// 0 raster, 1 ray traced, 2 path traced: the one rule the renderer draws by and the editor shows.
constexpr int drawnRenderMode(bool raytracing, bool pathTracing, bool tracingAvailable)
{
    if (!tracingAvailable) { return 0; }
    if (pathTracing) { return 2; }
    return raytracing ? 1 : 0;
}

// A drawnRenderMode() value as the renderer's logs name it.
constexpr const char *renderModeLogName(int drawnMode)
{
    if (drawnMode == 2) { return "path_tracing"; }
    return drawnMode == 1 ? "raytracing" : "rasterizer";
}
}// namespace Kataglyphis::VulkanRendererInternals::FrontendShared
