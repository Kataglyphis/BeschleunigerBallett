# Getting Started

## Prerequisites

Make sure these tools are available before you build the project:

- a C++23 capable compiler
- a C17 capable compiler
- CMake 4.1 or newer
- a Vulkan SDK installation for Vulkan-enabled builds
- Python plus the packages from `requirements.txt` for docs and formatting tasks
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

Sanitizers apply to Debug builds only. `clangcl-debug` enables AddressSanitizer and UBSan by default. There is no Windows TSan preset (clang-cl does not support `-fsanitize=thread` on this target); use `linux-debug-tsan-clang` or `linux-debug-tsan-GNU` for real TSan runs.

After building, these run helpers are available:

```pwsh
& ./scripts/windows/Invoke-ClangClDebug.ps1 2>&1 | Tee-Object -FilePath logs/debug/run.log
& ./scripts/windows/Invoke-ClangClRelease.ps1 2>&1 | Tee-Object -FilePath logs/release/run.log
```

If build dependencies are missing on the host, prefer the containerized workflow below — the toolchain image ships everything (clang-cl, CMake, Ninja, Vulkan SDK, Rust, sccache).

## Windows Container Workflow (Stevedore)

The Windows builds also run fully containerized in the ANTfrastructure developer image `ghcr.io/kataglyphis/kataglyphis_beschleuniger:winamd64`, exactly like CI (`.github/workflows/Windows.yml`). Install [Stevedore](https://github.com/slonopotamus/stevedore) with `winget install stevedore` and reboot, then:

```pwsh
# defaults to clangcl-debug,clangcl-profile,clangcl-release
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1
```

Details worth knowing:

- The script always uses Stevedore's `docker.exe`; `nerdctl` is not usable for builds or runs on Windows.
- Process isolation is the default so the container sees all host CPUs.
- By default the script streams the sources into a reusable container via tar and streams the resulting build trees and logs back into the working tree; `-UseBindMount` opts into bind-mounting the repo instead. On a Dev Drive the bind mount additionally requires the container filesystem filters to be allow-listed once from an elevated prompt, followed by a reboot: `fsutil devdrv setFiltersAllowed /volume D: "bindFlt,wcifs"` — the filter list must be one quoted argument (unquoted `bindFlt, wcifs` is parsed as two arguments and fails). Setup, verification, and revert steps live in `third_party/ANTfrastructure/docs/windows-container-build-performance.md`; this repo's measured transport numbers and incremental-build wiring live in `docs/container-build-caching.md` (on this Dev Drive host the tar-pipe measured faster than the bind mount — measure before switching).
- Builds are supported against the recorded submodule pins; restore them with `git submodule update --checkout --recursive`. The Windows scripts resolve PowerShell modules from the `third_party/ANTfrastructure` submodule when available, falling back to vendored copies in `scripts/windows/modules` (see `scripts/windows/Resolve-BuildModule.ps1`). When bumping `third_party/FUZZTEST`, keep `ABSL_TAG` in `third_party/CMakeLists.txt` >= FuzzTest's own Abseil pin (see `AGENTS.md`).

## Packaging

### Linux release packages

```bash
bash ./scripts/linux/cmake-configure-build.sh \
  --vulkan-setup-script /opt/vulkan/1.4.341.1/setup-env.sh \
  --preset linux-release-clang \
  --build-dir build-release \
  --build-config Release

bash ./scripts/linux/cmake-configure-build.sh \
  --vulkan-setup-script /opt/vulkan/1.4.341.1/setup-env.sh \
  --build-dir build-release \
  --skip-configure true \
  --build-target package
```

Artifacts land in the selected build directory. For AppImage packaging, enable `CPACK_ENABLE_APPIMAGE=ON` on a separate release build tree and ensure `appimagetool` is on `PATH`:

```bash
cmake -S . -B build-release-appimage \
  --preset linux-release-clang \
  -DCPACK_ENABLE_APPIMAGE=ON
cmake --build build-release-appimage --config Release --target package
```

### Windows MSIX

The Windows release workflow can produce an MSIX package. If signing is enabled, place the PFX certificate at the repository root and provide the certificate password through `MSIX_PFX_PASSWORD` or `MSIX_CERT_PASSWORD`.

CI retrieves the certificate over WebDAV instead of committing it: `Build-Windows.ps1 -WebDavHostname/-WebDavUsername/-WebDavPassword/-RemoteBasePath` (see the "Build/Test/Package" step of `.github/workflows/Windows.yml`) drives ANTfrastructure's `windows/scripts/certificates/download_webdav_files.py` through the `WindowsWebDav.Common` module (`--extension .pfx`; generating and importing certificates is documented in ANTfrastructure `windows/scripts/certificates/README.md`).

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