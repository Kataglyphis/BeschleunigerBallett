<div align="center">
  <a href="https://jonasheinle.de">
    <img src="images/logo.svg" alt="BeschleunigerBallett logo: a ballerina ant on the RGB hello triangle" width="200" />
  </a>

  <h1>BeschleunigerBallett</h1>

  <h4>Experimental graphics engine and renderer playground for Vulkan, a Rust WebGPU sibling renderer, modern CMake, testing, packaging, and Rust integration.</h4>
</div>

[![Linux arm64 · build + test](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/linux-arm64.yml/badge.svg?branch=develop)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/linux-arm64.yml)
[![Linux x64 · build + test](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/linux-x64.yml/badge.svg?branch=develop)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/linux-x64.yml)
[![Linux riscv64 · cross build + test](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/linux-riscv64.yml/badge.svg?branch=develop)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/linux-riscv64.yml)
[![Windows x64 · build + test](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/windows-x64.yml/badge.svg?branch=develop)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/windows-x64.yml)
[![Windows arm64 · cross build + test](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/windows-arm64-cross.yml/badge.svg?branch=develop)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/windows-arm64-cross.yml)
[![Lint gates](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/lint-gates.yml/badge.svg?branch=develop)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/lint-gates.yml)
[![Submodule pins](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/submodule-pins.yml/badge.svg?branch=develop)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/submodule-pins.yml)
[![CodeQL](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/github-code-scanning/codeql/badge.svg)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/github-code-scanning/codeql)
[![Automatic Dependency Submission](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/dependency-graph/auto-submission/badge.svg)](https://github.com/Kataglyphis/BeschleunigerBallett/actions/workflows/dependency-graph/auto-submission)
[![TopLang](https://img.shields.io/github/languages/top/Kataglyphis/BeschleunigerBallett)]()
[![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg)](https://www.paypal.com/donate/?hosted_button_id=BX9AVVES2P9LN)
[![Twitter](https://img.shields.io/twitter/follow/Cataglyphis_?style=social)](https://twitter.com/Cataglyphis_)
[![YouTube](https://img.shields.io/youtube/channel/subscribers/UC3LZiH4sZzzaVBCUV8knYeg?style=social)](https://www.youtube.com/channel/UC3LZiH4sZzzaVBCUV8knYeg)

[**Docs site**](https://beschleunigerballette.jonasheinle.de/)

BeschleunigerBallett is a renderer and graphics-engine playground used to explore modern graphics APIs and the surrounding engineering workflow. The repository combines a C++23-modules Vulkan engine, a companion Rust WebGPU renderer (native + browser), build automation, packaging, testing, documentation, and Rust integration.

Dependency updates: run `bash scripts/linux/renovate-local.sh` to report, `--apply --dry-run` to plan,
and `--apply` to update. CI checks the wrapper and config; see
[the shared Renovate guide](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/dependency-updates.md).

## Highlights

- **Vulkan renderer in C++23 modules:** forward and deferred rasterization, ray tracing, path tracing, PBR, cascaded shadow maps, skybox, volumetric clouds.
- **Fail-fast engine plumbing:** VMA memory, fence-synced uploads, a persisted `VkPipelineCache`, exceptions disabled project-wide.
- **A Rust WebGPU sibling renderer** from [OxidANT](https://github.com/Kataglyphis/OxidANT), native and [in the browser](https://beschleunigerballette.jonasheinle.de/webgpu_demo.html), sharing the shader math owned in OxidANT's pinned `shaders/` tree.
- **OBJ and glTF 2.0/GLB scenes** from [AssetANT](https://github.com/Kataglyphis/AssetANT), pinned by SHA-256 in `Resources/assets.lock`.
- **Five CI lanes:** Linux x64, arm64 and riscv64, Windows x64 and arm64; the GPU suites render on llvmpipe and lavapipe.
- **Tooling:** CMake presets, coverage, benchmarks, fuzzing, CPack and MSIX packages, Sphinx and Doxygen docs.

## Quick start

```bash
git clone --recurse-submodules --branch develop https://github.com/Kataglyphis/BeschleunigerBallett.git
cd BeschleunigerBallett
cmake --preset linux-debug-clang && cmake --build build && ctest --test-dir build --output-on-failure
```

On Windows, build in the toolchain container instead (Stevedore):
`pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1`.
Prerequisites, both platforms, the scenes, packaging and troubleshooting:
[docs/source/getting_started.md](docs/source/getting_started.md).

## Documentation

| Topic | Read |
| --- | --- |
| Getting started: prerequisites, Linux, Windows, the container build, packaging, troubleshooting | [docs/source/getting_started.md](docs/source/getting_started.md) |
| The scenes, how they are fetched, and their licences | [docs/source/getting_started.md § Scenes](docs/source/getting_started.md#scenes) |
| Project overview: scope, repository map, roadmap | [docs/source/README.md](docs/source/README.md) |
| Building the docs site, the C++ API reference, Graphviz | [docs/source/documentation_workflow.md](docs/source/documentation_workflow.md) |
| The C++ API reference (Doxygen, on the docs site) | [api/library_root](https://beschleunigerballette.jonasheinle.de/api/library_root.html) |
| The WebGPU demo in the browser | [docs/source/webgpu_demo.md](docs/source/webgpu_demo.md) |
| WSL2 with an NVIDIA GPU | [docs/source/wsl2_vulkan.rst](docs/source/wsl2_vulkan.rst) |
| Shaders: the Slang to SPIR-V and WGSL build | [docs/source/shader-build-pipeline.md](docs/source/shader-build-pipeline.md) |
| One Slang source for both renderers | [docs/source/shader-sharing.md](docs/source/shader-sharing.md) |
| Model loading | [docs/source/model-loading.md](docs/source/model-loading.md) |
| Path tracing | [docs/source/path-tracing.md](docs/source/path-tracing.md) |
| Volumetric clouds | [docs/source/clouds.md](docs/source/clouds.md) |
| GPU golden tests and synchronization validation | [docs/source/gpu-golden-testing.md](docs/source/gpu-golden-testing.md) |
| clang-format, clang-tidy, cmake-format | [docs/source/code-quality.md](docs/source/code-quality.md) |
| CI quality reports, Ruff/ty, PowerShell checks and refactor previews | [Shared quality tooling](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/code-quality-tooling.md) |
| The container build's transports and caches | [docs/source/container-build-caching.md](docs/source/container-build-caching.md) |
| The C++ engine's change log | [docs/source/cpp-renderer-improvements.md](docs/source/cpp-renderer-improvements.md) |
| Colour-space audit | [docs/source/webgpu-srgb-audit.md](docs/source/webgpu-srgb-audit.md) |
| The Rust WebGPU renderer's roadmap (OxidANT) | [webgpu-renderer-roadmap.md](https://github.com/Kataglyphis/OxidANT/blob/develop/crates/webgpu_renderer/docs/webgpu-renderer-roadmap.md) |
| Third-party licences | [docs/source/LICENSES-README.md](docs/source/LICENSES-README.md) |
| Literature: tutorials and papers behind the renderer | [docs/source/literature.md](docs/source/literature.md) |
| Rules for agents and contributors, and every topic's one home | [AGENTS.md](AGENTS.md) |
| What CI runs, and what it does not | [AGENTS.md § What CI runs](AGENTS.md#what-ci-runs-and-what-it-does-not) |
| Build images, CI actions and shared scripts (ANTfrastructure) | [docs/INDEX.md](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/INDEX.md) |
| Known work | [BACKLOG.md](BACKLOG.md) |
| Changes | [CHANGELOG.md](CHANGELOG.md) |

## Contributing and contact

BeschleunigerBallett is one of the [Kataglyphis repositories](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/repository-family.md),
which share one way to contribute and one contact. Licence: MIT, see [LICENSE](LICENSE).
