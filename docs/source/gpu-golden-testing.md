# GPU golden testing

The `GoldenRender.*` and `Integration.*` suites in `Test/commit/VulkanEngine/` render real frames
on a real GPU and assert on the captured pixels: the end-to-end net for render- and device-path
changes that the CPU unit suites cannot cover. `GoldenRender` keeps one subject per
`goldenRender*Suite.cpp` (frame, scene, light, sky, material, tracing, cloud) over the header-only
`GoldenRenderHarness.hpp`, the engine harness and pixel helpers. The header imports the engine's
modules, so it is a file's last include (gcc rejects an include after an import), and the file
imports the renderer too: IWYU and Windows clang-tidy look for an import in the file itself to know
they cannot read it, and `Test/commit/VulkanEngine/CMakeLists.txt` for that line to put the unit in
GCC's pool of one. A GCC Debug unit that imports the renderer holds 5-6 GB however short (clang
about 1 GB), so the goldens are seven files, not more, and two of them never compile together.

## They skip without a GPU

Each test starts with `SKIP_WITHOUT_GPU()` from `Test/commit/VulkanEngine/GpuTestGuard.hpp`, the one
GLFW/Vulkan-runtime guard (`BuildIntegrity.EveryGpuTestGuardsThroughTheSharedHeader`); a test on a
compute device of its own, without GLFW, passes its open's failure to `SKIP_WITHOUT_HEADLESS_DEVICE`
from the same header instead. The headless
container build (`scripts/windows/Build-Windows-Container.ps1 -RunTests`) has no GPU, so they report
**skipped**, not passed. A green container build proves the code *compiles and links* and the CPU
suites pass; it does **not** prove a render/device refactor preserves behaviour. With
`KATAGLYPHIS_REQUIRE_GPU` set and non-empty (OxidANT's variable), the guard and
`SKIP_WITHOUT_FRAME_CAPTURE` fail the test instead of skipping it; the hardware-RT guards still skip,
which the Linux runs then fail (below).

## Linux CI runs them on llvmpipe

Since 2026-09-29 the family Linux image ships lavapipe/llvmpipe, a CPU Vulkan device with ray
tracing, and `Xvfb`. The Linux `clang-tests` job (`reusable-linux.yml`, x64 and arm64) runs the
GPU suites for real: `run-ctest.sh --virtual-display` starts the run under `xvfb-run`, so
`glfwInit()` finds an X server, and exports `KATAGLYPHIS_REQUIRE_GPU=1`, so a broken Xvfb or GLFW
platform fails every GPU test instead of skipping it (the riscv64 GPU shards set it too). Since
2026-11-19 it also passes the hub's `--fail-on-skipped '^(GoldenRender|Integration)\.'` (the riscv64
shards too, through `ctest_run_main`), which fails the run on any ctest Skipped of those suites, the
hardware-RT `GTEST_SKIP`s included: llvmpipe has ray tracing, and none of the 96 skipped that day; a
test that cannot run there must be named, not skipped. The two Disabled goldens never count. Locally,
with `--virtual-display` as the first argument (the script checks only that one):

```bash
bash ./scripts/linux/run-ctest.sh --virtual-display --build-dir build --build-type Debug
```

- **Only that one Linux job runs them.** They cost ~20 minutes of llvmpipe on a 32-core host (~55
  on a 4-vCPU runner); the sanitizer and gcc jobs keep excluding them.
- **Xvfb runs with `-noreset`** (2026-11-15). Every golden probes GLFW and terminates it before its
  window opens; that probe was the server's last client, so Xvfb reset, and a window opened during
  the reset failed with `GLFW Error 65550: Failed to detect any supported platform`, one test in a
  few full runs (`RaytracedLargeMeshDoesNotLoseTheDevice`, then
  `Integration.RasterSegmentCancelsAPendingTracingPick`). `run-riscv64-tests.sh` passes it too.
- **`GuiInputSweepNeverCrashesOrLosesTheDevice` is too slow for a push.** It passes on llvmpipe
  (266 s on 32 cores; 613 s on 8 aarch64 cores with sync validation, 2026-11-16; 12 s on an RTX
  2080; 862 s on a 4-vCPU runner in run 43948778560) but once hit ctest's 1500 s timeout there (run
  36627716804), so every push
  excludes it. `linux-x64.yml`'s `gui-sweep` job runs it alone, Sundays and on a dispatch with
  `gui-sweep`: the `linux-debug-clang` build, then `run-ctest.sh --virtual-display` with
  `--timeout 3600`, nothing else of the lane. It is the only exclusion: the five goldens once excluded
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
  each vector into two NEON registers, which is the suspect of the intermittent arm64 crashes
  (`BACKLOG.md`, golden-render cases on the arm64 device). Mesa main replaced the radix sort with a merge sort
  (`ebcfbe60`, 2026-08-22), so the hub drops its `ENV` once the image's Mesa carries that. The
  Windows lavapipe run (`Invoke-LavapipeTests.ps1`) uses its own Mesa and keeps the pin.
