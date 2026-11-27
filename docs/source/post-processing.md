# Post-processing

What the C++ engine does around the HDR target: SSAO before the scene is lit, then exposure, auto exposure,
bloom and the tonemap between the scene passes and the swapchain, with the Rust WebGPU renderer's shaders, buffers
and defaults. Everything here is **shipped** behaviour; open work lives in `BACKLOG.md`.

## Pipeline shape

SSAO records inside `main`, before the pass that lights the scene (below). Each frame records these stages after
`main` (and the clouds), in `RenderStageOrder`:

| Stage | Order | Runs | Pass |
| --- | --- | --- | --- |
| `histogram` | 400 | auto exposure on | two compute dispatches: clear the bins, count the HDR target's luminance |
| `exposure_reduce` | 410 | every frame | one compute invocation: the bins, or the manual EV, into the exposure buffer |
| `bloom` | 450 | bloom strength above 0 | three half-resolution fullscreen passes: bright pass, horizontal and vertical blur |
| `post` | 500 | every frame | clouds over the HDR radiance, times `2^EV`, plus the bloom, ACES, sRGB |

The shaders are OxidANT's: `histogram/histogram.slang`'s `cs_clear_histogram`, `cs_build_histogram` and
`cs_reduce_exposure` compile to SPIR-V through its manifest rows, and the maths they share with the Rust
renderer's tonemap and bloom is `common/exposure.slang` ([`shader-sharing.md`](shader-sharing.md)). Bloom's three
passes are `bloom/bloom.slang`'s fragment entries behind `post.slang`'s `SV_VulkanVertexID` vertex, their threshold
and taps `common/bloom_filter.slang`. Both modules' vector files, `tests/vectors/exposure.json` and
`bloom_filter.json`, run on both targets (`Integration.SharedSlangKernelsMeetTheirVectors`).

## The scene targets

One set serves every frame in flight, as the Rust renderer's `RenderTargets` does, never one per swapchain image,
all at `VulkanRenderer::sceneExtent()`, the one scene extent (the swapchain's, until a render scale):

| Target | Format | B/px | Exists |
| --- | --- | --- | --- |
| scene HDR (`sceneColor`) | RGBA16F | 8 | always: every mode draws into it, post, bloom and the histogram read it |
| forward depth, SSAO prepass depth | D32 | 8 | while the forward path is picked |
| G-buffer normal, albedo, material, its depth | RGBA16F, RGBA8 sRGB, RGBA16F, D32 | 24 | while the deferred path is picked |
| FXAA intermediate | the swapchain's | 4 | always |
| path-traced history, traced depth | RGBA32F, R32F | 20 | on a device with ray tracing |
| bloom's two, SSAO's two | RGBA16F, R8, at half the extent | 4.5 | always |
| clouds, full and half resolution | RGBA16F | 10 | always |

- **A raster path's targets exist from its first frame until the switch away**, as Rust's transient pool drops
  what a frame's graph does not declare. `ensureRasterPathTargets` drains the GPU, frees the other path's targets,
  makes the picked path's and rebinds SSAO's depth, the G-buffer sets and the clouds' depth. It runs in
  `updateStateDueToUserInput` and again before `drawFrame` records, so no frame records against the other path's
  targets. A traced frame keeps the picked path's: without a TLAS the same pick draws raster.
- **Frames overlap on the queue, so each target's first barrier in a frame waits out the stages the last frame used
  it in**: a pipeline barrier orders against everything submitted before it, where the per-image fence the targets
  relied on before only ordered against the frame that used the same swapchain image.
- **What it saves**, measured on llvmpipe (4 swapchain images) as the allocator's growth from 1200x768 to 1920x1080,
  which `sceneTargetFootprint()` matches to the byte: every extent-sized image together held 206.5 B/px in both
  modes before, 54.5 forward and 70.5 deferred after, so at 1080p 108 MiB forward and 139 MiB deferred instead of
  408 MiB. The raster targets and post's alone are 20.5 and 36.5 B/px: plan C6's 12 and 32 plus the SSAO prepass on
  the forward path and bloom's and SSAO's 4.5.
- `VulkanRenderer::sceneTargetFootprint()` sums `vkGetImageMemoryRequirements` by row of this table;
  `Integration.SceneTargetsAreOneSetOfTheDrawnRasterPath` pins one HDR target and one FXAA intermediate, and the
  absent path, across switches, a swapchain recreation and the traced modes.

## The exposure buffer

One device-local storage buffer of two floats, `[adapted EV, target EV]`, as the Rust renderer's
`HistogramPass` keeps it (`renderer/ExposureStage.cpp`). `post.slang` reads `exposureState[0]` and scales the
composited radiance by `exposure_scale`, `exp2`, before ACES; nothing reads it back on the frame path.

- **Manual exposure** (the default): the reduce writes `[EV, EV]` every frame, so switching modes leaves no stale
  value and post always reads one source.
- **Auto exposure**: the histogram counts each HDR pixel's Rec. 709 luminance into 64 bins over log2 -10 to 4
  (bin 0 is black and never counts), and the reduce takes the bins' geometric mean to the EV that puts it on
  middle grey 0.18, then moves the adapted EV towards it by `1 - e^(-dt speed)`, the same time at any frame rate.
  An all-black frame holds; a frame of no time holds; a NaN or infinite state takes the target.
- `dt` is the difference of `RenderSettings::time_seconds` between two reduces (the app's GLFW clock); the tests
  advance it by hand, and a clock that stands still holds the exposure.
- A new scene (`refreshAfterSceneChange` with new geometry) restarts the adaptation from the manual EV, as
  OxidANT's `upload_scene` does. The first frame writes the starting value, since a new buffer holds garbage.
- One buffer serves every frame in flight, so each reduce waits for the previous frames' reads in post and their
  writes (`buildBufferMemoryBarrier`, `common/ImageBarrierHelper.hpp`); the HDR target's barriers name the
  histogram's compute stage beside post's fragment stage in every mode.

## Bloom

`renderer/BloomStage.cpp`, at half the swapchain's extent rounded down (`postHalfExtent`, OxidANT's `post_half`), into
two RGBA16F targets with one linear clamp-to-edge sampler:

- **Bright pass**: the HDR target sampled at the half-resolution texel's centre, the 2x2 average, with the clouds
  composited first when they draw (a sun they hide must not glow), times the exposure buffer's `2^EV`, less 1 per
  channel (`bloom_bright`): only what the tonemap would show past white glows. One set per swapchain image, for its
  uniform buffer; every set binds the one HDR target.
- **Blur**: the separable 9-tap Gaussian (`bloom_tap_weight`, weights 0.227 to 0.016, summing to one within 6e-7),
  horizontal into the scratch target, vertical back into the output. It moves energy four half-resolution texels
  and no further, and keeps it.
- **Composite**: post samples the output bilinearly at the pixel's uv and adds it times the strength to the exposed
  radiance before ACES, as `tonemap.slang` does; at strength 0 the stage does not record and post skips the read.
- Both targets start black and sampleable, cleared once at creation and after every swapchain recreation, since
  post binds the output on every frame. Each pass's barriers discard the target it overwrites after the previous
  frame's reads.

## SSAO

`renderer/SsaoStage.cpp` runs OxidANT's `ssao/ssao.slang` at half resolution into two R8 targets: 12 hemisphere taps
around the normal rebuilt from the depth's neighbours (radius 0.6, bias 0.02, intensity 1, the Rust renderer's), then a
3x3 box blur. It darkens the **ambient and IBL light only**, never the direct light, in both renderers, so it must read
a depth before the scene is lit:

- **Deferred**: the G-buffer's depth, in `DeferredRasterizer`'s `beforeLighting` hook, after the G-buffer turned
  sampleable.
- **Forward**: a single-sample depth prepass of the opaque and MASK meshes (`Rasterizer::recordDepthPrepass`,
  `rasterizer.slang`'s `fs_depth_prepass`, the forward pass's culling and draw walk) into its own depth image, which
  the forward pass does not share, so its depth test is unchanged.
- The forward and deferred lighting read the blurred occlusion at their pixel through `SSAO_BINDING` and
  `SceneUBO::screenOcclusion` (strength, 1 / extent) and multiply OxidANT's `screen_space_occlusion` into the ambient
  term (`common/screen_occlusion.slang`); a BLEND surface takes none, as its texel belongs to the surface behind it.
  The traced modes run no SSAO.
- The occlusion starts white and sampleable; at strength 0 neither the prepass nor SSAO records and the factor is 1.
- `KATAGLYPHIS_GPU_TIMING_JSON` names the scope `Ssao`, nested in `Main` as in the Rust renderer, prepass included.

## Settings

The Renderer tab's POST section, with the Rust renderer's defaults and ranges (`GUIRendererSharedVars`):

| Setting | Default | Range |
| --- | --- | --- |
| `exposure_ev` | 0 EV | -4 to 4 |
| `auto_exposure` | off | |
| `auto_exposure_speed` | 3 per second | 0.1 to 10 |
| `bloom_strength` | 0.6 | 0 to 2 |
| `ssao_strength` | 0.7 | 0 to 1 |

The renderer clamps each into its range and takes the default for NaN or an infinity (`sanitizePostSetting`).

## The goldens

The harness pins manual exposure at 0 EV, bloom at 0 and SSAO at 0 (`GoldenRenderHarness.hpp`): 0 EV multiplies by
exactly 1 and a strength of 0 records neither pass, so no golden that predates the post chain changed: post's added
`min(..., 1e4)` sits above 7.24, where ACES already shows white. `goldenRenderPostSuite.cpp` grades each effect
against the frame's own HDR capture:

- `ExposureScalesTheRadianceBeforeTheTonemap`: at -1, 0 and +1 EV every sampled pixel is post's encoding of its
  HDR radiance times `2^EV` within 1.5 levels, the buffer holds `[EV, EV]`, and the radiance read back off the
  levels doubles and halves (median ratio 2 and 0.5).
- `AutoExposureConvergesOnTheFramesLogAverageAndHolds`: from a dark and a bright Sun the target lies within a
  quarter EV of the HDR capture's log-average, the first frame moves `1 - e^(-0.5)` of the way, every frame moves
  towards the target and none past it, 25 frames converge, a static scene holds, and post shows the radiance at
  the adapted EV.
- `BloomSpreadsABrightCardsEnergyAndConservesIt`: an emissive card at radiance 6 on the navy: the blurred target
  is the CPU bright pass's 9-tap Gaussian texel for texel within the half floats' rounding, holds its energy within
  2 %, spills some of it outside the card's texels and none further than four texels, every pixel is post's
  encoding of the radiance plus 0.6 times the bilinear bloom within 1.5 levels, and a pixel the bloom does not
  reach is unchanged. It then blooms with the clouds and on a path-traced
  frame, which the sync validation of the GPU run watches.
- `SsaoDarkensTheAmbientLightAndNeverTheDirectLight`: on both raster paths, the Sun alone renders byte-identical with
  SSAO at 0 and 1, and the sky alone darkens over 0.5 % of the frame by 3 levels or more (1.3 % on llvmpipe) and
  brightens no pixel.

`KATAGLYPHIS_GPU_TIMING_JSON` names the passes `Histogram`, `ExposureReduce`, `Bloom` and `Ssao`, as the Rust
renderer's `TimedPass`.
