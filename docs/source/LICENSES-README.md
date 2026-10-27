# Licences of the open-source dependencies

The open-source dependencies this repository uses, with their licences. The list was checked against `git submodule status`, `third_party/CMakeLists.txt` (FetchContent), the vendored sources under `third_party/` and `third_party/OxidANT/crates/webgpu_renderer/Cargo.toml`. Every licence entry comes from the cited licence file, read on disk, or from the `license` field of the Cargo metadata, not from assumptions. For legally binding terms, the cited licence file itself is authoritative.

Created in the repository on request. Verification history:

- **2026-08-02**: last full verification against the disk; the Rust crate licences also against `cargo metadata --format-version 1 --locked` in the `:latest-cross` container (full crates.io registry).
- **2026-09-25**: the pins that had moved since (imgui, glfw, nlohmann/json, google/benchmark, OxidANT, ANTfrastructure) and the crate versions (log, wgpu, pollster, glam, egui, egui-wgpu, egui-winit) updated, their licences read from disk again; the other rows unchanged.
- **2026-10-07**: the pins of imgui, nlohmann/json, google/benchmark, spdlog, abseil-cpp, Microsoft GSL and OxidANT and glam 0.34.1 updated; the licence files of the four submodules are unchanged across the moved ranges (`git diff` empty). That evening nlohmann/json moved to 367336c83 and Microsoft GSL to v5.0.1; `LICENSE.MIT` and GSL's `LICENSE` are unchanged across both ranges.

## What every package ships

Every package (DEB, TGZ, AppImage, ZIP, MSI, NSIS, MSIX) installs these under
`share/licenses/GraphicsEngine/` (`/usr/share/...` in the DEB):

- `LICENSE`: the project's own text (ANTfrastructure's `CPackCommon`).
- `third_party/<component>/`: the original license file of each library the engine
  links and of Dear ImGui's fonts, as `licenses/third-party-licenses.txt` maps them;
  `cmake/ThirdPartyLicenses.cmake` installs that list and fails the configure on a
  missing or empty source. GSL and abseil come from their FetchContent checkouts.
- `cargo/`: the original texts of every crate in `kataglyphis_cxx_bridge`'s normal graph
  for the Rust target Corrosion builds, with a `manifest.json` of names, versions,
  sources and SHA-256s. `cmake/CargoLicenses.cmake` stages them with ANTfrastructure's
  `stage-cargo-licenses.py` and OxidANT's reviewed `packaging/cargo-license-policy.json`.
  A Windows checkout made before OxidANT pinned `LICENSE` to LF fails with
  `policy source SHA-256 mismatch`; re-checkout `third_party/OxidANT/LICENSE`.

Dear ImGui carries no texts for its fonts, so `licenses/fonts/` holds the ones each
font's name table names, copied unchanged; `licenses/imgui-fonts.NOTICE` records each
font's copyright, its license and the source and SHA-256 of every text. The Sponza scene's
texts sit beside the model. KTX is linked as headers only and no source includes one, so it
ships no text. Not covered yet: the GCC runtime and glibc copy that the Linux packages carry
for older hosts, and the MSVC runtime DLLs beside the Windows bundle; those belong to
ANTfrastructure's closure helpers.

## Git submodules under `third_party/` (shipped)

| Project | URL | Pin | Licence (per licence file) | File checked |
|---|---|---|---|---|
| tinyobjloader | https://github.com/tinyobjloader/tinyobjloader | v2.0.0rc10-73-g45636bd | MIT | `third_party/TINY_OBJ_LOADER/LICENSE` |
| glm | https://github.com/g-truc/glm | 6f14f479 | Dual: "The Happy Bunny License or MIT License" | `third_party/GLM/copying.txt` |
| imgui | https://github.com/ocornut/imgui | v1.92.9b-112-gf502f9c98 | MIT | `third_party/IMGUI/LICENSE.txt` |
| stb | https://github.com/nothings/stb | 2c980bb5 | Dual: MIT or public domain (your choice) | `third_party/STB/LICENSE` |
| glfw | https://github.com/glfw/glfw | 3.5.1-1-g92dcf4ce | zlib/libpng licence text | `third_party/GLFW/LICENSE.md` |
| Vulkan Memory Allocator (VMA) | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | 3aa92122 | MIT licence text (Copyright AMD) | `third_party/VULKAN_MEMORY_ALLOCATOR/LICENSE.txt` |
| nlohmann/json | https://github.com/nlohmann/json | v3.11.3-695-g367336c83 | MIT | `third_party/NLOHMANN_JSON/LICENSE.MIT` |
| google/benchmark | https://github.com/google/benchmark | v1.9.5-143-g39a78e3 | Apache-2.0 | `third_party/GOOGLE_BENCHMARK/LICENSE` |
| spdlog | https://github.com/gabime/spdlog | v1.17.0-53-gd6c93f38 | MIT; contains a bundled {fmt} (MIT) | `third_party/SPDLOG/LICENSE`; `third_party/SPDLOG/include/spdlog/fmt/bundled/fmt.license.rst` |
| google/fuzztest | https://github.com/google/fuzztest | 704efb34 (2026-06-29) | Apache-2.0; an additional Lucent notice for `fuzztest/internal/domains/rune.*` | `third_party/FUZZTEST/LICENSE` |
| kompute (only the optional playground, `KATAGLYPHIS_BUILD_KOMPUTE_PLAYGROUND`) | https://github.com/KomputeProject/kompute | v0.9.0-99-g267e019 | Apache-2.0 | `third_party/KOMPUTE/LICENSE` |
| cgltf | https://github.com/jkuhlmann/cgltf | v1.15-11-g85cd623 | MIT licence text (Copyright Johannes Kuhlmann) | `third_party/cgltf/LICENSE` |
| tomlplusplus | https://github.com/marzer/tomlplusplus | v3.4.0-50-g1e8829b | MIT | `third_party/tomlplusplus/LICENSE` |