- **The llvmpipe run loads lavapipe only, and ASan builds carry a libX11 leak suppression.**
  `run-ctest.sh --virtual-display` sets `VK_LOADER_DRIVERS_SELECT='*lvp*'` unless the caller names an ICD
  (`VK_ICD_FILENAMES`, `VK_DRIVER_FILES` or the variable itself, as the NVIDIA recipe below does). On GitHub's
  `ubuntu-26.04` runner image 20261116.202.1 another of the image's ICDs found a DRM node (Mesa's PowerVR ICD
  stopped warning that it found none) and left a block and its threads' memory behind while the loader
  enumerated devices; LeakSanitizer then failed every x64 clang GPU test (runs 44753690114, 44761639606,
  44770349057), with the container image of the green run before; with lavapipe alone run 44784592908 passed
  899 of 899 there. A suppression of the loader frame (`setup_loader_term_phys_devs`) is no substitute: it
  removed the direct block, and the threads' blocks it holds were still reported as indirect leaks.
  `glfwInit()` on X11 leaks libX11's locale state, which failed every GPU test in an ASan build, so
  `commitTestSuite` carries its own LeakSanitizer suppression (`Test/commit/VulkanEngine/lsanSuppressions.cpp`).
  A test that only probes, as `Integration.SharedSlangKernelsMeetTheirVectors` did until it took
  `SKIP_WITHOUT_HEADLESS_DEVICE`, has libX11 unloaded by exit, so the same leak shows with no X11 frame;
  a second line names `glfwInit` (2026-11-21) and stays for such a probe (owner decision 2026-11-21).
- **A software rasterizer is not the RX 9070 XT.** A pass on llvmpipe is a strong behavioural
  signal, not a substitute for the host loop below after a render/device change.

## Windows CI runs them on lavapipe

The Windows container run excludes them (`$gpuOnlySuites`). Both Windows lanes run them on the
runner instead, on Mesa's lavapipe outside the image: the hub image carries its own device since
2026-10-04 (CON50), and its own smoke proves it, but a host-run suite needs the host's ICD, and a
hosted runner takes one in a minute. `scripts/windows/Invoke-LavapipeTests.ps1` does it for either
arch:

- **What it installs.** lavapipe from [mmozeiko/build-mesa](https://github.com/mmozeiko/build-mesa)
  at the hub's `LAVAPIPE_VERSION` (26.3.0 since 2026-11-14; the one build that ships an arm64
  lavapipe, as mesa-dist-win has x86 and x64 only) and the Khronos loader from LunarG's runtime
  components at the hub's `VULKAN_VERSION` (1.4.363.0), both pinned by SHA256.
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
- **Crash dumps.** Every test executable links the hub's handler
  (`kataglyphis_add_crash_dump` in `Test/cmake/CommonTestBuild.cmake`), because the runner writes no
  WER `LocalDumps`. The ARM64 job sets `KATAGLYPHIS_CRASH_DUMP_DIR`, so a crash on any thread prints
  `kataglyphis-crash:` lines with the exception and the faulting thread's `module+0xRVA` frames into
  the test's stderr, and writes a minidump. The job uploads it with `commitTestSuite.exe` and its
  PDB as `arm64-lavapipe-crash-dump-<attempt>`; `llvm-symbolizer --obj=commitTestSuite.exe` names a
  frame in the suite.
