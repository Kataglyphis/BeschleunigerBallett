# Getting Started

## Prerequisites

- C++23 and C17 capable compilers
- CMake 4.1 or newer
- a Vulkan SDK installation for Vulkan-enabled builds
- Python plus the packages from `requirements.txt` for the docs build (the formatting steps install their own pinned `cmake-format` from ANTfrastructure)
- Rust and cargo: every build links OxidANT's cxx bridge through Corrosion
- network access for the first configure, which downloads the default scenes (see below)

## Clone the Repository

```bash
git clone --branch develop --recurse-submodules https://github.com/Kataglyphis/BeschleunigerBallett.git
cd BeschleunigerBallett
```

## Configure with CMake Presets

Use the presets in `CMakePresets.json`, not ad-hoc build commands:

```bash
cmake --list-presets
cmake --preset <preset-name>
cmake --build build --config Debug
ctest --test-dir build --output-on-failure
```

Visual Studio style generators on Windows need `-C Debug` or `-C Release` on `ctest`.

A host CMake older than 4.1 cannot read `CMakePresets.json` (`"version": 10`)
and fails `cmake --list-presets` with `Unrecognized "version" field`: read
the file itself, or the Windows configurations table in
[AGENTS.md](https://github.com/Kataglyphis/BeschleunigerBallett/blob/develop/AGENTS.md#windows-configurations-build-windowsps1).
On Windows, prefer `scripts/windows/Build-Windows-Container.ps1`, which builds
inside a container that already has a new enough CMake.
The Windows CI callers check out direct submodules; their build sources are all at that level,
so they avoid the nested ANTfrastructure and DocumANTation checkout cycle.

### Scenes

The scenes are not in the repository: `Resources/assets.lock` pins each one as an
[AssetANT](https://github.com/Kataglyphis/AssetANT) release archive by URL and SHA-256, and a
configure extracts the two the code, the tests and the packages read (`crytek-sponza` and
`Dinosaurs`, about 33 MB) into `Resources/Models` through a per-user cache
(`KATAGLYPHIS_ASSET_CACHE`, else `%LOCALAPPDATA%\kataglyphis-assets` or
`~/.cache/kataglyphis-assets`). The fetcher is ANTfrastructure's Python script, so the configure
needs Python 3.12 or newer on `PATH` (`python` on Windows, `python3` elsewhere). The paths the
engine loads did not change.

- `-DKATAGLYPHIS_FETCH_ALL_ASSETS=ON` fetches every locked scene (all fourteen), for the model picker.
- `cmake -P cmake/FetchAssets.cmake` fetches without configuring (add the same `-D` before `-P` for all).
- Offline, put each archive in the cache as `<sha256>-<name>.tar.xz`, or configure with
  `-DKATAGLYPHIS_FETCH_ASSETS=OFF` and go without the scenes and the tests that read them.

Each fetched scene carries its credit in `ATTRIBUTION.txt`; AssetANT's
`assets/<name>/asset.json` records where it came from and how its license was established.

| Scene | Where | License |
| --- | --- | --- |
| `crytek-sponza` | AssetANT; fetched by default and installed with the packages | CC BY 3.0: © 2010 Frank Meinl, Crytek, after Marko Dabrovic's Sponza; corrections by Morgan McGuire |
| `Dinosaurs` | AssetANT; fetched by default | unknown; the Triceratops mesh is named after joel3d.com |
| `Sulo` | AssetANT; `KATAGLYPHIS_FETCH_ALL_ASSETS` | modelled by [Sulo456](https://github.com/Sulo456), published with his permission |
| the other eleven | AssetANT; `KATAGLYPHIS_FETCH_ALL_ASSETS` | per scene, in [AssetANT's README](https://github.com/Kataglyphis/AssetANT#the-assets) |
| `VikingRoom` | tracked, a test fixture | CC BY 4.0: "Viking room" by nigelgoh on Sketchfab, the Vulkan Tutorial's copy with its material renamed |
| `GltfTest`, `ShadowTest`, `Plane`, `plane.obj`, `suzanne.obj` | tracked, test fixtures | — |

## Linux Workflow

The helper in `scripts/linux/` wraps configure and build:

```bash
bash ./scripts/linux/cmake-configure-build.sh \
  --preset linux-debug-clang \
  --build-dir build \
  --build-config Debug
```

Other helpers: `scripts/linux/run-ctest.sh`, `scripts/linux/build-coverage-gcovr.sh`,
`scripts/linux/build-coverage-llvm.sh`, `scripts/linux/run-perf-suite.sh`.

## Windows Workflow

`Build-Windows.ps1` runs configuration, build, formatting, clang-tidy, tests and packaging from one entry point. Configurations: `msvc-debug`, `msvc-release`, `clangcl-debug` (Debug with ASAN/UBSan), `clangcl-profile` (RelWithDebInfo with benchmarks), `clangcl-release`.

```pwsh
# single configuration
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows.ps1 -Configurations clangcl-debug

# full sanitizer/profile/release sweep
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows.ps1 `
  -Configurations "clangcl-debug,clangcl-profile,clangcl-release"
```

- `-TargetArch arm64` cross-builds `clangcl-release` for Windows on Arm inside the container image's arm64 bundle (`:winarm64`) and writes the portable bundle, MSI, ZIP and MSIX to `dist/windows-arm64`; `.github/workflows/windows-arm64-cross.yml` runs that.
- A host build of `clangcl-release` writes the same to `dist/windows-x64`, plus the NSIS installer; `.github/workflows/windows-x64.yml` runs it through `scripts/windows/Invoke-WindowsLane.ps1`, with the CPU tests, the fuzz seeds and the renderer comparisons.
- The Release package target uses ANTfrastructure's shared CMake argument builder; `-ParallelJobs` applies to packaging as well as the ordinary build.
- Sanitizers apply to Debug builds only; `clangcl-debug` enables AddressSanitizer and UBSan by default. There is no Windows TSan preset (clang-cl does not support `-fsanitize=thread` on this target): use `linux-debug-tsan-clang` or `linux-debug-tsan-GNU`.

Run helpers, after building:

```pwsh
& ./scripts/windows/Invoke-ClangClDebug.ps1 2>&1 | Tee-Object -FilePath logs/debug/run.log
& ./scripts/windows/Invoke-ClangClRelease.ps1 2>&1 | Tee-Object -FilePath logs/release/run.log
```

With build dependencies missing on the host, use the container workflow below: the toolchain image ships clang-cl, CMake, Ninja, the Vulkan SDK, Rust and sccache.

## Windows Container Workflow (Stevedore)

The Windows builds also run fully containerized in the ANTfrastructure developer image `ghcr.io/kataglyphis/kataglyphis_beschleuniger:winamd64`, exactly like CI (`.github/workflows/windows-x64.yml`). Install [Stevedore](https://github.com/slonopotamus/stevedore) with `winget install stevedore` and reboot, then:

```pwsh
# defaults to clangcl-debug,clangcl-profile,clangcl-release
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1
```

- The script always uses Stevedore's `docker.exe`; `nerdctl` is not usable for builds or runs on Windows.
- Process isolation is the default, so the container sees all host CPUs.
- By default the sources stream into a reusable container via tar, and the build trees and logs stream back into the working tree; `-UseBindMount` bind-mounts the repo instead. On a Dev Drive the bind mount needs the container filesystem filters allow-listed once, elevated, then a reboot. The command, its one-quoted-argument trap, verification and revert: [`third_party/ANTfrastructure/docs/windows-container-build-performance.md`](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/windows-container-build-performance.md#transport-b--bind-mount-dev-drive-needs-setup). This repo's measured transport numbers and incremental-build wiring: [`container-build-caching.md`](container-build-caching.md) (on this Dev Drive host the tar-pipe measured faster than the bind mount; measure before switching).
- Builds are supported against the recorded submodule pins; restore them with `git submodule update --checkout --recursive`. The Windows scripts resolve PowerShell modules from the `third_party/ANTfrastructure` submodule first, then from this project's `scripts/windows/modules`, which holds no copy of an upstream module (see `scripts/windows/Resolve-BuildModule.ps1`). When bumping `third_party/FUZZTEST`, keep `ABSL_TAG` in `third_party/CMakeLists.txt` >= FuzzTest's own Abseil pin (see `AGENTS.md`).

## Packaging

### Linux release packages

```bash
bash ./scripts/linux/cmake-configure-build.sh \
  --preset linux-release-clang \
  --build-dir build-release \
  --build-config Release

bash ./scripts/linux/cmake-configure-build.sh \
  --build-dir build-release \
  --skip-configure true \
  --build-target package
```

The Vulkan SDK environment defaults to the image's `/opt/vulkan/<VULKAN_VERSION>/setup-env.sh`, with the version read from ANTfrastructure's `linux/scripts/01-core/versions.env`; `--vulkan-setup-script` points it elsewhere.

Artifacts land in the build directory: a TGZ, a DEB and an AppImage (`CPACK_ENABLE_APPIMAGE` defaults to `ON`; `-DCPACK_ENABLE_APPIMAGE=OFF` skips it). CPack uses a readable `appimagetool` from `PATH`, or downloads the release pinned with its SHA256 in ANTfrastructure's `versions.env`; a configure that cannot get a verified tool fails instead of silently dropping the AppImage.

### Windows MSIX

The Windows release workflow can produce an MSIX package. It is signed when a PFX certificate sits at the repository root (the first `*.pfx` there, gitignored; without one the build warns and the MSIX stays unsigned), with the password from `MSIX_PFX_PASSWORD`, else `MSIX_CERT_PASSWORD`. The signing is ANTfrastructure's `Invoke-MsixPackage -Sign -SigningRoot <repo root>`.

CI fetches the certificate over WebDAV instead of committing it: `Build-Windows.ps1 -WebDavHostname/-WebDavUsername/-WebDavPassword/-RemoteBasePath` (the "Build, test, package (x64)" job of `.github/workflows/windows-x64.yml`, which passes the `WEBDAV_*` secrets) drives ANTfrastructure's `windows/scripts/certificates/download_webdav_files.py` through the `WindowsWebDav.Common` module (`--extension .pfx`). Generating and importing certificates: ANTfrastructure `windows/scripts/certificates/README.md`.

## Shader Include Workflow

Shaders are written in [Slang](https://shader-slang.com/) under
`Resources/ShadersSlang/`. The build scripts compile them to SPIR-V (C++) and
WGSL (Rust). Details: [`shader-build-pipeline.md`](shader-build-pipeline.md).

## Troubleshooting

Missing Vulkan validation layers fail startup with:

```bash
[error] Validation layers requested, but not available!
[error] Failed to create a Vulkan instance!
ERROR: vkGetInstanceProcAddr: Invalid instance
```

Install the validation packages from your operating system or the Vulkan SDK, then retry. On Linux:

```bash
sudo apt install libvulkan1 vulkan-tools vulkan-validationlayers
```

On Windows, Debug builds abort at startup with exit code `-1073740791` (`0xC0000409`) right after logging `Validation layers requested, but not available!`. Install the Vulkan SDK (`winget install VulkanSDK`), or set `VK_LAYER_PATH` to a directory containing `VkLayer_khronos_validation.dll`/`.json`. Profile and Release builds run without validation layers. Running the AddressSanitizer Debug build by hand, keep the `ASAN_OPTIONS` `log_path` relative: an absolute `C:\...` path breaks ASAN option parsing at the drive-letter colon. The `Invoke-ClangClDebug.ps1` helper already does this.

In WSL2, a run that shows only `llvmpipe` (Mesa's software renderer) is missing the NVIDIA Vulkan ICD. Installing it and restarting WSL (`wsl --shutdown`), step by step: [WSL2 — Vulkan (NVIDIA)](wsl2_vulkan.rst).