## Vendored sources (no submodule)

| Project | URL | Licence (per file headers) | File(s) checked |
|---|---|---|---|
| KTX (KTX-Software, partly vendored: `include/`, `lib/`, `other_include/`) | https://github.com/KhronosGroup/KTX-Software | Apache-2.0 (SPDX header in `include/ktx.h` and in 98 files under `lib/`) | `third_party/KTX/include/ktx.h` |

KTX-Software's full `LICENSE.md` is **not** vendored
(`third_party/KTX/LICENSE.md` does not exist), so the third-party components
embedded under `third_party/KTX/lib/` were verified one by one on disk, from a
vendored licence file or from the source file's own licence header:

| Component | Licence | File checked (each under `third_party/KTX/lib/`) |
|---|---|---|
| basisu (encoder + transcoder) | Apache-2.0 (Copyright Binomial LLC) | `basisu/LICENSE` (full Apache-2.0 text); licence header in `basisu/encoder/basisu_enc.cpp` and `basisu/transcoder/basisu_transcoder.cpp`. The REUSE texts `basisu/LICENSES/{Apache-2.0,BSD,Zlib}.txt` cover the subcomponents. |
| zstd (bundled in basisu) | BSD-3-Clause; the file header also offers GPLv2 ("You may select, at your option, one of the above-listed licenses") | `basisu/zstd/LICENSE` (BSD text, Copyright Facebook, Inc.); header of `basisu/zstd/zstd.c` |
| LodePNG | zlib licence text (Copyright 2005-2019 Lode Vandevenne) | licence header in `basisu/encoder/lodepng.h` |
| jpgd | Public domain (Rich Geldreich) | licence header in `basisu/encoder/jpgd.h` |
| stb_image / stb_image_write | Dual: MIT ("ALTERNATIVE A") or public domain/Unlicense ("ALTERNATIVE B"), your choice | licence block at the end of `astc-encoder/Source/stb_image.h` and `astc-encoder/Source/stb_image_write.h` |
| tinyexr | BSD-3-Clause (Copyright 2014-2019 Syoyo Fujita and contributors) | licence header in `astc-encoder/Source/tinyexr.h` |
| etcdec.cxx | **No OSI licence**: "Ericsson Texture Compression Codec Software License Agreement", a non-exclusive, non-transferable, free, perpetual, worldwide single licence with its own terms | licence header in `etcdec.cxx` |
| astc-encoder (Arm) | Apache-2.0 | SPDX header `// SPDX-License-Identifier: Apache-2.0` in `astc-encoder/Source/astcenc.h`. The `LICENSE.txt` that `astc-encoder/README.md` refers to is not vendored. |
| dfdutils (Khronos) | Apache-2.0; individual files taken from MIT projects are MIT | `dfdutils/LICENSE.adoc`; full texts in `dfdutils/LICENSES/{Apache-2.0,MIT}.txt` |

`etcdec.cxx` is the only component in the whole tree that is not under an
OSI-approved licence. For redistribution, the wording of the SLA in its file
header is authoritative.

## Build-time dependencies via FetchContent (`third_party/CMakeLists.txt`, not checked in)

Each licence was read from the LICENSE file of the local FetchContent checkout
under `build*/_deps/`. Those checkouts are not checked in; the paths record
which file was actually read.

