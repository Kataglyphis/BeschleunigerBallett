# Getting Started

## Prerequisites

Make sure these tools are available before you build the project:

- a C++23 capable compiler
- a C17 capable compiler
- CMake 4.1 or newer
- a Vulkan SDK installation for Vulkan-enabled builds
- Python plus the packages from `requirements.txt` for the docs build (the formatting steps install their own pinned `cmake-format` from ANTfrastructure)
- optionally Rust if you want to enable the experimental Rust path

## Clone the Repository

```bash
git clone --branch develop --recurse-submodules git@github.com:Kataglyphis/BeschleunigerBallett.git
cd BeschleunigerBallett
```

## Configure with CMake Presets

The repository already ships a `CMakePresets.json`. Start there instead of creating ad-hoc build commands.

```bash
cmake --list-presets
cmake --preset <preset-name>
cmake --build build --config Debug
ctest --test-dir build --output-on-failure
```

For Visual Studio style generators on Windows, add `-C Debug` or `-C Release` to `ctest`.

A host CMake older than 4.1 cannot read `CMakePresets.json` (`"version": 10`)
and fails `cmake --list-presets` with `Unrecognized "version" field` — read
the file itself, or the Windows configurations table in
[AGENTS.md](https://github.com/Kataglyphis/BeschleunigerBallett/blob/develop/AGENTS.md#windows-configurations-build-windowsps1),
instead.
On Windows, prefer `scripts/windows/Build-Windows-Container.ps1`, which builds
inside a container that already has a new enough CMake.

## Linux Workflow

For Linux, the helper script under `scripts/linux/` wraps the common configure-and-build path:

```bash
bash ./scripts/linux/cmake-configure-build.sh \
  --preset linux-debug-clang \
  --build-dir build \
  --build-config Debug
```

Useful adjacent scripts:

- `scripts/linux/run-ctest.sh`
- `scripts/linux/build-coverage-gcovr.sh`
- `scripts/linux/build-coverage-llvm.sh`
- `scripts/linux/run-perf-suite.sh`

## Windows Workflow

For Windows, use the orchestration script if you want configuration, build, formatting, and tests from one entry point. The available configurations are `msvc-debug`, `msvc-release`, `clangcl-debug` (Debug with ASAN/UBSan), `clangcl-profile` (RelWithDebInfo with benchmarks), and `clangcl-release`:

```pwsh
# single configuration
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows.ps1 -Configurations clangcl-debug

# full sanitizer/profile/release sweep
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows.ps1 `
  -Configurations "clangcl-debug,clangcl-profile,clangcl-release"
```

`-TargetArch arm64` cross-builds `clangcl-release` for Windows on Arm inside the container image's arm64 bundle (`:winarm64`) and writes the portable bundle, MSI, ZIP and MSIX to `dist/windows-arm64`; that is what `.github/workflows/windows-arm64-cross.yml` runs.

Sanitizers apply to Debug builds only. `clangcl-debug` enables AddressSanitizer and UBSan by default. There is no Windows TSan preset (clang-cl does not support `-fsanitize=thread` on this target); use `linux-debug-tsan-clang` or `linux-debug-tsan-GNU` for real TSan runs.

After building, these run helpers are available:

```pwsh
& ./scripts/windows/Invoke-ClangClDebug.ps1 2>&1 | Tee-Object -FilePath logs/debug/run.log
& ./scripts/windows/Invoke-ClangClRelease.ps1 2>&1 | Tee-Object -FilePath logs/release/run.log
```

If build dependencies are missing on the host, prefer the containerized workflow below — the toolchain image ships everything (clang-cl, CMake, Ninja, Vulkan SDK, Rust, sccache).

## Windows Container Workflow (Stevedore)

The Windows builds also run fully containerized in the ANTfrastructure developer image `ghcr.io/kataglyphis/kataglyphis_beschleuniger:winamd64`, exactly like CI (`.github/workflows/windows-x64.yml`). Install [Stevedore](https://github.com/slonopotamus/stevedore) with `winget install stevedore` and reboot, then:

```pwsh
# defaults to clangcl-debug,clangcl-profile,clangcl-release
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1
```

Details worth knowing:

- The script always uses Stevedore's `docker.exe`; `nerdctl` is not usable for builds or runs on Windows.
- Process isolation is the default so the container sees all host CPUs.
- By default the script streams the sources into a reusable container via tar and streams the resulting build trees and logs back into the working tree; `-UseBindMount` opts into bind-mounting the repo instead. On a Dev Drive the bind mount additionally requires the container filesystem filters to be allow-listed once from an elevated prompt, followed by a reboot: `fsutil devdrv setFiltersAllowed /volume D: "bindFlt,wcifs"` — the filter list must be one quoted argument (unquoted `bindFlt, wcifs` is parsed as two arguments and fails). Setup, verification, and revert steps live in `third_party/ANTfrastructure/docs/windows-container-build-performance.md`; this repo's measured transport numbers and incremental-build wiring live in `docs/container-build-caching.md` (on this Dev Drive host the tar-pipe measured faster than the bind mount — measure before switching).
- Builds are supported against the recorded submodule pins; restore them with `git submodule update --checkout --recursive`. The Windows scripts resolve PowerShell modules from the `third_party/ANTfrastructure` submodule first, then from this project's own modules in `scripts/windows/modules`, which holds no copy of an upstream module (see `scripts/windows/Resolve-BuildModule.ps1`). When bumping `third_party/FUZZTEST`, keep `ABSL_TAG` in `third_party/CMakeLists.txt` >= FuzzTest's own Abseil pin (see `AGENTS.md`).

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

Artifacts land in the selected build directory: a TGZ, a DEB and an AppImage (`CPACK_ENABLE_APPIMAGE` defaults to `ON`; configure with `-DCPACK_ENABLE_APPIMAGE=OFF` to skip it). CPack uses a readable `appimagetool` from `PATH`, or downloads the release pinned with its SHA256 in ANTfrastructure's `versions.env`; a configure that cannot get a verified tool fails instead of silently dropping the AppImage.

### Windows MSIX

The Windows release workflow can produce an MSIX package. It is signed when a PFX certificate sits at the repository root (the first `*.pfx` there, gitignored; without one the build warns and the MSIX stays unsigned), with the certificate password from `MSIX_PFX_PASSWORD` or, as a fallback, `MSIX_CERT_PASSWORD`. The signing itself is ANTfrastructure's `Invoke-MsixPackage -Sign -SigningRoot <repo root>`.

CI retrieves the certificate over WebDAV instead of committing it: `Build-Windows.ps1 -WebDavHostname/-WebDavUsername/-WebDavPassword/-RemoteBasePath` (see the "Build/Test/Package" step of `.github/workflows/windows-x64.yml`) drives ANTfrastructure's `windows/scripts/certificates/download_webdav_files.py` through the `WindowsWebDav.Common` module (`--extension .pfx`; generating and importing certificates is documented in ANTfrastructure `windows/scripts/certificates/README.md`).

## Shader Include Workflow

Shaders are written in [Slang](https://shader-slang.com/) under
`Resources/ShadersSlang/`. The build scripts compile them to SPIR-V (C++) and
WGSL (Rust). See `docs/shader-build-pipeline.md` for details.

## Troubleshooting

If Vulkan validation layers are missing, install the validation packages from your operating system or Vulkan SDK before retrying the build or run workflow. Startup then fails with errors like:

```bash
[error] Validation layers requested, but not available!
[error] Failed to create a Vulkan instance!
ERROR: vkGetInstanceProcAddr: Invalid instance
```

On Linux, install the runtime packages first:

```bash
sudo apt install libvulkan1 vulkan-tools vulkan-validationlayers
```

On Windows this shows up as Debug builds aborting at startup with exit code `-1073740791` (`0xC0000409`) right after logging `Validation layers requested, but not available!`. Install the Vulkan SDK (`winget install VulkanSDK`), or set `VK_LAYER_PATH` to a directory containing `VkLayer_khronos_validation.dll`/`.json`. Profile and Release builds run without validation layers. When running the AddressSanitizer Debug build manually, keep the `ASAN_OPTIONS` `log_path` relative (an absolute `C:\...` path breaks ASAN option parsing at the drive-letter colon) — the `Invoke-ClangClDebug.ps1` helper already handles this.