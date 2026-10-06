# GPU golden testing

The `GoldenRender.*` and `Integration.*` suites in
`Test/commit/VulkanEngine/` render real frames on a real GPU and assert on the
captured pixels. They are the end-to-end safety net for render- and
device-path changes that the CPU unit suites cannot cover.

## They skip without a GPU

Each of these tests begins with `SKIP_WITHOUT_GPU()` (or an equivalent
GLFW/Vulkan-runtime guard). In the headless container build
(`scripts/windows/Build-Windows-Container.ps1 -RunTests`) there is no GPU, so
they report **skipped**, not passed. A green container build therefore proves
the code *compiles and links* and that the CPU suites pass — it does **not**
prove a render/device refactor is behaviour-preserving.

## Linux CI runs them on llvmpipe

Since 2026-09-29 the family Linux image ships lavapipe/llvmpipe, a CPU Vulkan
device with ray tracing, and `Xvfb`. The Linux `clang-tests` job
(`reusable-linux.yml`, x64 and arm64) therefore runs the GPU suites for real:
`run-ctest.sh --virtual-display` starts the run under `xvfb-run`, so
`glfwInit()` finds an X server and nothing skips. Locally, the same:

```bash
bash ./scripts/linux/run-ctest.sh --virtual-display --build-dir build --build-type Debug
```

What that does not cover:

- **`GuiInputSweepNeverCrashesOrLosesTheDevice` is too slow for a runner.** It
  passes on llvmpipe (266 s on 32 cores; 12 s on an RTX 2080) but hit ctest's
  1500 s timeout on a 4-vCPU GitHub runner (run 36627716804), so CI excludes
  it. It is the only exclusion: the five goldens once excluded here failed on
  an RTX 2080 too, because their oracles were wrong, and were fixed
  (`BACKLOG.md`, "GPU suites on llvmpipe").
- **The image sets `LP_NATIVE_VECTOR_WIDTH=256`** (hub CON44, `:latest` of
  2026-10-01; `--virtual-display` pinned it here before that). Mesa 26.0's lavapipe builds acceleration structures with a radix sort
  whose shaders are compiled for 8-lane subgroups
  (`lvp_acceleration_structure.c`, `subgroup_size_log2 = 3`), but llvmpipe's
  subgroup is `native vector width / 32` lanes: 8 on an AVX2 host, 4 on arm64's
  128-bit NEON. At 4 lanes the scatter pass (`rs_scatter_smem`) writes through
  wild per-lane addresses and every test that builds a BVH SEGVs inside
  llvmpipe's JIT code (arm64 run 36746313937; on x64,
  `LP_NATIVE_VECTOR_WIDTH=128` reproduces it). 256 is x64's default already and
  on arm64 LLVM splits each vector into two NEON registers. Mesa main replaced
  the radix sort with a merge sort (`ebcfbe60`, 2026-08-22), so the hub drops
  its `ENV` once the image's Mesa carries that. The Windows lavapipe run
  (`Invoke-LavapipeTests.ps1`) uses its own Mesa and keeps the pin.
- **Only that one Linux job runs them.** They cost ~20 minutes of llvmpipe on a
  32-core host (~55 on a 4-vCPU runner); the ASan, TSan and gcc jobs keep
  excluding them.
- **The Windows container run excludes them** (`$gpuOnlySuites`) - the runner runs
  them instead, below - although the hub image carries lavapipe since 2026-10-04
  (CON50) and its own smoke proves the device.
- A software rasterizer is not the RX 9070 XT. A pass on llvmpipe is a strong
  behavioural signal, not a substitute for the host loop below after a
  render/device change.

## Windows CI runs them on lavapipe

Both Windows lanes run the same suites on Mesa's lavapipe, outside the image:
the hub image carries its own device since 2026-10-04 (CON50), but a host-run
  suite needs the host's ICD, and a hosted runner takes one in a minute. `scripts/windows/Invoke-LavapipeTests.ps1`