- **Which build.** The Release commit suite that `Build-Windows.ps1 -StageTests` stages, on both
  arches: a clean host has no validation layers for the Debug one, so the arm64 lane's Debug
  (ASan) suite, run beside it since 2026-10-03, skips the same GPU suites. x64 runs it on the
  runner host after the container build (`windows-x64.yml`'s `host-command`); arm64 in the
  `gpu-suites` job on `windows-11-arm`, from a checkout, which also runs the source-reading
  `BuildIntegrity.*` checks the staged tree lacks. From lavapipe 26.3.0 on, the hub's installer
  leaves `LP_NATIVE_VECTOR_WIDTH` unset, so arm64 runs llvmpipe at its native 128 bits; the Linux
  image still sets 256 while its Mesa predates 26.3.
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
| `PathTracedMaskCardShowsItsCutout` | 60 → 24 | 0.355; 0.350 since 2026-11-16 (aarch64 llvmpipe) | 0.354-0.356; 0.352 (one stream) | 0.15-0.49 | alpha test ignored: 0.631; 0.628 since 2026-11-16, fails |
| `AddedModelAppearsInPathTracing` | 60 → 24 | 0.016 before, 0.403 after; at steps of 48 levels since 2026-11-16, 0.0010 before, 0.201 after | 0.051-0.055 before, 0.417-0.421 after; 0.0016 before, 0.200 after (one stream) | 0.15; 0.1 since 2026-11-16 | AS not rebuilt: 0.071 after; 0.0019 since 2026-11-16, fails |
| `PathTracingPassesTheWhiteFurnaceTest` | 8 → 4 per card view, 80 → 40 in the rig | card within 0.0005 of 1, rig (roughness 0.5) 0.9997, 1.0 within ±0.1 (2026-11-21, aarch64 llvmpipe, one stream) | card within 0.0009 of 1, rig 0.9996, 0.9996 (one stream) | card 0.01 of 1, rig 0.994-1.005, 0.98 | albedo 0.99: rig 0.9889, fails; albedo 1.01: rig 1.0106, fails |
| `PathTracingAntiAliasesGeometricEdges` | 80 → 25 | 312 of 312 columns blended | 312 of 312 | over half | no jitter: 0-1 of 312, fails |

- **At the Sun of 2026-11-23** (OxidANT's, radiance 5 and warm; the anti-aliasing golden lights the rig
  at radiance 10), aarch64 llvmpipe, one stream, full and light: the mask card 0.351 and 0.352 of its
  box, the added model's box 0.0016 and 0.0018 before the add, 0.220 and 0.220 after, the furnace rig
  0.99967 and 0.99963, the edge 312 of 312 columns blended and 310 of 312 on steps of 37-42 levels.
  Every margin holds; the table's columns are the measurements at the old Sun (radiance 10, white).
- **Where each depth stops working.** At a history of 8 the mask card fails: noise pixels
  outside the card stretch its bounding box (0.139). From 16 on it finds the full variant's box.
  The added model's control picks up noise as the history shortens: 0.111 at 8, 0.078 at 16,
  0.055 at 24, 0.016 at 60. The 8-bit furnace stayed at 1.0 within ±6 from a history of 10;
  the HDR one (2026-11-15) is measured on one stream only. The
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
  candidate without its alpha test, `addModel` skips the AS rebuild, the furnace albedo is 0.95
  (0.9 for the 8-bit furnace), and the primary ray goes through the pixel centre. The full
  variant fails all four the same way. The 8-bit furnace's band was wide: albedo 0.95 passed
  both variants (mean 230.1).
- **A bounce sees the drawn sky since 2026-11-16** (the Miss bullet of `docs/source/path-tracing.md`):
  DOOM2016 times 0.35 is about a seventeenth of the old gradient, so the slab's shadow on the
  shadow rig is lit by interreflection alone, and its noise survives a 24- or 60-frame history.
  On the old metrics it read as card: the added model's empty box had 0.21 (full) and 0.28
  (light) of its steps over 6 levels, and the mask card's bounding box stretched along the
  shadow to 0.085 (light). The added model now counts steps over 48 levels (the checker's are
  far larger, the noise's 0.002 at most), the mask card takes its box from the changed pixels
  that survive a 3x3 erosion and counts every changed pixel inside it. The furnace and
  anti-aliasing goldens did not move (0.9823 and 312 of 312 in both variants).
  Since 2026-11-18 the sky is Poly Haven's CC0 `kloofendal_43d_clear_puresky`, far brighter than
  DOOM2016 (hemisphere means of 159-222 and 93-124 displayed levels): all 91 GPU tests passed
  unchanged on aarch64 llvmpipe, the added model's box at 0.199 of its steps after the add (0.0008
  before), the traced mask card at 0.350 of its box, the furnace at 0.9823.
  Since plan I11 a bounce reads the IBL's sharpest level times the Ambient slider, not the cube map
  times 0.35: the GPU tests kept their bounds but the leaning-normal card's, raised to 0.04 (it reads 0.124).
- **A missing skybox is the analytic sky since 2026-11-18**: `WithoutSkyboxFacesTheSkyIsTheAnalyticGradient`
  points `KATAGLYPHIS_SKYBOX_DIR` at no folder and checks every sky pixel (the navy of the switched-off
  sky marks them) against the displayed zenith, horizon and ground of OxidANT's gradient. On aarch64
  llvmpipe: 254288 sky pixels, 0 dark, 0 outside the anchors' 92..210, 133..215, 128..221, blue minus
  red 13.5. Negative control: the old black faces fail all three assertions (254288 dark).
- **The furnace rig moved to roughness 0.8 on 2026-11-16 and back to 0.5 on 2026-11-21.** With
  the sampled Smith remap (`docs/source/path-tracing.md`) the single-scatter BRDF's own `E(mu)`
  passed 1 at grazing angles at 0.5 and the rig's mean rose to 1.019; at 0.8 `E(mu)` stayed in
  0.981-0.997, which capped the mean below 1 (0.9823, bounds 0.977-0.988). Since `brdf.slang`'s
  multiple-scattering gain (plan N1) a white surface returns 1 at every roughness and angle, so the
  rig is back at 0.5 (owner decision 2026-11-21). Its bounds sit halfway between the measured 0.9997 (both
  variants) and a furnace albedo of 0.99 (0.9889) or 1.01 (1.0106): 0.994-1.005, so a 1 % gain or loss
  per bounce fails either way. Measured on one stream only; the four extra streams are not re-run.
- **The anti-aliasing golden reads one edge: the slab's top against the sky** (owner decision
  2026-10-12), at y 56 of the 1200x768 frame. `sky_silhouette` (`GoldenMetrics.hpp`) walks each
  column of the panel-free crop down from the sky to its first step of more than 20 levels, so
  the measurement follows the edge if the framing moves. The sky is analytic and the slab's front
  face is seen head-on, so without jitter the edge pixel is slab: per column it sits 0-3 levels
  inside the 42-44-level step, and 5-11 with jitter (2026-10, a white sun of radiance 10). Since
  2026-11-23 the test lights the rig with the warm default Sun at radiance 10, where the step is
  37.9-41.1 levels; at the default 5 only 61 of 312 columns cleared 20. More than 4 counts as blended, and the test
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

