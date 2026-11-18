# Licences of the open-source dependencies

The open-source dependencies this repository uses, with their licences. The list was checked against `git submodule status`, `third_party/CMakeLists.txt` (FetchContent), the vendored sources under `third_party/` and `third_party/OxidANT/crates/webgpu_renderer/Cargo.toml`. Every licence entry comes from the cited licence file, read on disk, or from the `license` field of the Cargo metadata, not from assumptions. For legally binding terms, the cited licence file itself is authoritative.

Created in the repository on request. Verification history:

- **2026-08-02**: last full verification against the disk; the Rust crate licences also against `cargo metadata --format-version 1 --locked` in the `:latest-cross` container (full crates.io registry).
- **2026-09-25**: the pins that had moved since (imgui, glfw, nlohmann/json, google/benchmark, OxidANT, ANTfrastructure) and the crate versions (log, wgpu, pollster, glam, egui, egui-wgpu, egui-winit) updated, their licences read from disk again; the other rows unchanged.
- **2026-10-07**: the pins of imgui, nlohmann/json, google/benchmark, spdlog, abseil-cpp, Microsoft GSL and OxidANT and glam 0.34.1 updated; the licence files of the four submodules are unchanged across the moved ranges (`git diff` empty). That evening nlohmann/json moved to 367336c83 and Microsoft GSL to v5.0.1; `LICENSE.MIT` and GSL's `LICENSE` are unchanged across both ranges.
- **2026-11-14**: the Renovate run moved imgui, stb, Vulkan Memory Allocator, nlohmann/json, google/benchmark, spdlog, google/fuzztest (off its `release_2026-06-29` line to `main`), tomlplusplus, OxidANT and ANTfrastructure; every row below names its new pin. The licence files of imgui, VMA, nlohmann/json, google/benchmark, spdlog (with the bundled {fmt}), google/fuzztest (with the Lucent `rune.*` notice), tomlplusplus and OxidANT are unchanged from the pin each row named before (`git diff` empty). stb's `LICENSE` preamble changed in `aae9fee` from a free choice between two licences to "in the public domain; most of them also MIT"; `stb_image.h` v2.32 and `stb_image_write.h` v1.18, the two headers this repository includes, still carry the MIT-or-public-domain block. ANTfrastructure's SPDX headers were counted again, and egui, egui-wgpu and egui-winit moved to 0.37.0, each `license` field read for that exact version.
- **2026-11-15 (later)**: OxidANT moved to `eb97c364`, its renderer parity work merged with its `develop`. `LICENSE` is unchanged across the range (`git diff` empty); `Cargo.lock` adds `epaint_default_fonts` 0.37.0 for OxidANT's `gui` crate (which neither the bridge nor the renderer uses) and `sha2` to the renderer's dev-dependencies. The renderer takes `image` 0.25.10 as a normal dependency (it was a dev-dependency); the C++ bridge (`kataglyphis_cxx_bridge`, `cxx` only) does not depend on the renderer, so the packaged crate texts are unchanged. The renderer embeds Dear ImGui's Roboto Medium and Cousine (byte-identical to `third_party/IMGUI/misc/fonts`, SHA-256 checked), and the docs site's demo carries the Colosseum: both in *The docs site's WebGPU demo* below.
- **2026-11-15**: OxidANT moved on to `f8b8c587` and ANTfrastructure to `2c81ef49`, pulled before the commit. Both `LICENSE` files and OxidANT's `Cargo.lock` are unchanged across the moved ranges (`git diff` empty); at `2c81ef49` all 589 SPDX headers of the hub's own files say MIT.
- **2026-11-16**: OxidANT moved to `78303c2`. `LICENSE` and `Cargo.lock` are unchanged across the range (`git diff` empty). The renderer's editor now also embeds two symbol subsets of egui's emoji fonts (`epaint_default_fonts` 0.37.0): Noto Emoji (OFL-1.1) and emoji-icon-font (MIT); OxidANT's fonts `NOTICE` records each source, hash and `pyftsubset` command and carries the MIT text, and its cargo-license policy re-pins that `NOTICE`. Both in *The docs site's WebGPU demo* below.
- **2026-11-16 (later)**: OxidANT moved to `026ef42`. `LICENSE` is unchanged across the range (`git diff` empty). `Cargo.lock` adds `basisu` 0.1.0 (Apache-2.0) as a normal dependency of the renderer, with its dependency `ruzstd` 0.7.3 (MIT); `portable-atomic`, its other one, was already in the graph. Both are pure Rust, so they reach the docs site's wasm demo, whose staged crate texts grow from 258 to 260 packages (the hub stager over OxidANT's unchanged policy, run in the CI image: each crate ships its own `LICENSE`, so no policy row is needed). The C++ bridge (`cxx` only) still does not depend on the renderer, so the packaged crate texts are unchanged.
- **2026-11-18 (later)**: OxidANT moved to `0b9e638`, glTF accessor checks and a `load_flat` fuzz property. `LICENSE` and the license policy are unchanged across the range (`git diff` empty); `Cargo.lock` adds only `proptest` (already in the graph) to `kataglyphis_assets`' dev-dependencies, so the staged crates are unchanged.
- **2026-11-18**: OxidANT moved to `0b79367`, raster shadows for point and spot lights in the WebGPU renderer and a perf-host script fix. `LICENSE`, `Cargo.lock` and the license policy are unchanged across the range (`git diff` empty), so the staged crates are too.
- **2026-11-17**: OxidANT moved to `4bff5e1`, adding the `kataglyphis_assets` crate (the CPU glTF/KTX2 loader, MIT, the workspace `LICENSE`) and `load_gltf_flat` to the C++ bridge. `LICENSE` is unchanged across the range (`git diff` empty). The bridge now depends on `kataglyphis_assets`, so its runtime graph grows from 12 to 55 crates (`gltf`, `image`, `png`, `zune-jpeg`, `ktx2`, `basisu`, `ruzstd`, `bevy_mikktspace`, `glam`, `serde_json`, `flate2`, `miniz_oxide` among them); every one was already in OxidANT's reviewed policy, which adds only `kataglyphis_assets@3.0.0`, and the hub's `stage-cargo-licenses.py` stages 129-130 packages for the bridge on every target. `licenses/third-party-licenses.txt` is unchanged.
- **2026-11-17 (later)**: OxidANT moved to `7c81619`. `LICENSE` and `Cargo.lock` are unchanged across the range (`git diff` empty), so the staged crate texts are unchanged.
- **2026-11-17**: OxidANT moved to `fe273c9`. `LICENSE` and `Cargo.lock` are unchanged across the range (`git diff` empty); the root `Cargo.toml` only adds a `profile.dev` opt-level, so the staged crate texts are unchanged.
- **2026-11-16 (late night)**: OxidANT moved to `ae01294`. `LICENSE` is unchanged. `Cargo.lock` replaces `ruzstd` 0.7.3 with 0.9.1 (MIT, its own `LICENSE`; RUSTSEC-2026-0344), now a direct dependency of the renderer, while `basisu` loses its `zstd` feature. The wasm demo's staged texts follow the version; the C++ bridge still does not depend on the renderer, so the packaged crate texts are unchanged.
- **2026-11-16 (night)**: OxidANT moved to `04f7a0e`. `LICENSE`, `Cargo.lock` and every `Cargo.toml` are unchanged across the range, so the staged crate texts are unchanged.
- **2026-11-16 (evening)**: OxidANT moved to `216b0a1`. `LICENSE`, `Cargo.lock` and every `Cargo.toml` are unchanged across the range (`git diff` empty), so the staged crate texts of the bridge and the wasm demo are unchanged.

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
for older hosts; that belongs to ANTfrastructure's closure helpers. For the MSVC runtime DLLs,
every Windows package (ZIP, MSI, NSIS, the portable bundle and the MSIX staged from it) carries
`licenses/msvc-runtime/Redist.txt` and `README.txt` at the install root beside `bin/`, not under
`share/licenses/GraphicsEngine/`. ANTfrastructure's `CPackCommon` installs both and appends the
notice to the MSI licence page, and its `New-PortableBundle` checks the CPack MSI and ZIP for
them; the wording is the hub's draft
([the MSVC runtime notice](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/third-party-licenses.md#the-msvc-runtime-notice)).

## Git submodules under `third_party/` (shipped)

| Project | URL | Pin | Licence (per licence file) | File checked |
|---|---|---|---|---|
| tinyobjloader | https://github.com/tinyobjloader/tinyobjloader | v2.0.0rc10-73-g45636bd | MIT | `third_party/TINY_OBJ_LOADER/LICENSE` |
| glm | https://github.com/g-truc/glm | 6f14f479 | Dual: "The Happy Bunny License or MIT License" | `third_party/GLM/copying.txt` |
| imgui | https://github.com/ocornut/imgui | v1.93.0-72-g8c36ecfb6 | MIT | `third_party/IMGUI/LICENSE.txt` |
| stb | https://github.com/nothings/stb | 9600522d | Public domain, most libraries also MIT (your choice); `stb_image.h` v2.32 and `stb_image_write.h` v1.18 carry the MIT-or-public-domain block | `third_party/STB/LICENSE`; the licence blocks at the end of `third_party/STB/stb_image.h` and `third_party/STB/stb_image_write.h` |
| glfw | https://github.com/glfw/glfw | 3.5.1-1-g92dcf4ce | zlib/libpng licence text | `third_party/GLFW/LICENSE.md` |
| Vulkan Memory Allocator (VMA) | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | v3.5.0 | MIT licence text (Copyright AMD) | `third_party/VULKAN_MEMORY_ALLOCATOR/LICENSE.txt` |
| nlohmann/json | https://github.com/nlohmann/json | v3.11.3-963-g81888ff9f | MIT | `third_party/NLOHMANN_JSON/LICENSE.MIT` |
| google/benchmark | https://github.com/google/benchmark | v1.10.0-28-gcf4d7fe | Apache-2.0 | `third_party/GOOGLE_BENCHMARK/LICENSE` |
| spdlog | https://github.com/gabime/spdlog | v1.17.0-66-g6d9805df | MIT; contains a bundled {fmt} (MIT) | `third_party/SPDLOG/LICENSE`; `third_party/SPDLOG/include/spdlog/fmt/bundled/fmt.license.rst` |
| google/fuzztest | https://github.com/google/fuzztest | fcde2c27 (main, 2026-11-13) | Apache-2.0; an additional Lucent notice for `fuzztest/internal/domains/rune.*` | `third_party/FUZZTEST/LICENSE` |
| kompute (only the optional playground, `KATAGLYPHIS_BUILD_KOMPUTE_PLAYGROUND`) | https://github.com/KomputeProject/kompute | v0.9.0-99-g267e019 | Apache-2.0 | `third_party/KOMPUTE/LICENSE` |
| tomlplusplus | https://github.com/marzer/tomlplusplus | v3.4.0-88-gcfd7159 | MIT | `third_party/tomlplusplus/LICENSE` |

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
| OxidANT | https://github.com/Kataglyphis/OxidANT | 0b9e638 (develop) | MIT (`LICENSE`, Copyright (c) 2025 Kataglyphis; `license = "MIT"` in `[workspace.package]`) | `third_party/OxidANT/LICENSE` + `third_party/OxidANT/Cargo.toml` |
| ANTfrastructure | https://github.com/Kataglyphis/ANTfrastructure | 2c81ef49 (develop) | MIT per `LICENSE` (Copyright (c) 2024 Jonas Heinle); all 589 of its own files with an SPDX header carry `SPDX-License-Identifier: MIT`; OCI label `org.opencontainers.image.licenses="MIT"` in the three image definitions that set one (`linux/Dockerfile.torch`, `windows/Dockerfile`, `windows/Dockerfile.torch`) | `third_party/ANTfrastructure/LICENSE` + SPDX headers + OCI labels |

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

- **Rows updated on 2026-09-25** (a new lock version; log, wgpu, pollster): the
  same `license` field, read in the `Cargo.toml` of the exact version in the
  host's registry cache (`scoop/persist/rustup/.cargo/registry/src`).
- **Rows updated on 2026-11-14** (egui, egui-wgpu and egui-winit 0.37.0): the
  same `license` field, read in the `Cargo.toml` of the exact lock version in
  the registry the CI image's `cargo` filled for the locked workspace (volume
  `bb-cargo-cache`). `egui-winit` is a plain crates.io crate again: OxidANT's
  `[patch.crates-io]` no longer carries it.

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
| egui | 0.37.0 | MIT OR Apache-2.0 | `Cargo.toml` of the exact lock version (container registry, 2026-11-14) |
| egui-wgpu | 0.37.0 | MIT OR Apache-2.0 | `Cargo.toml` of the exact lock version (container registry, 2026-11-14) |
| egui-winit | 0.37.0 | MIT OR Apache-2.0 | `Cargo.toml` of the exact lock version (container registry, 2026-11-14) |
| ktx2 | 0.5.0 | Apache-2.0 | `cargo metadata` (exact lock version, container) |
| image (`png` only) | 0.25.10 | MIT OR Apache-2.0 | `Cargo.toml` of the exact lock version (container registry, 2026-11-15) |
| basisu | 0.1.0 | Apache-2.0 | `Cargo.toml` and `LICENSE` of the exact lock version (container registry, 2026-11-16); built without its `zstd` feature since OxidANT `ae01294`; the renderer inflates Zstandard KTX2 itself with `ruzstd` 0.9.1 (MIT, same check) |

Notes on single entries:

- **winit 0.30.13**, **ktx2 0.5.0** and **basisu 0.1.0** are *not*
  dual-licensed but plain Apache-2.0, unlike the other crates in this table.
- **bevy_mikktspace 1.0.0** is a compound expression: the vendored MikkTSpace C
  code is under Zlib, the Rust part dual MIT/Apache-2.0; both conditions apply
  at once (`AND`).
- **pollster 1.0.1** uses the deprecated slash notation `Apache-2.0/MIT`; it
  means a choice (equivalent to `Apache-2.0 OR MIT`).

## The docs site's WebGPU demo (not in any package)

`scripts/linux/docs-build-web.sh` publishes the Rust renderer as wasm on the docs site, with
third-party works beside it that no package carries:

| Work | Licence | Where its texts go |
|---|---|---|
| Colosseum (`Colosseum-web.glb`, AssetANT release `Colosseum-web-v1`): Colosseum by Sulo456 (https://github.com/Sulo456), converted to glTF by AssetANT | CC BY 4.0 (AssetANT's `assets/Colosseum-web/asset.json`) | the archive's `ATTRIBUTION.txt` and `LICENSES/` (`CC-BY-4.0.txt`, `NOTICE.txt` listing the conversion's changes), copied to `webgpu-demo/Colosseum-web/` |
| Roboto Medium, Cousine Regular (embedded in the wasm by `crates/webgpu_renderer/src/ui/fonts.rs`) | Apache-2.0 and OFL-1.1, per each font's name table | OxidANT's `crates/webgpu_renderer/assets/fonts/{NOTICE,Apache-2.0.txt,OFL-1.1.txt}`, copied to `webgpu-demo/fonts/`; the same texts as `licenses/fonts/` |
| Noto Emoji and emoji-icon-font symbol subsets (`NotoEmoji-Subset.ttf`, `EmojiIcon-Subset.ttf`, embedded by the same `fonts.rs`), cut from egui's `epaint_default_fonts` 0.37.0 | OFL-1.1 (Noto Emoji, a Modified Version keeping its name) and MIT (emoji-icon-font) | the same `NOTICE`, which carries the MIT text, and `OFL-1.1.txt`, copied to `webgpu-demo/fonts/` |

`licenses/third-party-licenses.txt` lists what the packages install, so neither has a row there.
The `Colosseum-gltf` scene, which `KATAGLYPHIS_FETCH_ALL_ASSETS` fetches for the model picker, is
CC BY 4.0 by the same author and ships nowhere either.

## Toolchain (build time, not shipped)

| Tool | Origin | Licence |
|---|---|---|
| slangc (Slang shader compiler) | From the installed Vulkan SDK (`VULKAN_SDK\Bin` or `PATH`), see `scripts/windows/Build-SlangShaders.ps1` / `scripts/linux/compile-slang-shaders.sh`; not vendored in the repository | Toolchain, not shipped; licence not verifiable from the repository |

## Unverified, to check

None open.

Resolved on 2026-11-15:

- **The docs site's wasm demo ships its crates' texts**: `scripts/linux/docs-build-web.sh` stages
  the original licence files of the renderer's 258 `wasm32-unknown-unknown` crates into
  `webgpu-demo/licenses/cargo/` with the hub's `stage-cargo-licenses.py`, since OxidANT's policy
  reviews `bevy_mikktspace@1.0.0` (`Zlib AND (MIT OR Apache-2.0)`, its Zlib notice in `src/lib.rs`).

- **ANTfrastructure, the MIT/Apache contradiction, open since 2026-09-25**: at
  `2c81ef49` its last two files, `.claude/hooks/guard-destructive-deletes.ps1`
  and `.py`, say `SPDX-License-Identifier: MIT` too, so `LICENSE` and all 589
  SPDX headers agree (25 files carried `Apache-2.0` on 2026-09-25, 2 at
  `3c39fac6`).

Resolved on 2026-11-14:

- **egui / egui-wgpu**: the `license` field is read for the exact lock version
  (0.37.0) now, instead of from 0.36.1.

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
