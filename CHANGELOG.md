# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Every push to `main` releases `v<VERSION.txt's version>` once every lane is green (`release.yml`),
with this file's `## [<version>]` section as the release notes, so before a merge raise
`VERSION.txt` and turn `## [Unreleased]` into that section. The notes of v1.4.2 and earlier are on
the GitHub releases themselves.

## [Unreleased]

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