| Project | URL | Pin | Licence (per licence file) | File checked |
|---|---|---|---|---|
| abseil-cpp | https://github.com/abseil/abseil-cpp | 20260817.0 (`ABSL_TAG`) | Apache-2.0 | `build-clangcl-profile/_deps/abseil-cpp-src/LICENSE` |
| googletest (only with `BUILD_TESTING`) | https://github.com/google/googletest | v1.18.0 (tag archive) | BSD-3-Clause licence text (Copyright Google Inc.) | `build-clangcl-profile/_deps/googletest-src/LICENSE` |
| Microsoft GSL | https://github.com/microsoft/GSL | v5.0.1 | MIT | `build-clangcl-profile/_deps/gsl-src/LICENSE` |
| Corrosion (build tool) | https://github.com/corrosion-rs/corrosion | c4786e7a (a commit; v0.6.1 lacks later fixes) | MIT | `build-clangcl-profile/_deps/corrosion-src/LICENSE` |
| ANTLR4 C++ runtime (transitive, via FuzzTest) | https://github.com/antlr/antlr4 | pinned by FuzzTest | BSD-3-Clause licence text (The ANTLR Project) | `build-linux-local/_deps/antlr_cpp-src/LICENSE.txt` |
| RE2 (transitive, via FuzzTest) | https://github.com/google/re2 | pinned by FuzzTest | BSD-3-Clause licence text (The RE2 Authors) | `build-linux-local/_deps/re2-src/LICENSE` |

## Internal, project-specific submodules

| Project | URL | Pin | Licence | Source checked |
|---|---|---|---|---|
| OxidANT | https://github.com/Kataglyphis/OxidANT | d610fb49 (develop) | MIT (`LICENSE`, Copyright (c) 2025 Kataglyphis; `license = "MIT"` in `[workspace.package]`) | `third_party/OxidANT/LICENSE` + `third_party/OxidANT/Cargo.toml` |
| ANTfrastructure | https://github.com/Kataglyphis/ANTfrastructure | 7d819f77 | MIT per `LICENSE` (Copyright (c) 2024 Jonas Heinle); 275 of its own files carry `SPDX-License-Identifier: MIT`, **25 carry `Apache-2.0` again** (see "Unverified, to check"); OCI label `org.opencontainers.image.licenses="MIT"` in the three image definitions that set one (`linux/Dockerfile.torch`, `windows/Dockerfile`, `windows/Dockerfile.torch`) | `third_party/ANTfrastructure/LICENSE` + SPDX headers + OCI labels |

## Rust crate dependencies (`third_party/OxidANT/crates/webgpu_renderer/Cargo.toml`, `[dependencies]`)

Versions per `third_party/OxidANT/Cargo.lock`.

**Source of the licence entries:** the `license` field of the Cargo metadata,
exactly the field the crate published on crates.io, copied unchanged (hence,
for example, the old separator in `pollster`). No entry comes from a README, a
neighbouring version or a guess.

- **Rows unchanged since 2026-08-02**: read for the exact version in
  `Cargo.lock` on 2026-08-02 in the `:latest-cross` container of the time
  (volume `cargo-cache`; it held the full registry, while the host cache was
  incomplete because the crate builds run in the container) with the command
  below. The `source` of every package was checked as well; all are
  `registry+https://github.com/rust-lang/crates.io-index`.

  ```sh
  cargo metadata --format-version 1 --locked   # CARGO_HOME=/cargo-cache
  ```

- **Rows updated on 2026-09-25** (a new lock version): the same `license`
  field, read in the `Cargo.toml` of the exact version in the host's registry
  cache (`scoop/persist/rustup/.cargo/registry/src`) or, for the patched
  `egui-winit`, in the vendored
  `third_party/OxidANT/third_party/egui-winit-0.36.2/Cargo.toml`
  (`[patch.crates-io]` in `third_party/OxidANT/Cargo.toml`). `egui` and
  `egui-wgpu` 0.36.2 are not in that cache; their entry was read from 0.36.1
  (see "Unverified, to check").