## Shared Slang kernels against one vector file

`Integration.SharedSlangKernelsMeetTheirVectors` (`sharedShaderVectorsSuite.cpp`, plan X3, 2026-11-21) is
the C++ half of OxidANT's vector kernels (OxidANT `docs/shaders.md` § Vector kernels). For every
`third_party/OxidANT/shaders/tests/vectors/*.json` it loads its kernel's SPIR-V from
`Resources/ShadersSlang/build/spirv/tests/`, dispatches it once per case on a compute device of its own
(no surface, no validation layer), and checks every component against its f64 reference within
`abs + rel * |ref|`; an integer must match, and a word past the last case must stay unwritten. One
failure per module lists each miss by case, output and component. A new vector file needs no edit
here; fewer than three files fail the test (an unstaged OxidANT tree). It dispatches one invocation per
case, so a kernel whose SPIR-V `LocalSize` is not 1x1x1 fails before its dispatch; a fence timeout
waits for the device before anything is destroyed, and a short or unmapped read-back fails. On aarch64
llvmpipe (2026-11-21): brdf 21 cases, noise 16, path_estimator 20, the closest passing component at
0.071, 0.075 and 0.024 of its tolerance. Controls: `brdf.slang`'s `multiscatter_gain` returning 1 fails 244
brdf and 52 path_estimator components, its diffuse back on `1 - F` 129 and 41, the counts OxidANT's
model predicts; one edited noise reference fails exactly `inside a cell: snoise[0]`; a brdf kernel
compiled with `numthreads(2, 1, 1)` fails as `declares 2x1x1` before its dispatch. Without a device
it fails under `KATAGLYPHIS_REQUIRE_GPU` and skips otherwise.

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

<!-- golden-counts: defined=104 runnable=102 integration=15 total=117 excluded=3 -->

This makes what the container can only compile-check (device creation, image barriers, the
deferred/forward command streams, path/ray tracing) verifiable.

Shader-only units are cheap: edit a `.slang` source, run `Build-SlangShaders.ps1` (Windows) /
`compile-slang-shaders.sh` (Linux) to refresh the compiled SPIR-V, then run one golden; no C++
rebuild. The `BuildIntegrity` tests check each `.spv` is not older than its `.slang` source. The
output tree (`Resources/ShadersSlang/build/`) is **gitignored, not committed**: a fresh clone must
run the compile script once before the engine has anything to load
([`shader-build-pipeline.md`](shader-build-pipeline.md)).

## Writing a new golden test — cautions learned the hard way

A new `GoldenRender` or `Integration` test raises the `golden-counts` marker above in the same
change: `BuildIntegrity.GoldenTestCountsInDocsMatchTheSuite` fails until it matches the suite. It
goes into the file of its subject; a helper a second file needs moves into `GoldenRenderHarness.hpp`.

The single home for these instrument cautions; `docs/source/cpp-renderer-improvements.md`,
`docs/source/path-tracing.md` and the suite's one-line comments link here. The expensive mistakes:

