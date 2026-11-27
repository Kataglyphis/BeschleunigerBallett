# Post-processing

What the C++ engine does to the HDR target between the scene passes and the swapchain: exposure, auto
exposure and the tonemap, with the Rust WebGPU renderer's shaders, buffers and defaults. Everything here is
**shipped** behaviour; open work lives in `BACKLOG.md`.

## Pipeline shape

Each frame records these stages after `main` (and the clouds), in `RenderStageOrder`:

| Stage | Order | Runs | Pass |
| --- | --- | --- | --- |
| `histogram` | 400 | auto exposure on | two compute dispatches: clear the bins, count the HDR target's luminance |
| `exposure_reduce` | 410 | every frame | one compute invocation: the bins, or the manual EV, into the exposure buffer |
| `post` | 500 | every frame | clouds over the HDR radiance, times `2^EV`, ACES, sRGB |

The shaders are OxidANT's: `histogram/histogram.slang`'s `cs_clear_histogram`, `cs_build_histogram` and
`cs_reduce_exposure` compile to SPIR-V through its manifest rows, and the maths they share with the Rust
renderer's tonemap and bloom is `common/exposure.slang` ([`shader-sharing.md`](shader-sharing.md)). Its vector
file, `tests/vectors/exposure.json`, runs on both targets (`Integration.SharedSlangKernelsMeetTheirVectors`).

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

## Settings

The Renderer tab's POST section, with the Rust renderer's defaults and ranges (`GUIRendererSharedVars`):

| Setting | Default | Range |
| --- | --- | --- |
| `exposure_ev` | 0 EV | -4 to 4 |
| `auto_exposure` | off | |
| `auto_exposure_speed` | 3 per second | 0.1 to 10 |

The renderer clamps each into its range and takes the default for NaN or an infinity (`sanitizePostSetting`).

## The goldens

The harness pins manual exposure at 0 EV (`GoldenRenderHarness.hpp`), which multiplies by exactly 1, so no golden
that predates the post chain changed: post's added `min(..., 1e4)` sits above 7.24, where ACES already shows
white. `goldenRenderPostSuite.cpp` grades each effect against the frame's own HDR capture:

- `ExposureScalesTheRadianceBeforeTheTonemap`: at -1, 0 and +1 EV every sampled pixel is post's encoding of its
  HDR radiance times `2^EV` within 1.5 levels, the buffer holds `[EV, EV]`, and the radiance read back off the
  levels doubles and halves (median ratio 2 and 0.5).
- `AutoExposureConvergesOnTheFramesLogAverageAndHolds`: from a dark and a bright Sun the target lies within a
  quarter EV of the HDR capture's log-average, the first frame moves `1 - e^(-0.3)` of the way, every frame moves
  towards the target and none past it, 40 frames converge, a static scene holds, and post shows the radiance at
  the adapted EV.

`KATAGLYPHIS_GPU_TIMING_JSON` names the passes `Histogram` and `ExposureReduce`, as the Rust renderer's
`TimedPass`.
