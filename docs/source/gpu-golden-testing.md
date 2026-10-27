# GPU golden testing

The `GoldenRender.*` and `Integration.*` suites in `Test/commit/VulkanEngine/` render real frames
on a real GPU and assert on the captured pixels: the end-to-end net for render- and device-path
changes that the CPU unit suites cannot cover.

## They skip without a GPU

Each test starts with `SKIP_WITHOUT_GPU()` (or an equivalent GLFW/Vulkan-runtime guard). The
headless container build (`scripts/windows/Build-Windows-Container.ps1 -RunTests`) has no GPU, so
they report **skipped**, not passed. A green container build proves the code *compiles and links*
and the CPU suites pass; it does **not** prove a render/device refactor preserves behaviour.

## Linux CI runs them on llvmpipe

Since 2026-09-29 the family Linux image ships lavapipe/llvmpipe, a CPU Vulkan device with ray
tracing, and `Xvfb`. The Linux `clang-tests` job (`reusable-linux.yml`, x64 and arm64) runs the
GPU suites for real: `run-ctest.sh --virtual-display` starts the run under `xvfb-run`, so
`glfwInit()` finds an X server and nothing skips. Locally:

```bash
bash ./scripts/linux/run-ctest.sh --virtual-display --build-dir build --build-type Debug
```

- **Only that one Linux job runs them.** They cost ~20 minutes of llvmpipe on a 32-core host (~55
  on a 4-vCPU runner); the sanitizer and gcc jobs keep excluding them.
- **`GuiInputSweepNeverCrashesOrLosesTheDevice` is too slow for a runner.** It passes on llvmpipe
  (266 s on 32 cores; 12 s on an RTX 2080) but hit ctest's 1500 s timeout on a 4-vCPU GitHub runner
  (run 36627716804), so CI excludes it. It is the only exclusion: the five goldens once excluded
  here failed on an RTX 2080 too, because their oracles were wrong, and were fixed (`BACKLOG.md`,
  "GPU suites on llvmpipe", pruned in `162e41d3`).
- **The image sets `LP_NATIVE_VECTOR_WIDTH=256`** (hub CON44, `:latest` of 2026-10-01;
  `--virtual-display` pinned it here before that). Mesa 26.0's lavapipe builds acceleration
  structures with a radix sort whose shaders are compiled for 8-lane subgroups
  (`lvp_acceleration_structure.c`, `subgroup_size_log2 = 3`), but llvmpipe's subgroup is
  `native vector width / 32` lanes: 8 on an AVX2 host, 4 on arm64's 128-bit NEON. At 4 lanes the
  scatter pass (`rs_scatter_smem`) writes through wild per-lane addresses and every test that
  builds a BVH SEGVs inside llvmpipe's JIT code (arm64 run 36746313937; on x64,
  `LP_NATIVE_VECTOR_WIDTH=128` reproduces it). 256 is x64's default already; on arm64 LLVM splits
  each vector into two NEON registers. Mesa main replaced the radix sort with a merge sort
  (`ebcfbe60`, 2026-08-22), so the hub drops its `ENV` once the image's Mesa carries that. The
  Windows lavapipe run (`Invoke-LavapipeTests.ps1`) uses its own Mesa and keeps the pin.
- **ASan builds carry a libX11 leak suppression.** `glfwInit()` on X11 leaks libX11's locale
  state, which failed every GPU test in an ASan build, so `commitTestSuite` carries its own
  LeakSanitizer suppression (`Test/commit/VulkanEngine/lsanSuppressions.cpp`).
- **A software rasterizer is not the RX 9070 XT.** A pass on llvmpipe is a strong behavioural
  signal, not a substitute for the host loop below after a render/device change.

## Windows CI runs them on lavapipe

The Windows container run excludes them (`$gpuOnlySuites`). Both Windows lanes run them on the
runner instead, on Mesa's lavapipe outside the image: the hub image carries its own device since
2026-10-04 (CON50), and its own smoke proves it, but a host-run suite needs the host's ICD, and a
hosted runner takes one in a minute. `scripts/windows/Invoke-LavapipeTests.ps1` does it for either
arch:

- **What it installs.** lavapipe from [mmozeiko/build-mesa](https://github.com/mmozeiko/build-mesa)
  26.2.3 (the one build that ships an arm64 lavapipe; mesa-dist-win has x86 and x64 only) and the
  Khronos loader from LunarG's 1.4.357.0 runtime components, both pinned by SHA256.
  `vulkan-1.dll` goes beside the suite, so no System32 copy wins.
- **How the loader finds it.** `VK_DRIVER_FILES` names the ICD, but the loader ignores it in an
  elevated process, which a hosted runner's is, so the ICD is also registered under
  `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`. `VK_LOADER_DRIVERS_SELECT` keeps any other driver out,
  and `vulkaninfo --summary` must list llvmpipe before a test runs.
- **How it runs them.** Each test in its own process, from the repo root, with ctest's 1500 s
  limit, as ctest does on Linux; the GUI input sweep stays out for time. Any failure, any skip or
  no pass fails the step: a skip here is a test that did not run. Every invocation keeps fresh
  gtest JSON and separate stdout/stderr files under ignored `logs/lavapipe/<run>/`. A failed case
  reports its raw process exit code, whether JSON exists, and the file paths. The ARM64 workflow
  uploads those files on failure as `arm64-lavapipe-test-logs-<attempt>`, even when Windows produced
  no crash dump. The upload fails if no evidence files exist, so a missing capture stays visible.
  A nonzero exit without a gtest assertion still needs those logs or a dump to
  locate the fault; the last printed renderer phase alone does not identify it.
- **Which build.** The Release commit suite that `Build-Windows.ps1 -StageTests` stages, on both
  arches: a clean host has no validation layers for the Debug one, so the arm64 lane's Debug
  (ASan) suite, run beside it since 2026-10-03, skips the same GPU suites. x64 runs it on the
  runner host after the container build (`windows-x64.yml`'s `host-command`); arm64 in the
  `gpu-suites` job on `windows-11-arm`, from a checkout, which also runs the source-reading
  `BuildIntegrity.*` checks the staged tree lacks. The script sets `LP_NATIVE_VECTOR_WIDTH=256`
  itself; on Linux the image does.
  The ARM64 test artifact includes the local `Resources/ShadersSlang/` source tree and OxidANT's
  pinned tree under `third_party/OxidANT/shaders/`: `RepoFilesUnit` runs from that artifact and
  resolves both source roots relative to it, not the separate checkout.
  The same artifact carries freshly generated WGSL. Artifact download rewrites mtimes,
  so `BuildIntegrity` compares its normalized WGSL and source-tree inputs with the host
  checkout; the local single-tree test still checks timestamps. An assertion failure has no crash
  dump; the workflow uploads a dump only when Windows created one.
- **Release frames the same view.** A Release app opens on crytek-sponza from inside, a Debug one
  on Dinosaurs from (0, 6, 26) (`Camera.cpp`, `SceneConfig.cpp`), and the goldens were calibrated
  on the latter: the first Release run failed 14 of 40 on framing alone (cards out of view, 13 of
  373 meshes "visible"). So `EngineHarness` pins that scene, unless the test picked its own, and
  that camera, whatever the build type.
- **The runner's desktop must hold the window.** Windows clamps a new window to the desktop, and
  `windows-11-arm`'s is 1024x768, which shrank the 1200x768 frame to 1004x749; the script switches
  a CI runner's display mode to fit (`ChangeDisplaySettings`), never a workstation's.

## The riscv64 light variant

Under QEMU one path-traced frame costs up to about 190 s (AGENTS.md § The riscv64 lane), so
four path-traced goldens accumulate a shorter history on riscv64.
`KATAGLYPHIS_PT_GOLDEN_VARIANT=light` selects it, and only `scripts/linux/run-riscv64-tests.sh`
sets it: every other lane runs the full variant. Any value but `full` or `light` fails the test.
Each of the four logs the variant it ran, for example
`path-traced variant: light, 24-frame history (full 60, light 24)`, and the riscv64 lane runs
ctest with `-V`, so a passing shard shows the line too.

The thresholds are the full variant's. Measured on x64 lavapipe in `:latest`, Debug clang build,
2026-10-12:

| Test | History, full → light | Full | Light, five RNG streams | Threshold | Light, property broken |
| --- | --- | --- | --- | --- | --- |
| `PathTracedMaskCardShowsItsCutout` | 60 → 24 | 0.355 | 0.354-0.356 | 0.15-0.49 | alpha test ignored: 0.631, fails |
| `AddedModelAppearsInPathTracing` | 60 → 24 | 0.016 before, 0.403 after | 0.051-0.055 before, 0.417-0.421 after | 0.15 | AS not rebuilt: 0.071 after, fails |
| `PathTracingPassesTheWhiteFurnaceTest` | 80 → 40 | mean 231.87, 1.0 within ±6 | 231.83, 1.0 | 225.6-237.6, 0.98 | albedo 0.9: 228.19, 0.869, fails |
| `PathTracingAntiAliasesGeometricEdges` | 80 → 25 | 312 of 312 columns blended | 312 of 312 | over half | no jitter: 0-1 of 312, fails |

- **Where each depth stops working.** At a history of 8 the mask card fails: noise pixels
  outside the card stretch its bounding box (0.139). From 16 on it finds the full variant's box.
  The added model's control picks up noise as the history shortens: 0.111 at 8, 0.078 at 16,
  0.055 at 24, 0.016 at 60. The furnace stays at 1.0 within ±6 from a history of 10. The
  anti-aliasing golden separates at any depth: on one stream 249 of 312 columns blend in a
  2-frame accumulation against 62 through the pixel centre, 302 against 8 at 9 frames, and 312
  against 0 from 25. Its light history is therefore 25, not the 50 it started with: on riscv64
  the 50-frame test alone took 2 h 12 min under QEMU (run 38210484163), and over the five
  streams at 25 the jittered kernel blends 312 of 312 columns and the pixel-centred one 0-1.
  On riscv64 the 25-frame test blends 312 of 312 too, in 60 min of ctest (run 38232793922).
- **Why five streams.** riscv64's llvmpipe follows a different sample path wherever its floating
  point differs, so a margin has to hold across streams, not on one. The four extra streams (a
  seed offset in `path_tracing.slang`, not committed) give the light column's ranges.
- **The controls** are one-line edits, measured and reverted: the path tracer commits every
  candidate without its alpha test, `addModel` skips the AS rebuild, the furnace albedo is 0.9,
  and the primary ray goes through the pixel centre. The full variant fails all four the same
  way. The furnace band is wide: albedo 0.95 passes both variants (mean 230.1).
- **The anti-aliasing golden reads one edge: the slab's top against the sky** (owner decision
  2026-10-12), at y 56 of the 1200x768 frame. `sky_silhouette` (`GoldenMetrics.hpp`) walks each
  column of the panel-free crop down from the sky to its first step of more than 20 levels, so
  the measurement follows the edge if the framing moves. The sky is analytic and the slab's front
  face is seen head-on, so without jitter the edge pixel is slab: per column it sits 0-3 levels
  inside the 42-44-level step, and 5-11 with jitter. More than 4 counts as blended, and the test
  fails when fewer than half the columns blend. Measured over the five streams, both variants:
  312 of 312 columns jittered, 0 of 312 through the pixel centre (0-1 at the light variant's 25
  frames).
- **Why not every edge.** Until 2026-10-12 the oracle counted a partial pixel on any edge in the
  crop and asked for 15 %: 81.4-82.4 % with jitter, 48.8-49.4 % without, so a kernel without
  jitter passed. Penumbrae on the ground (y 362 and 425) and a 1-px row of distant ground under
  the sky (y 320) sit between their neighbours with or without jitter.
- **The no-jitter control stays in the suite.** `GoldenMetrics.SkySilhouetteRejectsAPixelCentredEdge`
  runs `sky_silhouette` over a frame with the levels measured here and a pixel-centred edge pixel,
  and must find none blended; it needs no GPU, so every lane runs it.

Locally:

```bash
KATAGLYPHIS_PT_GOLDEN_VARIANT=light bash ./scripts/linux/run-ctest.sh --virtual-display \
  --build-dir build -- -V -R '^GoldenRender\.(PathTracedMaskCardShowsItsCutout|AddedModelAppearsInPathTracing|PathTracingPassesTheWhiteFurnaceTest|PathTracingAntiAliasesGeometricEdges)$'
```

## Running them on the host GPU

The container build delivers the test executable into the working tree at
`build-clangcl-debug/commitTestSuite.exe`; on a host with a GPU it runs the golden tests for real.
Invoke it directly (the host's `ctest` cannot read the container-generated CMake tree), **from the
repository root**:

```
cd <repo root>
./build-clangcl-debug/commitTestSuite.exe --gtest_filter='GoldenRender.*:Integration.*'
```

**The working directory matters.** The Slang-emitted SPIR-V shaders load from *cwd-relative*
paths (e.g. `Resources/ShadersSlang/build/spirv/...`). Run from `build-clangcl-debug/`, the suite
fails with an access violation / empty-`codeSize` `vkCreateShaderModule`: a wrong cwd, **not** a
code bug. Run from the repo root, where `Resources/` resolves. The default scenes must be there
too: a configure fetches them from AssetANT, `Build-Windows-Container.ps1` does it on the host
before a container build, and `cmake -P cmake/FetchAssets.cmake` does it anywhere else (AGENTS.md
§ Large tracked assets).

The executable carries AddressSanitizer/UBSan (the debug config's flags) and runs fine on the host.

### On a Linux host with an NVIDIA GPU, in the image

The family image runs the suites on a real NVIDIA GPU through CDI (rootless nerdctl, spec
generated by `nvidia-ctk cdi generate`). The spec mounts `nvidia_icd.json` into
`/etc/vulkan/icd.d` and the driver's `libGLX_nvidia` and `libnvidia-*` libraries, so nothing is
added by hand; `vulkaninfo --summary` then lists the GPU beside llvmpipe. Measured 2026-10-01 on
an RTX 2080, driver 595.58.03, `:latest-amd64`:

```bash
nerdctl run --rm --user 0:0 --device nvidia.com/gpu=all \
  -e VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json \
  -e ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0 \
  -v "$PWD:/workspace" -w /workspace ghcr.io/kataglyphis/kataglyphis_beschleuniger:latest-amd64 \
  bash -lc "bash ./scripts/linux/run-ctest.sh --virtual-display --build-dir build -- -R '^(GoldenRender|Integration)\.'"
```

- `VK_ICD_FILENAMES` hides llvmpipe, so the device the log's
  `Selected Vulkan physical device:` line names can only be the GPU.
- `protect_shadow_gap=0` is required by the ASan debug build. ASan reserves the shadow gap, the
  NVIDIA driver wants that address range when a device enables `VK_KHR_acceleration_structure`,
  and `vkCreateDevice` returns -3 (`VK_ERROR_INITIALIZATION_FAILED`). A device without the
  ray-tracing extensions is created either way; a non-ASan build needs neither option.
- `detect_leaks=0`: the driver (`libnvidia-glcore`, `libGLX_nvidia` via xcb) leaks a few KB at
  exit, which LeakSanitizer turns into a failed test.
- Xvfb is enough: the NVIDIA ICD presents to it, and all 39 tests passed on the 2080 in 4.5
  minutes.

## Rust WebGPU suite: requiring a GPU explicitly

The Rust renderer (`third_party/OxidANT/crates/webgpu_renderer`) has the same problem in
miniature: its GPU tests guard on `GpuContext::headless_or_skip()`, which prints `SKIP: no GPU
adapter available in this environment` and returns early when no adapter is usable, so a skipped
test and a passing one are the same `ok` line in `cargo test` output. In the host verification
loop, set `KATAGLYPHIS_REQUIRE_GPU=1` so a missing/unusable adapter panics instead of silently
skipping:

```
$env:KATAGLYPHIS_REQUIRE_GPU=1
cargo test -p kataglyphis_webgpu_renderer
```

A clean run has zero `SKIP: no GPU` lines in the output.

## The verification loop for render/device changes

The canonical per-unit verification pattern; `AGENTS.md` and `docs/source/cpp-renderer-improvements.md`
link here. For a behaviour-preserving refactor of the record path, a pipeline, a device feature,
an image transition, or the loader upload path:

1. Build in the container: `scripts/windows/Build-Windows-Container.ps1
   -Configurations 'clangcl-debug'` (tar-pipe by default; a bind mount on a Dev Drive needs its
   filters allow-listed and measured slower, see `docs/source/container-build-caching.md`).
   **Fresh-container rule:** `-FreshContainer` (after deleting the local build tree) is required
   after ANY module-interface change, or stale BMIs can ASan-fault at member init. That covers
   `.ixx` member edits AND plain shared headers whose structs cross module boundaries
   (`ObjectDescription.hpp` taught this with an exit-3 crash). Body-only `.cpp` and shader edits
   build incrementally.
2. Run the golden + integration suites on the host GPU (an RX 9070 XT here), as above.
3. Where rendering changed, add a validation-layer-clean runtime check: an 8–10 s engine run with
   stderr captured, grepping the validation output.
4. All tests passing = the recorded frames are unchanged = the refactor is render-equivalent. The
   baseline is every runnable `GoldenRender` test (every defined test except the two `DISABLED_`
   tools, `DumpsFrameToPng` and `PcfRadiusSweepMeasuresTheShadowRig`, which do not run by default)
   plus every `Integration` test; the counts are in the machine-readable marker below, which
   `BuildIntegrity.GoldenTestCountsInDocsMatchTheSuite` pins against the suite source.

<!-- golden-counts: defined=43 runnable=41 integration=2 total=43 excluded=3 -->

This makes what the container can only compile-check (device creation, image barriers, the
deferred/forward command streams, path/ray tracing) verifiable.

Shader-only units are cheap: edit a `.slang` source, run `Build-SlangShaders.ps1` (Windows) /
`compile-slang-shaders.sh` (Linux) to refresh the compiled SPIR-V, then run one golden; no C++
rebuild. The `BuildIntegrity` tests check each `.spv` is not older than its `.slang` source. The
output tree (`Resources/ShadersSlang/build/`) is **gitignored, not committed**: a fresh clone must
run the compile script once before the engine has anything to load
([`shader-build-pipeline.md`](shader-build-pipeline.md)).

## Writing a new golden test — cautions learned the hard way

The single home for these instrument cautions; `docs/source/cpp-renderer-improvements.md`,
`docs/source/path-tracing.md` and the suite's one-line comments link here. The expensive mistakes:

- **A validation counter must start before the frames it judges.** The layer reports one
  message id ten times, then goes quiet (its `duplicate_message_limit`), and every sync hazard
  shares one id. A `ScopedValidationErrorCounter` made after the warmup frames counted nothing
  while the log held hazards. Make it after the `EngineHarness`, though: on the arm64 runner the
  loader logs a PowerVR ICD's enumeration failure as an error while the harness creates the
  instance.
- **Captures are tonemapped**, and the **ImGui overlay is composited into them**. The opaque
  ImGui panel covers the LEFT ~70% of the 1200x768 test frame; the panel-free right edge
  (x >= 0.72w) is the scene. A pixel classifier written against raw scene colours (e.g.
  `r < 60 && b < 60`) can silently measure only the overlay, and whole-frame or centre-crop means
  have measured, at various times: the FPS-counter digits, SSAO, the caster's own body, and
  nothing at all. Crop away from the overlay (the existing tests crop to the right/lower scene
  region) and prefer luminance-delta oracles over absolute-colour thresholds.
- **Prefer counting to averaging for sparse signals.** Changed-pixel / swung-pixel / detail
  fractions discriminate where means drown (a UNORM ceiling clamps, a texture is near-greyscale,
  a skeleton is 3% of the frame).
- **A count says how much changed; only the shape says whether what changed is the effect.**
  Always capture an unconditional control (the effect disabled) and a same-state noise reference
  in the same run, and compare against them. Dump amplified diff-map PNGs and *look* at them
  before trusting any new pixel metric: `GoldenRender.DISABLED_DumpsFrameToPng` dumps the frame,
  the control, and an amplified difference to PNG
  (`--gtest_also_run_disabled_tests --gtest_filter=*DumpsFrameToPng*`,
  `KATAGLYPHIS_FRAME_DUMP=out`).

Effects with no runtime toggle (the tonemap is always on) are hard to isolate this way:
"compressed vs clipped" is ambiguous without a control, so a robust oracle must key on a property
the effect uniquely produces (e.g. retained variation), not just "brighter" or "darker".

## The PCF radius bound, measured

`MAX_PCF_RADIUS` (the GUI slider's upper bound, and the clamp in both `clampPcfRadius` and
`cascaded_shadow.slang`) is 5, down from 20 on 2026-10-06. The default radius stays 2, so no
golden moved. The number comes from `GoldenRender.DISABLED_PcfRadiusSweepMeasuresTheShadowRig`,
which renders the shadow rig (`Models/ShadowTest/shadow_rig.obj`, forward raster, shadow
intensity 1) at every radius from 0 to `MAX_PCF_RADIUS`, 35 frames each, and prints a table:

```sh
xvfb-run -a commitTestSuite --gtest_also_run_disabled_tests \
  --gtest_filter=GoldenRender.DISABLED_PcfRadiusSweepMeasuresTheShadowRig
```

`KATAGLYPHIS_FRAME_DUMP=out` also writes `out-pcf<r>.png` per radius. A shadows-off capture at
each radius masks every pixel the overlay moves (the slider text, the FPS digits). The rest of the
frame splits in two:

- **near**: within 16 px of the radius-0 shadow, where penumbra softening shows;
- **far-lit**: everything else, lit at radius 0, which PCF should leave alone.

Measured in `:latest` on llvmpipe, Debug build, 1200x768; two runs agreed within half a
percentage point:

| Radius | Taps | Near: pixels a +1 step moves >= 3 levels | Near: mean step change (levels) | Far-lit: mean darkening (levels) | Far-lit: darkened >= 3 levels | Main pass (ms) |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | 1 | - | - | 0.00 | 0 % | 4.1 |
| 1 | 9 | 26.9 % | 2.16 | 0.02 | 0 % | 4.7 |
| 2 | 25 | 34.3 % | 2.41 | 0.90 | 15.4 % | 5.6 |
| 3 | 49 | 29.8 % | 1.56 | 1.81 | 39.2 % | 8.1 |
| 4 | 81 | 22.3 % | 1.07 | 2.44 | 39.3 % | 11.1 |
| 5 | 121 | 5.3 % | 0.81 | 2.89 | 39.4 % | 15.6 |
| 6 | 169 | 1.9 % | 0.65 | 3.25 | 39.4 % | 21.0 |
| 8 | 289 | 1.7 % | 0.38 | 3.76 | 39.5 % | 33.7 |
| 12 | 625 | 1.3 % | 0.38 | 4.34 | 39.6 % | 72.9 |
| 16 | 1089 | 1.4 % | 0.20 | 4.66 | 39.6 % | 121.4 |
| 20 | 1681 | 1.3 % | 0.20 | 4.86 | 39.6 % | 181.4 |

The threshold: a step is visible when it moves at least 2 % of the near pixels by 3 or more
luminance levels (of 255, about 1.2 %). Radius 5 is the last step over it. Every step after it
stays at 1.3 to 1.9 % with a mean change of 0.2 to 0.65 levels, while the Main pass grows from
15.6 ms to 181 ms. Any threshold between 2 % and 5 % gives the same bound.

Large radii add mostly an artifact, not softness. Every tap compares against the centre texel's
depth with one constant bias, so on a receiver sloped against the light the outer taps shadow it.
Lit surfaces far from any shadow darken from radius 2 on, and the darkening keeps growing with the
radius. A receiver-plane depth bias would fix it (BACKLOG.md, C++ engine ideas).

## Synchronization validation

Vulkan's `khronos_validation.validate_sync` layer setting catches a bug class no pixel oracle
sees: a missing or incorrect barrier between two GPU commands that read or write the same
resource (WRITE-AFTER-WRITE, READ-AFTER-WRITE, WRITE-AFTER-READ). It found 10 real
WRITE-AFTER-WRITE hazards in July 2026.

**Linux CI runs it on every push since 2026-10-06.** `run-ctest.sh --virtual-display` sets
`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1` for the GPU suites and fails the run when ctest's log
holds a `SYNC-HAZARD` line, naming the tests that logged one. Most tests ignore validation
messages, so the log is the only place a hazard shows. On llvmpipe it costs about 2%: the 42 GPU
tests took 921 s with it and 900 s without, on 32 cores. Its first run found two
WRITE-AFTER-WRITE classes, both fixed the same day:

- **The deferred render pass** (`DeferredRasterizer.cpp`). The geometry-to-lighting dependency
  left the G-buffers' store ops, and depth's load-op clear, outside its scopes. The
  lighting-to-external one left depth's store before the final layout transition.
- **Back-to-back BLAS builds** (`ASManager.cpp`). One scratch buffer serves every model, and the
  barrier between builds only made the next one *read*.

`GoldenRender.DeferredFramesAndBlasRebuildsAreFreeOfSyncHazards` pins both: it turns sync
validation on for its own harness, so it holds on any lane with validation layers.

**Windows x64 validates too since 2026-10-06**, though its GPU suites run in Release.
`Build-Windows.ps1 -StageTests` copies the image's `VkLayer_khronos_validation` for the target
arch (`C:\runtime\vulkan-layers`, hub CON64) into `vulkan-layers\` beside the staged suite, and
fails when the image has none. `Invoke-LavapipeTests.ps1` registers its JSON under
`HKLM\SOFTWARE\Khronos\Vulkan\ExplicitLayers` for the run (`HKCU` when not elevated), because an
elevated loader, as on the runner, ignores `VK_ADD_LAYER_PATH`. It sets
`KATAGLYPHIS_VULKAN_VALIDATION=1` (the switch `validationLayersEnabled()` reads in a Release
build) and `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1`, and fails a test whose captured output holds a
`VUID-` or `SYNC-HAZARD` line. Windows arm64 validates the same way since the `:winarm64` of
2026-10-10 carries the hub's arm64 build of the layer (CON64; run 38100828628 staged it).

On a host GPU, the manual run after touching render passes, barriers, or frames-in-flight is:

```
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Invoke-SyncValidation.ps1
```

It uses the same `commitTestSuite.exe` as above (repo root or `build-clangcl-debug\`, same
working-directory requirement), copies `scripts/vk_layer_settings.txt` next to it for the run (the
Vulkan loader reads `vk_layer_settings.txt` from the CWD or the executable's directory; there is
no path env var for it), and exits non-zero with a per-hazard summary if the run's log contains
`SYNC-HAZARD`.

## Known issue: path-tracing compute device-lost on the large dinosaur mesh

`GoldenRender.PathTracingAccumulatesAndConverges`,
`GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice` and
`Integration.RenderModesSelectableInGui` hard-aborted (`VK_ERROR_DEVICE_LOST`, zero preceding
validation errors) on the RX 9070 XT when path tracing ran against the shipped debug scene
(`Models/Dinosaurs/dinosaurs.obj`, hundreds of thousands of vertices); GPU capture tooling
(RenderDoc/PIX/AMD crash dumps) was not available here. The cause was found 2026-08-05 and the
three now pass on llvmpipe in CI. What is left is the `BACKLOG.md` entry "Confirm on the RX 9070
XT that the path-tracing device loss is gone", which also says when to retire this section. The
full bisection was the "Localize (and fix if cheap) the path-tracing compute
`VK_ERROR_DEVICE_LOST`" entry, in `BACKLOG.md`'s history before `dbcc8873`.

As of 2026-07-31 the bug is confirmed scoped to `path_tracing.slang`/RayQuery compute, not the
shared vertex-upload/buffer-device-address path:
`GoldenRender.RaytracedLargeMeshDoesNotLoseTheDevice` raytraces the identical dinosaur mesh
through the RT *pipeline* (`raytrace.rchit.slang`, same `Vertices*` BDA read pattern) and does
not lose the device. Excluding the three tests with
`--gtest_filter='GoldenRender.*:Integration.*:-GoldenRender.PathTracingAccumulatesAndConverges:GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice:Integration.RenderModesSelectableInGui'`
runs `total - excluded` tests (see the `golden-counts` marker above). A 2026-08-01 run on the
RX 9070 XT, against the 28-test suite of then, ran clean: "28 tests from 2 test suites ran",
"PASSED 28 tests", 1 `DISABLED_` test not run. That is a record, not a claim about today's suite.