- **A validation counter must start before the frames it judges.** The layer reports one
  message id ten times, then goes quiet (its `duplicate_message_limit`), and every sync hazard
  shares one id. A `ScopedValidationErrorCounter` made after the warmup frames counted nothing
  while the log held hazards. Make it after the `EngineHarness`, though: on the arm64 runner the
  loader logs a PowerVR ICD's enumeration failure as an error while the harness creates the
  instance.
- **Captures are tonemapped**, and **the harness hides the editor** (`GUI::setOverlayVisible(false)`,
  2026-11-14): its docks would cover the frame's edges and recentre the projection on the rect
  between them. Until then an opaque ImGui panel covered the left ~70% of the 1200x768 frame, so
  the existing tests crop to the right/lower scene region (x >= 0.72w) they were calibrated on. A
  pixel classifier written against raw scene colours (e.g. `r < 60 && b < 60`) can silently
  measure the wrong thing, and whole-frame or centre-crop means have measured, at various times:
  the FPS-counter digits, SSAO, the caster's own body, and nothing at all. Prefer luminance-delta
  oracles over absolute-colour thresholds.
- **The harness pins FXAA off** (`GUIRendererSharedVars::fxaaMode`, 2026-11-21): the goldens
  grade pixels FXAA would move, edges above all. `FxaaSmoothsTheRasterStaircaseAndLeavesFlatRegionsAlone`
  turns it on: a white card turned 20 degrees over the navy sky must blend more than 30 % of the
  unfiltered frame's edge pixels between card and sky (fewer than 1 % without FXAA, the control;
  2452 of 3868 measured with it, 0 without), move no pixel farther than two pixels from every edge
  (0 of 895412), filter a raster frame the same in Raster and Always, and leave a ray-traced frame
  alone in Raster. The integration suites keep the default, Always.
- **The harness pins VSync off** (`GUIRendererSharedVars::vsync`, 2026-11-22), so frames stay
  unpaced by the display, as before VSync defaulted on. `VsyncTogglesBetweenFifoAndAnUncappedMode`
  turns it on (Fifo) and off again (Mailbox, else Immediate where the surface offers one) and
  captures a frame after each rebuild. The integration suites keep the default, on.
- **A GPU timing covers the scene since its geometry arrived** (2026-11-22): the averages and the
  `KATAGLYPHIS_GPU_TIMING_JSON` export restart at a load, a reload or an `addModel`. Before, the
  about 240 sky-only frames a Debug run draws while Dinosaurs parses outweighed its six frames of
  the scene, so Forward read 8-15 ms on llvmpipe against about 540 ms per frame of the scene. On
  llvmpipe a pass's first frame also compiles its pipeline (the deferred G-buffer 1.2 s, its
  lighting 1.4 s, once), so a short average right after a mode switch overstates it. And on
  llvmpipe a Debug (ASan) timing is mostly ASan: its interceptors slow llvmpipe's vertex path 7-9
  times (Forward 76 ms in `linux-profile-clang`, 557 ms with the ASan runtime preloaded into it).
- **For a radiance bound, read the HDR target.** `EngineHarness::capture_hdr_frame` arms
  `VulkanRenderer::requestHdrCapture`, which copies the RGBA16F target post just read, as linear
  floats. ACES squeezes everything above about 1 into the last few 8-bit levels, so a ratio or an
  energy bound measured on the tonemapped frame is weak or vacuous there:
  `PathTracingPassesTheWhiteMetalFurnaceTest` and `PathTracingPassesTheWhiteFurnaceTest` compare a
  white card's radiance to 1 within 0.01 and the rig's mean to 0.994-1.005,
  `PathTracingFloorsAZeroRoughnessTexel` two cards within 3 %,
  `PathTracingHonorsTheUntexturedMetallicFactor` a metal to its dielectric twin, and
  `HdrTargetStaysBelowTheHalfFloatMaximum` every texel to the fp16-safe 65000. That last one is
  the only way to see the overflow on lavapipe, which rounds radiance past 65504 to 65504 rather
  than +Inf, so its 8-bit frame looks right either way.
- **Drive an editor flow with input, not the shared vars**: a write to `GuiSceneSharedVars` skips
  the click, focus and frame order the flow depends on. `ImGuiInputDriver`
  (`Test/commit/VulkanEngine/ImGuiInputDriver.hpp`) sends cursor, button and key events through
  the callbacks the window installed, one transition per frame as ImGui takes them, and finds a
  control by moving the pointer along a line until ImGui's hit test reports its ID (rebuilt from
  its ID-stack scope); `editorInputSuite.cpp` uses it. A line that never hovers the ID fails the
  test, so a moved scope cannot pass by pressing nothing.