| Crate | Version (lock) | Licence (`license` field) | Source checked |
|---|---|---|---|
| anyhow | 1.0.104 | MIT OR Apache-2.0 | `cargo metadata` (exact lock version, container) |
| log | 0.4.34 | MIT OR Apache-2.0 | `Cargo.toml` of the exact lock version (host cache, 2026-09-25) |
| bytemuck | 1.25.2 | Zlib OR Apache-2.0 OR MIT | `cargo metadata` (exact lock version, container) |
| env_logger | 0.11.11 | MIT OR Apache-2.0 | `cargo metadata` (exact lock version, container) |
| wgpu | 30.0.1 | MIT OR Apache-2.0 | `Cargo.toml` of the exact lock version (host cache, 2026-09-25) |
| winit | 0.30.13 | Apache-2.0 | `cargo metadata` (exact lock version, container) |
| pollster | 1.0.1 | Apache-2.0/MIT | `Cargo.toml` of the exact lock version (host cache, 2026-09-25) |
| glam | 0.34.1 | MIT OR Apache-2.0 | `license` field of the exact lock version on crates.io (2026-10-07) |
| gltf | 1.4.1 | MIT OR Apache-2.0 | `cargo metadata` (exact lock version, container) |
| bevy_mikktspace | 1.0.0 | Zlib AND (MIT OR Apache-2.0) | `cargo metadata` (exact lock version, container) |
| egui | 0.36.2 | MIT OR Apache-2.0 | `Cargo.toml` of 0.36.1 (host cache, 2026-09-25); 0.36.2 itself not read |
| egui-wgpu | 0.36.2 | MIT OR Apache-2.0 | `Cargo.toml` of 0.36.1 (host cache, 2026-09-25); 0.36.2 itself not read |
| egui-winit | 0.36.2 (vendored patch) | MIT OR Apache-2.0 | `third_party/OxidANT/third_party/egui-winit-0.36.2/Cargo.toml` (2026-09-25) |
| ktx2 | 0.5.0 | Apache-2.0 | `cargo metadata` (exact lock version, container) |

Notes on single entries:

- **winit 0.30.13** and **ktx2 0.5.0** are *not* dual-licensed but plain
  Apache-2.0, unlike the other crates in this table.
- **bevy_mikktspace 1.0.0** is a compound expression: the vendored MikkTSpace C
  code is under Zlib, the Rust part dual MIT/Apache-2.0; both conditions apply
  at once (`AND`).
- **pollster 1.0.1** uses the deprecated slash notation `Apache-2.0/MIT`; it
  means a choice (equivalent to `Apache-2.0 OR MIT`).

## Toolchain (build time, not shipped)

| Tool | Origin | Licence |
|---|---|---|
| slangc (Slang shader compiler) | From the installed Vulkan SDK (`VULKAN_SDK\Bin` or `PATH`), see `scripts/windows/Build-SlangShaders.ps1` / `scripts/linux/compile-slang-shaders.sh`; not vendored in the repository | Toolchain, not shipped; licence not verifiable from the repository |

## Unverified, to check

Open since the check of 2026-09-25:

- **ANTfrastructure, the MIT/Apache contradiction is back**: `LICENSE` says MIT
  and 275 of its own files carry `SPDX-License-Identifier: MIT`, but 25 of its
  own files carry `SPDX-License-Identifier: Apache-2.0` again, together with
  "All rights reserved." (among them `windows/scripts/host/*.ps1`,
  `windows/scripts/build/Build-*All.ps1`,
  `windows/scripts/modules/WindowsSourceBuild.Common.psm1`,
  `.claude/hooks/guard-destructive-deletes.*`). To be fixed in ANTfrastructure,
  not here.
- **egui / egui-wgpu 0.36.2**: the `license` field was read from 0.36.1,
  because 0.36.2 is not in the host cache; confirm it for the exact version
  with `cargo metadata --locked` in the container.

Resolved on 2026-08-07:

- **ANTfrastructure, missing LICENSE**: the submodule now carries a `LICENSE`
  file; its pin was also corrected from `2ac69e95` to the then current
  `45709430`.
- **ANTfrastructure, MIT/Apache contradiction**: resolved in favour of **MIT**.
  `LICENSE` said MIT while 101 of its own source files carried
  `SPDX-License-Identifier: Apache-2.0` and two of the three image definitions
  set the OCI label `Apache-2.0`. All three places say MIT now, and the phrase
  "All rights reserved.", which contradicts MIT, left the headers. Not touched:
  `linux/webserver/dist/assets/NOTICES` and `docs/deps/deps.json`, which state
  third-party licences (among them Apache 2.0 for LLVM, TVM, OpenCV, Vulkan
  SDK) and stay correct.

Resolved on 2026-08-02:

- The 11 previously unverified Rust crates (env_logger, wgpu, winit, pollster,
  glam, gltf, bevy_mikktspace, egui, egui-wgpu, egui-winit, ktx2), verified
  from `cargo metadata` in the container (crate table above).
- `anyhow`, `log` and `bytemuck`, verified for the **exact** lock version
  instead of a neighbouring cached one; their licence expressions did not
  change.
- The embedded third-party parts in `third_party/KTX/lib/`, verified one by
  one from the files on disk (table under "Vendored sources").

## Notes

- Removed since the state of 2026-03-26: **glad** (no `third_party/glad` submodule any more, no glad or OpenGL loader references under `Src/`) and **KTX as a submodule** (now partly vendored, see above).
- Kompute is built only with `KATAGLYPHIS_BUILD_KOMPUTE_PLAYGROUND=ON` (a demo, not part of the engine, in no package). Since 2026-10-06 only the Linux CI job `clang-tests` does that, and it also starts the playground; Kompute then fetches fmt 11.0.0 through FetchContent.