does it for either arch:

- **What it installs.** lavapipe from
  [mmozeiko/build-mesa](https://github.com/mmozeiko/build-mesa) 26.2.3 (the one
  build that ships an arm64 lavapipe; mesa-dist-win has x86 and x64 only) and the
  Khronos loader from LunarG's 1.4.357.0 runtime components, both pinned by
  SHA256. `vulkan-1.dll` goes beside the suite, so no System32 copy wins.
- **How the loader finds it.** `VK_DRIVER_FILES` names the ICD, but the loader
  ignores it in an elevated process, which a hosted runner's is; so it is also
  registered under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`.
  `VK_LOADER_DRIVERS_SELECT` keeps any other driver out, and `vulkaninfo
  --summary` must list llvmpipe before a test runs.
- **How it runs them.** Each test in its own process, from the repo root, with
  ctest's 1500 s limit, as ctest does on Linux; the GUI input sweep stays out for
  time. Any failure, any skip or no pass fails the step: a skip here is a test
  that did not run.
- **Which build.** The Release commit suite that `Build-Windows.ps1 -StageTests`
  stages, on both arches: a clean host has no validation layers for the Debug one, so the
  arm64 lane's Debug (ASan) suite skips the same GPU suites and runs beside it since
  2026-10-03. x64 runs it on the runner host after the
  container build (`windows-x64.yml`'s `host-command`); arm64 in the `gpu-suites`
  job on `windows-11-arm`, from a checkout, which also runs the source-reading
  `BuildIntegrity.*` checks the staged tree lacks. The script sets
  `LP_NATIVE_VECTOR_WIDTH=256` itself, which on Linux the image does.
- **Release frames the same view.** A Release app opens on crytek-sponza from
  inside, a Debug one on Dinosaurs from (0, 6, 26) (`Camera.cpp`,
  `SceneConfig.cpp`), and the goldens were calibrated on the latter; the first
  Release run failed 14 of 40 on framing alone (cards out of view, 13 of 373
  meshes "visible"). So `EngineHarness` pins that scene, unless the test picked
  its own, and that camera, whatever the build type.
- **The runner's desktop must hold the window.** Windows clamps a new window to
  the desktop, and `windows-11-arm`'s is 1024x768, which shrank the 1200x768
  frame to 1004x749; the script switches a CI runner's display mode to fit
  (`ChangeDisplaySettings`), and never a workstation's.

`commitTestSuite` carries its own LeakSanitizer suppression for libX11
(`Test/commit/VulkanEngine/lsanSuppressions.cpp`): `glfwInit()` on X11 leaks
libX11's locale state, which failed every GPU test in an ASan build.

## Running them on the host GPU

The container build delivers the built test executable back into the working
tree at `build-clangcl-debug/commitTestSuite.exe`. On a host with a GPU it runs
the golden tests for real. Invoke the executable directly — the host's `ctest`
cannot read the container-generated CMake tree — and run it **from the
repository root**:

```
cd <repo root>
./build-clangcl-debug/commitTestSuite.exe --gtest_filter='GoldenRender.*:Integration.*'
```

**The working directory matters.** The Slang-emitted SPIR-V shaders are loaded
via *cwd-relative* paths (e.g. `Resources/ShadersSlang/build/spirv/...`). Running
from `build-clangcl-debug/` fails with an access violation /
empty-`codeSize` `vkCreateShaderModule` — that is a wrong cwd, **not** a code
bug. Run from the repo root, where `Resources/` resolves.

The executable carries AddressSanitizer/UBSan (the debug config's flags); it
runs fine on the host.

### On a Linux host with an NVIDIA GPU, in the image

The family image runs the suites on a real NVIDIA GPU through CDI (rootless
nerdctl, spec generated by `nvidia-ctk cdi generate`). The spec mounts
`nvidia_icd.json` into `/etc/vulkan/icd.d` and the driver's `libGLX_nvidia`
and `libnvidia-*` libraries, so nothing has to be added by hand; `vulkaninfo
--summary` then lists the GPU beside llvmpipe. Measured 2026-10-01 on an
RTX 2080, driver 595.58.03, `:latest-amd64`:

```bash
nerdctl run --rm --user 0:0 --device nvidia.com/gpu=all \
  -e VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json \
  -e ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0 \
  -v "$PWD:/workspace" -w /workspace ghcr.io/kataglyphis/kataglyphis_beschleuniger:latest-amd64 \
  bash -lc "bash ./scripts/linux/run-ctest.sh --virtual-display --build-dir build -- -R '^(GoldenRender|Integration)\.'"