- **Prefer counting to averaging for sparse signals.** Changed-pixel / swung-pixel / detail
  fractions discriminate where means drown (a UNORM ceiling clamps, a texture is near-greyscale,
  a skeleton is 3% of the frame).
- **A count says how much changed; only the shape says whether what changed is the effect.**
  Always capture an unconditional control (the effect disabled) and a same-state noise reference
  in the same run, and compare against them. Dump amplified diff-map PNGs and *look* at them
  before trusting any new pixel metric: `GoldenRender.DISABLED_DumpsFrameToPng` dumps the frame,
  the control, and an amplified difference to PNG
  (`--gtest_also_run_disabled_tests --gtest_filter=*DumpsFrameToPng*`,
  `KATAGLYPHIS_FRAME_DUMP=out`; `KATAGLYPHIS_FRAME_DUMP_WITH_UI=1` shows the editor in it).

Effects with no runtime toggle (the tonemap is always on) are hard to isolate this way:
"compressed vs clipped" is ambiguous without a control, so a robust oracle must key on a property
the effect uniquely produces (e.g. retained variation), not just "brighter" or "darker". Post has
an exact one: arm both captures for the same frame and every opaque texel's display value is
`post_display_level` (`GoldenMetrics.hpp`) of the HDR texel post read (below).

## Shadow and post oracles

`ShadowsDarkenSomePixels` and `ShadowsMoveWhenTheLightRotates` say a shadow exists and moves.
Three goldens (2026-11-16) say where it is and what post does with a texel:

- **`ShadowFootprintMatchesTheAnalyticCaster`** casts a ray per pixel through the renderer's own
  camera matrices onto the rig's ground, and from the hit towards the light against the slab's
  box (`predict_ground_shadow`). A pixel counts as shadowed when intensity 1 darkens it by more
  than 4 levels against intensity 0; `compare_shadow_masks` compares only pixels 6 px or more
  from a predicted edge or a non-ground pixel. Five cases: the default light, a light tilted
  towards the camera (the shadow lands on the slab's other side and its centroid must move as the
  geometry says), the camera at z 10 and 44 (the predicted shadow lies 81 % in cascade 0 and
  100 % in cascade 2, by `computeCascadeData`'s splits), and the model moved and turned 90
  degrees through the editor's transform. Each case asks for agreement above 0.97 and a centroid
  within 8 px. Measured on aarch64 llvmpipe: 1.0 and 0 px in every case.
- **`WidestPcfLeavesLitGroundLit`**: at `MAX_PCF_RADIUS`, predicted-lit ground 16 px or more from
  any shadow or non-ground pixel must not darken by 3+ levels; below 1 % passes, 0 of 375144
  measured.
- **`PostEncodesEachHdrTexelThroughAcesAndSrgb`**: at a tenth of the default sun and at the
  default, every HDR texel must be opaque (the sky is in the HDR target since 2026-11-21) and
  more than 99.9 % of them must sit within 2 levels of ACES then sRGB of their HDR value, green
  texels within half a stop of 0.18 must average within 2 levels of the formula's mean over the
  same texels (level 124 over 660990 of them on llvmpipe), and the two frames must hold at least
  1000 green texels below 0.05, within that half stop and above 1.0, so the toe, middle and
  shoulder are all checked. The band was ±0.004 until Phase 19, whose dimmer bake moved the
  sky-lit ground out of it (99845 texels to 418).
  The C++ post has no exposure or bloom stage to test; those are the Rust renderer's.
- **`TheSkyLightsShadowedGroundInEveryRasterAndRayTracedFrame`** (2026-11-23): the shadow rig with the
  sun's shadow at full strength, in forward, deferred and ray-traced frames, at Ambient 0, the default
  0.35 and the slider's top, 2. Over the predicted shadow's core (`predict_ground_shadow`, every
  pixel's 13 x 13 window shadowed, 54690 px) the linear HDR radiance must be none at 0, blue
  (b over twice r, the zenith's irradiance on an upward face) at the default, 2 / 0.35 times the
  default's in every channel within 2 % at 2, and the same in all three modes within 5 %.
  Measured on aarch64 llvmpipe: (0.0223, 0.0397, 0.0869) at the default, ratio 5.713, deferred
  within 0.01 % of forward and the ray tracer equal to it. With the term dropped from
  `deferred.slang` its shadow is black and the test fails. `ForwardLightingRespondsToTheDirectionalLight`
  and `RaytracedShadowsAreDarkerThanLitGround` set the Ambient to 0 since: they isolate the sun. The
  first also switches the skybox off and reads its radiance-0 crop over geometry only (the navy marks
  the sky): the cube-map sky alone had put the crop at 29.8 against its bound of 30. Geometry reads 0
  over 161257 px (204.6 lit); with forward's sun lobe blind to the radiance, as b513d010's diffuse was,
  it stays at 115.3, its lit value, and fails.
- **`AMirrorInTheSunsShadowReflectsTheSkyAndNoSun`** (2026-11-23): a smooth white metal card
  (roughness 0.05) under the rig's slab, turned so the eye sees the sun mirrored in its centre, in
  forward, deferred and ray-traced frames. At Ambient 0 the card must be black (the slab shadows it,
  so the test measures something); per unit of Ambient its centre may gain no more than the
  brightest sky radiance, the horizon's 0.72, and at least 0.2 in blue. Measured on aarch64
  llvmpipe at Ambient 2: (0.277, 0.403, 0.722), a gain of (0.139, 0.202, 0.361), deferred within
  0.5 %. With OxidANT's sun disk back in `analytic_ambient` the card reads (87.7, 83.5, 75.1) at Ambient 2 and fails in every mode.
- **`AMirrorFacingThePhotoSunReflectsTheSkyAroundIt`** (2026-11-26): the default photo sky, the Sun's radiance 0
  and Ambient 1. Looking at the photo's sun (0.6248, 0.6769, 0.3891), the sky pass still draws its saturated disk
  (7.24 in every channel within 2 degrees) and the ring at 11-14 degrees around it; then a smooth white metal card
  (roughness 0.05), turned so the eye sees that sun mirrored in its centre, must reflect 0.3 to 2 times the ring's
  luminance in forward, deferred, ray-traced and path-traced frames (1 spp, two bounces, 4 frames). Measured on
  aarch64 llvmpipe: ring (0.579, 1.205, 4.120), luminance 1.282; the mirror 0.908 of it in forward and ray tracing,
  0.903 deferred, 0.923 path traced. With the engine's second environment pass keeping the sun (`AmbientMaps.cpp`'s
  `leaveOutSun` 0) it reads 4.60, 4.57, 4.60 and 5.07 and fails in every mode.
- **`SkyFillsTheHdrTargetInEveryMode`**: on the shadow rig with the navy sky, the forward,
  deferred, ray-traced and path-traced HDR targets hold no transparent texel and more than 5000
  texels at the navy's untonemapped radiance, which post shows as the navy; the path tracer's
  camera miss draws it through `drawn_sky.slang`, as the sky pass does.

The controls, one-line edits each run on the three and reverted (`ShadowsDarkenSomePixels`, run
beside them, failed only the first):

| Edit | Footprint | PCF | Post |
| --- | --- | --- | --- |
| Every fragment reads cascade 0 | fails: far camera 0 of 12280 shadowed px, centroids off by 16-611 px | passes | passes |
| Every tap compares at the centre depth (the kernel before the receiver plane) | fails: agreement 0.51-0.97 | fails: 100 % darkened | passes |
| The shadow pass ignores the model matrix | fails: moved model agreement 0.48, 254 px off | passes | passes |
| The cascades fit a light mirrored in z | fails: agreement 0.66-0.99, shift 1374 against 7435 | fails: 14.3 % | passes |
| Post skips the sRGB encode | passes | passes | fails: 2.1 % match, 0.18 at 67 |
| Post clamps instead of ACES | passes | passes | fails: 2.1-2.6 % match, 0.18 at 117 |

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
each radius masks every pixel that moves without shadows (the overlay's slider text and FPS digits
while the harness still showed it). The rest of the frame splits in two:

- **near**: within 16 px of the radius-0 shadow, where penumbra softening shows;
- **far-lit**: everything else, lit at radius 0, which PCF should leave alone.

Measured 2026-11-16 in `:latest` on aarch64 llvmpipe, Debug build, 1200x768 with the editor
hidden; two runs agreed exactly on the pixel columns. Each cell: the receiver-plane kernel
(OxidANT's `common/shadow_pcf.slang`, since the 2026-11-16 pin), then in brackets the kernel
before it, which compared every tap against the centre texel's depth:

| Radius | Taps | Near: pixels a +1 step moves >= 3 levels | Near: mean step change (levels) | Far-lit: mean darkening (levels) | Far-lit: darkened >= 3 levels | Main pass (ms) |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | 1 | - | - | 0.00 (0.00) | 0 % (0 %) | 7.8-8.3 |
| 1 | 9 | 0.5 % (39.8 %) | 0.08 (3.15) | 0.00 (0.05) | 0 % (0 %) | 14.9-16.4 |
| 2 | 25 | 0.4 % (50.0 %) | 0.07 (3.45) | 0.00 (1.46) | 0 % (27.2 %) | 23.7-25.9 |
| 3 | 49 | 0.8 % (44.0 %) | 0.41 (2.27) | 0.00 (2.92) | 0 % (62.4 %) | 36.3-39.0 |
| 4 | 81 | 1.0 % (31.8 %) | 0.65 (1.83) | 0.00 (3.93) | 0 % (62.4 %) | 55.5-57.9 |
| 5 | 121 | 1.1 % (9.5 %) | 0.45 (1.41) | 0.00 (4.65) | 0 % (62.4 %) | 80.9-90.6 |

The Main pass costs the same with either kernel (the slope is computed once per pixel); the
old kernel's times on this host are within the same ranges.

**Why 5.** The bound was set on 2026-10-06 with the old kernel, measured to radius 20 on a
32-core x64 host: a step counted as visible when it moved at least 2 % of the near pixels by 3
or more luminance levels (of 255, about 1.2 %), and radius 5 was the last step over it, while
the Main pass grew from 15.6 ms at 5 to 181 ms at 20. Most of what that criterion saw was the
artifact: on a receiver sloped against the light the outer taps read its own nearer depth as an
occluder, so lit surfaces far from any shadow darkened from radius 2 on (27 % of them by 3+
levels at the default 2 on this host, 62 % from 3). With each tap compared at the receiver
plane's depth beneath it, no lit pixel darkens at any radius, and no step moves more than
1.1 % of the near pixels: on the rig, where the 2048-texel map is finer than the screen, the
penumbra a larger radius adds is a few pixels wide. The criterion no longer separates the
radii, so the bound now rests on cost alone (the Main pass grows about 11x from radius 0 to 5
here); it stays 5, which OxidANT's `forward_lighting.slang` mirrors.

## Synchronization validation

Vulkan's `khronos_validation.validate_sync` layer setting catches a bug class no pixel oracle
sees: a missing or incorrect barrier between two GPU commands that read or write the same
resource (WRITE-AFTER-WRITE, READ-AFTER-WRITE, WRITE-AFTER-READ). It found 10 real
WRITE-AFTER-WRITE hazards in July 2026.

**Linux CI runs it on every push since 2026-10-06.** `run-ctest.sh --virtual-display` sets
`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1` for the GPU suites and passes the hub's
`--fail-on-log-pattern 'SYNC-HAZARD|VUID-'` (CON291), which fails the run when a test's output in
ctest's log holds either, naming each test. Most tests ignore validation messages, so the log is
the only place a hazard or a validation error shows. On llvmpipe it costs about 2%: the 42 GPU
tests took 921 s with it and 900 s without, on 32 cores. Its first run found two
WRITE-AFTER-WRITE classes, both fixed the same day:

- **The deferred render pass** (`DeferredRasterizer.cpp`). The geometry-to-lighting dependency
  left the G-buffers' store ops, and depth's load-op clear, outside its scopes. The
  lighting-to-external one left depth's store before the final layout transition. Since
  2026-11-16 the two halves are separate dynamic-rendering passes with sync2 barriers between
  them (`cpp-renderer-improvements.md`).
- **Back-to-back BLAS builds** (`ASManager.cpp`). One scratch buffer serves every model, and the
  barrier between builds only made the next one *read*.

`GoldenRender.DeferredFramesAndBlasRebuildsAreFreeOfSyncHazards` pins both: it turns sync
validation on for its own harness, so it holds on any lane with validation layers.

**`validate_sync` alone misses two classes**, both off by default in the 1.4.363 layer
(measured 2026-11-16, with the barriers removed: 0 hazards by default):

- `syncval_load_op_after_store_op_validation`: a load op reading what another rendering
  instance stored. Until 2026-11-21 post's `eLoad` of the swapchain image after the sky pass
  was one; without its barrier the setting reported `READ_AFTER_WRITE` at
  `vkCmdBeginRendering`. Post now overwrites the swapchain image (`eDontCare`), and the
  traced sky's `eLoad` of the ray-traced target follows a storage write.
- `syncval_shader_accesses_heuristic`: shader reads and writes through descriptors. Post's
  read of the clouds' compute output is one; without its barrier the setting reports
  `READ_AFTER_WRITE` at post's `vkCmdDraw`.

`run-ctest.sh --virtual-display` and `Invoke-LavapipeTests.ps1` turn both on, and
`GoldenRender.CloudCompositeAndTracedSkyAreFreeOfSyncHazards` sets them for its own
harness. Raise `VK_KHRONOS_VALIDATION_DUPLICATE_MESSAGE_LIMIT` when hunting: every hazard of one
kind shares a message id, so ten load-op reports silence the shader-read ones behind them.

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
`SYNC-HAZARD` or `VUID-`.

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
