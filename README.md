<div align="center">
  <a href="https://jonasheinle.de">
    <img src="images/logo.png" alt="logo" width="200" />
  </a>

  <h1>BeschleunigerBallett</h1>

  <h4>Experimental graphics engine and renderer playground for Vulkan, a Rust WebGPU sibling renderer, modern CMake, testing, packaging, and optional Rust integration.</h4>
</div>

<div align="center"> 
  <br> 
  <a href="https://jonasheinle.de"><img src="images/vulkan-logo.png" alt="VulkanEngine" width="200"></a>
  <a href="https://jonasheinle.de"><img src="images/Engine_logo.png" alt="VulkanEngine" width="200"></a>
  <a href="https://jonasheinle.de"><img src="images/glm_logo.png" alt="VulkanEngine" width="200"></a>
</div>

see also [**__Official homepage__**](https://beschleunigerballette.jonasheinle.de/). 

[![Linux build + test + coverage on Ubuntu 26.04 ARM](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/Linux_arm.yml/badge.svg?branch=develop)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/Linux_arm.yml)
[![Linux build + test + coverage on Ubuntu 26.04 x86](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/Linux_x86.yml/badge.svg?branch=develop)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/Linux_x86.yml)
[![Windows Server 2025 build x86 MSVC and Clang](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/Windows.yml/badge.svg?branch=develop)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/Windows.yml)  
[![CodeQL](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/github-code-scanning/codeql/badge.svg)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/github-code-scanning/codeql)
[![Automatic Dependency Submission](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/dependency-graph/auto-submission/badge.svg)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/dependency-graph/auto-submission)
[![TopLang](https://img.shields.io/github/languages/top/Kataglyphis/BeschleunigerBallett)]()  
[![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg)](https://www.paypal.com/donate/?hosted_button_id=BX9AVVES2P9LN)
[![Twitter](https://img.shields.io/twitter/follow/Cataglyphis_?style=social)](https://twitter.com/Cataglyphis_)
[![YouTube](https://img.shields.io/youtube/channel/subscribers/UC3LZiH4sZzzaVBCUV8knYeg?style=social)](https://www.youtube.com/channel/UC3LZiH4sZzzaVBCUV8knYeg)

## Overview

BeschleunigerBallett is a renderer and graphics-engine playground used to explore modern graphics APIs and the surrounding engineering workflow. The repository combines a C++23-modules Vulkan engine, a companion Rust WebGPU renderer (native + browser), build automation, packaging, testing, documentation, and Rust integration.

## Highlights

- Vulkan renderer (C++23 modules) with forward + deferred rasterization, ray tracing, path tracing, PBR, cascaded shadow maps (consumed by both lighting paths), skybox, volumetric clouds, OBJ loading, and mip mapping
- VMA-backed memory, fence-synced uploads with a persistent staging buffer, a persisted `VkPipelineCache`, and fail-fast Vulkan error handling (exceptions are disabled project-wide)
- Companion Rust WebGPU renderer (`third_party/OxidANT/crates/webgpu_renderer`): glTF 2.0/GLB, PBR + IBL, CSM, SSAO, bloom, skinning, animations, LOD — runs natively and in the browser; shares Slang shader sources with the C++ Vulkan renderer (see `docs/shader-sharing.md`)
- Tooling around CMake presets, CI, code coverage, benchmarking, fuzzing (including a real OBJ-parsing fuzz target), packaging, Sphinx, Doxygen, and Graphviz
- Linux and Windows as the primary development targets

## Repository Layout

| Path | Purpose |
| --- | --- |
| `Src/` | Engine and renderer source code |
| `Resources/` | Shaders and runtime assets |
| `scripts/linux/` | Linux build, test, coverage, analysis, and docs helpers |
| `scripts/windows/` | Windows build, run, and dependency setup helpers |
| `Test/` | Tests |
| `docs/source/` | Hand-written Sphinx pages |
| `Documents/` | Generated PDF and reference artifacts |
| `third_party/` | Third-party dependencies and submodules |

## Quick Start

### Clone

```bash
git clone --branch develop --recurse-submodules git@github.com:Kataglyphis/BeschleunigerBallett.git
cd BeschleunigerBallett
```

### Configure and build with CMake presets

```bash
cmake --list-presets
cmake --preset <preset-name>
cmake --build build --config Debug
ctest --test-dir build --output-on-failure
```

A host CMake older than 4.1 cannot read `CMakePresets.json` (`"version": 10`)
and fails `cmake --list-presets` with `Unrecognized "version" field` — read
the file itself, or the Windows configurations table in
[AGENTS.md](AGENTS.md#windows-configurations-build-windowsps1), instead. On
Windows, prefer `scripts/windows/Build-Windows-Container.ps1`, which builds
inside a container that already has a new enough CMake.

Full prerequisites, the per-platform workflows (Linux helper scripts, the Windows `Build-Windows.ps1` configurations, the containerized Stevedore build), packaging (CPack/MSIX), and troubleshooting live in [docs/source/getting_started.md](docs/source/getting_started.md).

## Documentation

The repository ships two documentation entry points:

- this README for repository-level orientation
- the Sphinx site under `docs/` for getting started, workflow notes, Graphviz output, and optional API reference material

Each topic has exactly one home; the full topic-guide inventory is the Docs table in [AGENTS.md](AGENTS.md). Renderer-agnostic and Windows-container knowledge lives in the ANTfrastructure submodule so other projects can consume it (see the "Reusable Work Belongs in ANTfrastructure" rule there).

To build the Sphinx HTML docs locally, see
[docs/source/documentation_workflow.md](docs/source/documentation_workflow.md).

If Doxygen XML is available, the Sphinx build automatically includes the generated C++ API reference. If no XML is present, the hand-written docs still build cleanly and the API section stays hidden.

## Tests and Analysis

- `ctest` runs the configured test suite from the active build directory
- `scripts/linux/run-ctest.sh` wraps Linux test execution
- `scripts/linux/build-coverage-gcovr.sh` and `scripts/linux/build-coverage-llvm.sh` generate coverage reports
- `scripts/linux/run-perf-suite.sh` runs performance-oriented checks
- `scripts/windows/Build-Windows.ps1` can orchestrate formatting, tidy, builds, tests, and packaging

## Dependency Updates

Submodule bumps go through `scripts/linux/renovate-local.sh`, not by hand. Run
it from WSL — there is no node on the Windows host. It runs Renovate as a local
CLI to report what is behind; `--apply` is then plain git, moving only the
gitlinks whose submodule declares a branch in `.gitmodules` and naming the rest
for a deliberate manual bump. That half needs the git that wrote this working
tree, so from WSL the script switches to `git.exe` and refuses up front when it
cannot reach one — a Linux git over a Windows checkout would abort half-applied.
Other managers are report-only. See
[`third_party/ANTfrastructure/docs/dependency-updates.md`](third_party/ANTfrastructure/docs/dependency-updates.md).

## Packaging

Linux binary packages are generated with CPack (optionally as AppImage); the Windows release build can produce a signed MSIX. The workflows live in [docs/source/getting_started.md](docs/source/getting_started.md#packaging).

## Shaders

Shaders are written in [Slang](https://shader-slang.com/) under
`Resources/ShadersSlang/` and compiled ahead of time by
`scripts/windows/Build-SlangShaders.ps1` / `scripts/linux/compile-slang-shaders.sh`
to SPIR-V and WGSL. See `docs/shader-build-pipeline.md` and AGENTS.md §
Shaders for the full pipeline.

## Docker and Build Environments

Containerized and reproducible environment details live in [ANTfrastructure](https://github.com/Kataglyphis/ANTfrastructure). On Windows the container runtime is [Stevedore](https://github.com/slonopotamus/stevedore); `scripts/windows/Build-Windows-Container.ps1` builds this project inside the prebuilt toolchain image (sources travel via a tar-pipe into a reusable container by default, `-UseBindMount` opts into a bind mount — see [`docs/container-build-caching.md`](docs/container-build-caching.md)), and `.github/workflows/Windows.yml` runs the same flow in CI.

## Roadmap

- Keep the renderer and tooling foundations healthy on Linux and Windows
- Expand test, fuzz, and performance coverage
- Continue documenting build, packaging, and API workflows
- Keep generated reference material easy to reproduce locally

All open work is consolidated in [`BACKLOG.md`](BACKLOG.md) — sized
commitments first, then unsized ideas and recurring chores (perf tests,
periodic profile/TSan/sync-validation runs).
Renderer-specific plans and status live in `docs/`:
`webgpu-renderer-roadmap.md` (Rust WebGPU renderer),
`cpp-renderer-improvements.md` (C++ engine improvement campaign),
`shader-sharing.md` (sharing shader code between both renderers),
`webgpu-srgb-audit.md` (color-space audit), and
`webgpu-gltf-rust-plan.md` (glTF loading plan for the Rust renderer).

## Contributing

Contributions are welcome. A good change keeps code, docs, and build scripts aligned.

1. Fork the project
2. Create a feature branch
3. Implement and test the change
4. Update documentation when behavior or workflows change
5. Open a pull request

## License

Distributed under the MIT License. See `LICENSE` for more information.

## Third-party Licenses

See the full third-party license overview in [docs/LICENSES-README.md](docs/LICENSES-README.md).

## Contact

Jonas Heinle - [@Cataglyphis_](https://twitter.com/Cataglyphis_) - renderdude@jotrockenmitlocken.de

Project link: [https://github.com/Kataglyphis/BeschleunigerBallett](https://github.com/Kataglyphis/BeschleunigerBallett)

## Further Reading

- [Official homepage](https://beschleunigerballette.jonasheinle.de/)
- [ANTfrastructure](https://github.com/Kataglyphis/ANTfrastructure)
- [Doxygen PDF reference](Documents/refman.pdf)
- [Sphinx docs source](docs/source)

## Literature 

Some very helpful literature, tutorials, etc. 

* [View Frustum Culling](http://www.lighthouse3d.com/tutorials/view-frustum-culling/geometric-approach-extracting-the-planes/)

OpenGL 
* [learnopengl.com](https://learnopengl.com/)
* [ogldev.org](https://ogldev.org/)
* [Cascaded Shadow Maps](https://ahbejarano.gitbook.io/lwjglgamedev/chapter26)
* [Compute Shader in OpenGL](https://antongerdelan.net/opengl/compute.html)

Clouds
* [pbr-book](https://www.pbr-book.org/)
* [Inigo Quilez](https://iquilezles.org)
* [Shadertoy Horizon Zero Dawn](https://www.shadertoy.com/view/WddSDr)
* [Sebastian Lague](https://m.youtube.com/watch?v=4QOcCGI6xOU&t=97s)
* [Horizon Zero Dawn](http://advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf)
* [Clouds and noise](https://thebookofshaders.com/12/)
* [Shadertoy Clouds using 3D Perlin noise](https://www.shadertoy.com/view/XlKyRw)

Noise
* [Worley noise online demo](https://github.com/Erkaman/glsl-worley)

Vulkan
* [Udemy course by Ben Cook](https://www.udemy.com/share/102M903@JMHgpMsdMW336k2s5Ftz9FMx769wYAEQ7p6GMAPBsFuVUbWRgq7k2uY6qBCG6UWNPQ==/)
* [Vulkan Tutorial](https://vulkan-tutorial.com/)
* [Vulkan Raytracing Tutorial](https://developer.nvidia.com/rtx/raytracing/vkray)
* [Vulkan Tutorial; especially chapter about integrating imgui](https://frguthmann.github.io/posts/vulkan_imgui/)
* [NVidia Raytracing tutorial with Vulkan](https://nvpro-samples.github.io/vk_raytracing_tutorial_KHR/)
* [Blog from Sascha Willems](https://www.saschawillems.de/)

Physically Based Shading
* [Advanced Global Illumination by Dutre, Bala, Bekaert](https://www.oreilly.com/library/view/advanced-global-illumination/9781439864951/)
* [The Bible: PBR book](https://pbr-book.org/3ed-2018/Reflection_Models/Microfacet_Models)
* [Real shading in Unreal engine 4](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf)
* [Physically Based Shading at Disney](https://blog.selfshadow.com/publications/s2012-shading-course/burley/s2012_pbs_disney_brdf_notes_v3.pdf)
* [RealTimeRendering](https://www.realtimerendering.com/)
* [Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs](https://hal.inria.fr/hal-01024289/)
* [Sampling the GGX Distribution of Visible Normals](https://pdfs.semanticscholar.org/63bc/928467d760605cdbf77a25bb7c3ad957e40e.pdf)

Path tracing
* [NVIDIA Path tracing Tutorial](https://github.com/nvpro-samples/vk_mini_path_tracer/blob/main/vk_mini_path_tracer/main.cpp)

Docker
* [Vulkan Minimal Docker setup](https://github.com/j3soon/docker-vulkan-runtime)
* [scoop](https://scoop.sh/#/apps)
* [Docker container windows GPU](https://learn.microsoft.com/de-de/virtualization/windowscontainers/deploy-containers/gpu-acceleration)
* [Docker windows](https://hub.docker.com/r/microsoft/windows)

## Common issues

### WSL2 — Vulkan (NVIDIA)

Wenn in WSL2 nach dem Start der Anwendung nur "llvmpipe" (Mesa Software‑Renderer) angezeigt wird, siehe die Troubleshooting‑Seite: [docs/source/wsl2_vulkan.rst](docs/source/wsl2_vulkan.rst) für Schritt‑für‑Schritt‑Anweisungen zur Installation des NVIDIA‑ICD und zum Neustart von WSL (`wsl --shutdown`).

### Missing Vulkan validation layers

If startup fails with `Validation layers requested, but not available!`, see the Troubleshooting section in [docs/source/getting_started.md](docs/source/getting_started.md#troubleshooting) (Linux packages, Windows exit code and `VK_LAYER_PATH`).