```

- `VK_ICD_FILENAMES` hides llvmpipe, so the device the log's `Selected Vulkan
  physical device:` line names can only be the GPU.
- `protect_shadow_gap=0` is required by the ASan debug build. ASan reserves the
  shadow gap, the NVIDIA driver wants that address range when a device enables
  `VK_KHR_acceleration_structure`, and `vkCreateDevice` returns -3
  (`VK_ERROR_INITIALIZATION_FAILED`). A device without the ray-tracing
  extensions is created either way; a non-ASan build needs neither option.
- `detect_leaks=0`: the driver (`libnvidia-glcore`, `libGLX_nvidia` via xcb)
  leaks a few KB at exit, which LeakSanitizer turns into a failed test.
- Xvfb is enough: the NVIDIA ICD presents to it, and all 39 tests passed on
  the 2080 in 4.5 minutes.

## Rust WebGPU suite: requiring a GPU explicitly

The Rust renderer (`third_party/OxidANT/crates/webgpu_renderer`)
has the same problem in miniature: its GPU tests guard on
`GpuContext::headless_or_skip()` and print `SKIP: no GPU adapter available in
this environment` and return early when no adapter is usable, so a skipped
test and a passing test are the same `ok` line in `cargo test` output. Set
`KATAGLYPHIS_REQUIRE_GPU=1` when running this host verification loop so a
missing/unusable adapter panics instead of silently skipping:

```
$env:KATAGLYPHIS_REQUIRE_GPU=1
cargo test -p kataglyphis_webgpu_renderer
```

A clean run has zero `SKIP: no GPU` lines in the output.

## The verification loop for render/device changes

This is the canonical per-unit verification pattern; `AGENTS.md` and
`docs/cpp-renderer-improvements.md` link here rather than restating it.

For a behaviour-preserving refactor of the record path, a pipeline, a device
feature, an image transition, or the loader upload path:

1. Build in the container: `scripts/windows/Build-Windows-Container.ps1
   -Configurations 'clangcl-debug'` (the tar-pipe transport is the default; a
   bind mount on a Dev Drive needs its filters allow-listed and measured
   slower, see `docs/container-build-caching.md`). Fresh-container rule: `-FreshContainer` (after
   deleting the local build tree) is required after ANY module-interface
   change — `.ixx` member edits AND plain shared headers whose structs cross
   module boundaries (`ObjectDescription.hpp` taught this with an exit-3
   crash) — otherwise stale BMIs can ASan-fault at member init. Body-only
   `.cpp` and shader edits build incrementally.
2. Run the golden + integration suites on the host GPU (an RX 9070 XT here)
   as above.
3. Where rendering changed, add a validation-layer-clean runtime check: an
   8–10 s engine run with stderr captured, grepping the validation output.
4. All tests passing = the recorded frames are unchanged = the refactor is
   render-equivalent. The baseline is every runnable `GoldenRender` test
   (every defined test except `DISABLED_DumpsFrameToPng`, which does not run
   by default) plus every `Integration` test - see the machine-readable
   counts below, which `BuildIntegrity.GoldenTestCountsInDocsMatchTheSuite`
   pins against the suite source.

<!-- golden-counts: defined=42 runnable=41 integration=2 total=43 excluded=3 -->

This turns changes the container can only compile-check (device creation,
image barriers, the deferred/forward command streams, path/ray tracing) into
verifiable ones.

Shader-only units are cheap: edit a `.slang` source, run
`Build-SlangShaders.ps1` (Windows) / `compile-slang-shaders.sh` (Linux) to
refresh the compiled SPIR-V, then run one golden — no C++ rebuild needed.
The `BuildIntegrity` tests check each `.spv` is not older than its `.slang`
source. Note the output tree (`Resources/ShadersSlang/build/`) is **gitignored,
not committed** — a fresh clone must run the compile script once before the
engine has anything to load (see
[`shader-build-pipeline.md`](shader-build-pipeline.md)).

## Writing a new golden test — cautions learned the hard way

This is the single home for these instrument cautions;
`docs/cpp-renderer-improvements.md` and `docs/path-tracing.md` link here.
The expensive mistakes, which the suite's one-line comments point back to:

- **A validation counter must start before the frames it judges.** The layer reports one
  message id ten times, then goes quiet (its `duplicate_message_limit`), and every
  sync hazard shares one id. A `ScopedValidationErrorCounter` made after the warmup
  frames counted nothing while the log held hazards. Make it after the `EngineHarness`,
  though: on the arm64 runner the loader logs a PowerVR ICD's enumeration failure as an
  error while the harness creates the instance.
- **Captures are tonemapped**, and the **ImGui overlay is composited into
  them**. The opaque ImGui panel covers the LEFT ~70% of the 1200x768 test
  frame — the panel-free right edge (x >= 0.72w) is the scene. A pixel
  classifier written against raw scene colours (e.g. `r < 60 && b < 60`) can
  silently measure only the overlay, and whole-frame or centre-crop means
  have measured, at various times: the FPS-counter digits, SSAO, the
  caster's own body, and nothing at all. Crop away from the overlay (the
  existing tests crop to the right/lower scene region) and prefer
  luminance-delta oracles over absolute-colour thresholds.
- **Prefer counting to averaging for sparse signals.** Changed-pixel /
  swung-pixel / detail fractions discriminate where means drown (a UNORM
  ceiling clamps, a texture is near-greyscale, a skeleton is 3% of the
  frame).
- **A count says how much changed; only the shape says whether what changed is
  the effect.** Always capture an unconditional control (the effect disabled)
  and a same-state noise reference in the same run, and compare against them.
  Dump amplified diff-map PNGs and *look* at them before trusting any new
  pixel metric: `GoldenRender.DISABLED_DumpsFrameToPng` dumps the frame, the
  control, and an amplified difference to PNG — use it
  (`--gtest_also_run_disabled_tests --gtest_filter=*DumpsFrameToPng*`,
  `KATAGLYPHIS_FRAME_DUMP=out`) to see what a metric is really measuring.

Effects with no runtime toggle (the tonemap is always on) are hard to isolate
this way: "compressed vs clipped" is ambiguous without a control, so a robust
oracle must key on a property the effect uniquely produces (e.g. retained
variation), not just "brighter" or "darker".

## Synchronization validation

The golden/integration suites above catch behaviour-visible regressions;
Vulkan's `khronos_validation.validate_sync` layer setting catches a
different class of bug that a pixel oracle cannot see: a missing or
incorrect barrier between two GPU commands that read or write the same
resource (WRITE-AFTER-WRITE, READ-AFTER-WRITE, WRITE-AFTER-READ). It found
10 real WRITE-AFTER-WRITE hazards in July 2026.

**Linux CI runs it on every push since 2026-10-06.** `run-ctest.sh --virtual-display`
sets `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1` for the GPU suites and fails the run when
ctest's log holds a `SYNC-HAZARD` line, naming the tests that logged one. Most tests
ignore validation messages, so the log is the only place a hazard shows. On llvmpipe it
costs about 2%: the 42 GPU tests took 921 s with it and 900 s without, on 32 cores.
Its first run found two WRITE-AFTER-WRITE classes, both fixed the same day:

- **The deferred render pass** (`DeferredRasterizer.cpp`). The geometry-to-lighting
  dependency left the G-buffers' store ops, and depth's load-op clear, outside its
  scopes. The lighting-to-external one left depth's store before the final layout
  transition.
- **Back-to-back BLAS builds** (`ASManager.cpp`). One scratch buffer serves every
  model, and the barrier between builds only made the next one *read*.

`GoldenRender.DeferredFramesAndBlasRebuildsAreFreeOfSyncHazards` pins both: it turns sync
validation on for its own harness, so it holds on any lane with validation layers.

**Windows x64 validates too since 2026-10-06**, though its GPU suites run in Release.
`Build-Windows.ps1 -StageTests` copies the image's `VkLayer_khronos_validation`
(`$env:VULKAN_SDK\Bin`, 1.4.357) into `vulkan-layers\` beside the staged suite.
`Invoke-LavapipeTests.ps1` then sets `VK_ADD_LAYER_PATH` there,
`KATAGLYPHIS_VULKAN_VALIDATION=1` (the switch `validationLayersEnabled()` reads in a Release
build) and `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1`. It fails a test whose captured output
holds a `VUID-` or `SYNC-HAZARD` line. Windows arm64 runs unvalidated: the image carries no
arm64 layer. On a host GPU, the manual run after touching render passes, barriers, or
frames-in-flight is:

```
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Invoke-SyncValidation.ps1
```

This builds on the same `commitTestSuite.exe` as above (repo root or
`build-clangcl-debug\`, same working-directory requirement), copies
`scripts/vk_layer_settings.txt` next to it for the duration of the run (the
Vulkan loader reads `vk_layer_settings.txt` from the CWD or the executable's
directory - there is no path env var for it), and exits non-zero with a
per-hazard summary if the run's log contains `SYNC-HAZARD`.

## Known issue: path-tracing compute device-lost on the large dinosaur mesh

`GoldenRender.PathTracingAccumulatesAndConverges`,
`GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice` and
`Integration.RenderModesSelectableInGui` currently hard-abort
(`VK_ERROR_DEVICE_LOST`, zero preceding validation errors) on the RX 9070 XT
when path tracing runs against the shipped debug scene
(`Models/Dinosaurs/dinosaurs.obj`, hundreds of thousands of vertices). Root
cause is not yet found (needs GPU capture tooling — RenderDoc/PIX/AMD crash
dumps — not available here); see the "Localize (and fix if cheap) the
path-tracing compute `VK_ERROR_DEVICE_LOST`" entry in `BACKLOG.md` for the
full bisection.

As of 2026-07-31 the bug is confirmed scoped to `path_tracing.slang`/RayQuery
compute specifically, not the shared vertex-upload/buffer-device-address
path: `GoldenRender.RaytracedLargeMeshDoesNotLoseTheDevice` raytraces the
identical dinosaur mesh through the RT *pipeline* (`raytrace.rchit.slang`,
same `Vertices*` BDA read pattern) and does not lose the device. Excluding
the three tests above with
`--gtest_filter='GoldenRender.*:Integration.*:-GoldenRender.PathTracingAccumulatesAndConverges:GoldenRender.GuiInputSweepNeverCrashesOrLosesTheDevice:Integration.RenderModesSelectableInGui'`
runs the rest of the suite clean: `total - excluded` = 38 tests today (see the
`golden-counts` marker above). This is a record of a 2026-08-01 run against
the 28-test suite that existed then on the RX 9070 XT — "28 tests from 2
test suites ran", "PASSED 28 tests", 1 `DISABLED_` test not run — not a
claim about today's suite.
