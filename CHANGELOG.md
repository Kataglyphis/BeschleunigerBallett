# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Every push to `main` releases `v<VERSION.txt's version>` once every lane is green (`release.yml`),
with this file's `## [<version>]` section as the release notes, so before a merge raise
`VERSION.txt` and turn `## [Unreleased]` into that section. The notes of v1.4.2 and earlier are on
the GitHub releases themselves.

## [Unreleased]

## [1.5.2] - 2026-10-17

### Added
- **Windows arm64 ships a setup `.exe` too** (2026-10-17), as x64 always did. NSIS has no arm64 stub,
  so it is the same x86 stub, which Windows on Arm runs emulated; the arm64 build used to drop it
  because the hub's arch gate refused that stub. The gate now reports an NSIS installer instead of
  failing it, and the `windows-11-arm` job installs it silently, checks that every installed binary
  is ARM64, starts `bin/GraphicsEngine.exe --version` from the install directory and uninstalls it again
  (`installed-run-command`). The hub moves to 4cfaabcb. Owner rule of the same day: every release ships
  Windows arm64 installers next to the x64 ones.

### Changed

- 2026-10-17 — **The scenes are AssetANT release assets, not tracked files.** The fourteen scene
  directories under `Resources/Models` (518 of the 633 MiB HEAD tracked) left the tree for
  [AssetANT](https://github.com/Kataglyphis/AssetANT), one reproducible `<name>.tar.xz` per release.
  `Resources/assets.lock` pins each by SHA-256 and URL, and `cmake/FetchAssets.cmake` extracts
  `crytek-sponza` and `Dinosaurs` into `Resources/Models` at configure time through a per-user cache,
  so the paths the engine loads did not move. `-DKATAGLYPHIS_FETCH_ALL_ASSETS=ON` fetches all
  fourteen, and `cmake -P cmake/FetchAssets.cmake` fetches without a configure. The test fixtures
  (`GltfTest`, `ShadowTest`, `Plane`, `VikingRoom`, `plane.obj`, `suzanne.obj`) and
  `Resources/Textures` stay tracked, and the history keeps every scene: nothing was rewritten.

## [1.5.1] - 2026-10-17

### Fixed

- 2026-10-16 — **The Linux packages start outside the build image.** 1.5.0's `.deb`, tarball and
  AppImage need GCC 16.2's `libstdc++` (`GLIBCXX_3.4.36`, newer than any distro's) and carry
  none; the `.deb` claimed `libstdc++6 (>= 13.1)`. ANTfrastructure's
  `kataglyphis_install_linux_closure` now puts the compiler's runtime in `lib/graphicsengine/`
  together with the image's loader and glibc 2.43, and `bin/GraphicsEngine` is a launcher that
  starts the program through them (`LOADER`), so an older glibc (Debian 13, Ubuntu 24.04) is no
  obstacle: the arm64 `.deb`, tarball and AppImage start on a Debian 13 Raspberry Pi 5 (measured
  2026-10-17). The `.deb` takes only the Vulkan loader from the distro. GraphicsEngine finds its
  resources from the working directory, which the launcher leaves alone.
- 2026-10-16 — **A clean-host job proves it**: after each Linux lane, `ubuntu-26.04` and
  `ubuntu-26.04-arm` install the `.deb` with apt, check every installed ELF with `ldd`, and start
  `GraphicsEngine --version` from the `.deb`, the unpacked tarball and the AppImage.

## [1.5.0] - 2026-10-16

### Changed

- 2026-10-16 — **A push runs Linux x64 and the gates; Linux arm64, Linux riscv64, Windows x64 and
  Windows arm64 run on request** (`on-request.yml`, the ci token in a commit message, or each
  lane's Run workflow button), the family's rule in [ANTfrastructure's
  `branch-and-release-strategy.md`](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/branch-and-release-strategy.md).
- 2026-10-16 — **Every push to `main` releases `v<version>`, and only when all five lanes are
  green** (`release.yml`). It carries the `.deb`, AppImage, flatpak (named per arch) and tarball of
  both Linux arches and the MSI, MSIX and zip of both Windows arches plus x64's installer, gathered
  by `scripts/linux/collect-release-assets.sh`; its suite runs with Linux x64 on every push. The
  Linux packages upload as `linux-packages-<arch>`, since one release run holds both arches.
  `gh workflow run release.yml --ref develop -f dry-run=true` runs it all on `develop` and
  publishes nothing.
