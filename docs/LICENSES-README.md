# Lizenzübersicht der Open-Source-Abhängigkeiten

Die folgenden Tabellen listen die im Repository genutzten Open-Source-Abhängigkeiten auf (Stand: 2026-08-02). Verifikation: Diese Liste wurde gegen `git submodule status`, `third_party/CMakeLists.txt` (FetchContent), die gevendorten Quellen unter `third_party/` sowie `third_party/OxidANT/crates/webgpu_renderer/Cargo.toml` abgeglichen; jede Lizenzangabe stammt aus der jeweils zitierten, auf der Festplatte gelesenen Lizenzdatei bzw. dem `license`-Feld der Cargo-Metadaten — nicht aus Annahmen. Für rechtsverbindliche Angaben ist immer die zitierte Lizenzdatei selbst maßgeblich.

## Git-Submodule unter `third_party/` (mitgeliefert)

| Projekt | URL | Pin | Lizenz (laut Lizenzdatei) | Geprüfte Datei |
|---|---|---|---|---|
| tinyobjloader | https://github.com/tinyobjloader/tinyobjloader | v2.0.0rc10-73-g45636bd | MIT | `third_party/TINY_OBJ_LOADER/LICENSE` |
| glm | https://github.com/g-truc/glm | 6f14f479 | Dual: "The Happy Bunny License or MIT License" | `third_party/GLM/copying.txt` |
| imgui | https://github.com/ocornut/imgui | v1.92.9b-1-g9b7699f32 | MIT | `third_party/IMGUI/LICENSE.txt` |
| stb | https://github.com/nothings/stb | 2c980bb5 | Dual: MIT oder Public Domain (nach Wahl) | `third_party/STB/LICENSE` |
| glfw | https://github.com/glfw/glfw | 3.5.1 (d9d6f0f1) | Zlib/libpng-Lizenztext | `third_party/GLFW/LICENSE.md` |
| Vulkan Memory Allocator (VMA) | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | 3aa92122 | MIT-Lizenztext (Copyright AMD) | `third_party/VULKAN_MEMORY_ALLOCATOR/LICENSE.txt` |
| nlohmann/json | https://github.com/nlohmann/json | v3.11.3-505-gb17c272f4 | MIT | `third_party/NLOHMANN_JSON/LICENSE.MIT` |
| google/benchmark | https://github.com/google/benchmark | v1.9.5-122-g04b5f41 | Apache-2.0 | `third_party/GOOGLE_BENCHMARK/LICENSE` |
| spdlog | https://github.com/gabime/spdlog | v1.17.0-44-g57cb5fb7 | MIT; enthält gebündeltes {fmt} (MIT) | `third_party/SPDLOG/LICENSE`; `third_party/SPDLOG/include/spdlog/fmt/bundled/fmt.license.rst` |
| google/fuzztest | https://github.com/google/fuzztest | 704efb34 (2026-06-29) | Apache-2.0; zusätzliche Lucent-Notiz für `fuzztest/internal/domains/rune.*` | `third_party/FUZZTEST/LICENSE` |
| kompute (nur optionales Playground, `KATAGLYPHIS_BUILD_KOMPUTE_PLAYGROUND`) | https://github.com/KomputeProject/kompute | v0.9.0-99-g267e019 | Apache-2.0 | `third_party/KOMPUTE/LICENSE` |
| cgltf | https://github.com/jkuhlmann/cgltf | v1.15-11-g85cd623 | MIT-Lizenztext (Copyright Johannes Kuhlmann) | `third_party/cgltf/LICENSE` |
| tomlplusplus | https://github.com/marzer/tomlplusplus | v3.4.0-50-g1e8829b | MIT | `third_party/tomlplusplus/LICENSE` |

## Gevendorte Quellen (kein Submodul)

| Projekt | URL | Lizenz (laut Datei-Headern) | Geprüfte Datei(en) |
|---|---|---|---|
| KTX (KTX-Software, Teil-Vendoring: `include/`, `lib/`, `other_include/`) | https://github.com/KhronosGroup/KTX-Software | Apache-2.0 (SPDX-Header in `include/ktx.h` und in 98 Dateien unter `lib/`) | `third_party/KTX/include/ktx.h` |

Hinweis KTX: Die vollständige `LICENSE.md` von KTX-Software ist **nicht** mitvendort
(`third_party/KTX/LICENSE.md` existiert nicht). Die eingebetteten
Third-Party-Komponenten unter `third_party/KTX/lib/` wurden deshalb einzeln aus den
Dateien auf der Festplatte verifiziert — teils aus mitvendorten Lizenzdateien, teils
aus dem Lizenzkopf der Quelldatei selbst:

| Komponente | Lizenz | Geprüfte Datei (jeweils unter `third_party/KTX/lib/`) |
|---|---|---|
| basisu (Encoder + Transcoder) | Apache-2.0 (Copyright Binomial LLC) | `basisu/LICENSE` (voller Apache-2.0-Text); Lizenzkopf in `basisu/encoder/basisu_enc.cpp` und `basisu/transcoder/basisu_transcoder.cpp`. Die REUSE-Texte `basisu/LICENSES/{Apache-2.0,BSD,Zlib}.txt` liegen für die Unterkomponenten bei. |
| zstd (in basisu gebündelt) | BSD-3-Clause; der Dateikopf nennt zusätzlich GPLv2 zur Wahl ("You may select, at your option, one of the above-listed licenses") | `basisu/zstd/LICENSE` (BSD-Text, Copyright Facebook, Inc.); Kopf von `basisu/zstd/zstd.c` |
| LodePNG | Zlib-Lizenztext (Copyright 2005-2019 Lode Vandevenne) | Lizenzkopf in `basisu/encoder/lodepng.h` |
| jpgd | Public Domain (Rich Geldreich) | Lizenzkopf in `basisu/encoder/jpgd.h` |
| stb_image / stb_image_write | Dual: MIT ("ALTERNATIVE A") oder Public Domain/Unlicense ("ALTERNATIVE B"), nach Wahl | Lizenzblock am Dateiende von `astc-encoder/Source/stb_image.h` und `astc-encoder/Source/stb_image_write.h` |
| tinyexr | BSD-3-Clause (Copyright 2014-2019 Syoyo Fujita und Beitragende) | Lizenzkopf in `astc-encoder/Source/tinyexr.h` |
| etcdec.cxx | **Keine OSI-Lizenz**: "Ericsson Texture Compression Codec Software License Agreement" — eine nicht-exklusive, nicht übertragbare, kostenlose, unbefristete, weltweite Einzellizenz mit eigenen Bedingungen | Lizenzkopf in `etcdec.cxx` |
| astc-encoder (Arm) | Apache-2.0 | SPDX-Header `// SPDX-License-Identifier: Apache-2.0` in `astc-encoder/Source/astcenc.h`. Hinweis: die in `astc-encoder/README.md` referenzierte `LICENSE.txt` ist nicht mitvendort. |
| dfdutils (Khronos) | Apache-2.0, einzelne aus MIT-Projekten übernommene Dateien MIT | `dfdutils/LICENSE.adoc`; Volltexte in `dfdutils/LICENSES/{Apache-2.0,MIT}.txt` |

Zu beachten: `etcdec.cxx` ist die einzige Komponente im gesamten Baum, die nicht
unter einer OSI-anerkannten Lizenz steht. Für Redistribution ist der Wortlaut der
SLA im Dateikopf maßgeblich.

## Build-Zeit-Abhängigkeiten via FetchContent (`third_party/CMakeLists.txt`, nicht im Repo eingecheckt)

Lizenz jeweils aus der LICENSE-Datei des lokalen FetchContent-Checkouts unter `build*/\_deps/` gelesen.

| Projekt | URL | Pin | Lizenz (laut Lizenzdatei) | Geprüfte Datei |
|---|---|---|---|---|
| abseil-cpp | https://github.com/abseil/abseil-cpp | 20260526.0 (`ABSL_TAG`) | Apache-2.0 | `build-clangcl-profile/_deps/abseil-cpp-src/LICENSE` |
| googletest (nur `BUILD_TESTING`) | https://github.com/google/googletest | 56efe398 (URL-Pin) | BSD-3-Clause-Lizenztext (Copyright Google Inc.) | `build-clangcl-profile/_deps/googletest-src/LICENSE` |
| Microsoft GSL | https://github.com/microsoft/GSL | v4.2.1 | MIT | `build-clangcl-profile/_deps/gsl-src/LICENSE` |
| Corrosion (nur `RUST_FEATURES`, Build-Tool) | https://github.com/corrosion-rs/corrosion | master | MIT | `build-clangcl-profile/_deps/corrosion-src/LICENSE` |
| ANTLR4 C++ Runtime (transitiv via FuzzTest) | https://github.com/antlr/antlr4 | von FuzzTest gepinnt | BSD-3-Clause-Lizenztext (The ANTLR Project) | `build-linux-local/_deps/antlr_cpp-src/LICENSE.txt` |
| RE2 (transitiv via FuzzTest) | https://github.com/google/re2 | von FuzzTest gepinnt | BSD-3-Clause-Lizenztext (The RE2 Authors) | `build-linux-local/_deps/re2-src/LICENSE` |

## Interne / projekt-spezifische Submodule

| Projekt | URL | Pin | Lizenz | Geprüfte Quelle |
|---|---|---|---|---|
| OxidANT | https://github.com/Kataglyphis/OxidANT | 49e8ee24 (develop) | MIT (`license = "MIT"` in `[workspace.package]`; keine LICENSE-Datei im Submodul) | `third_party/OxidANT/Cargo.toml` |
| ContainerHub | https://github.com/Kataglyphis/ContainerHub | 6d5b1af5 (main) | MIT — `LICENSE` (Copyright (c) 2024 Jonas Heinle), `SPDX-License-Identifier: MIT` in allen 101 eigenen Quelldateien, OCI-Label `org.opencontainers.image.licenses="MIT"` in allen drei Image-Definitionen | `third_party/ContainerHub/LICENSE` + SPDX-Header + OCI-Labels |

## Rust-Crate-Abhängigkeiten (`third_party/OxidANT/crates/webgpu_renderer/Cargo.toml`, `[dependencies]`)

Versionen laut `third_party/OxidANT/Cargo.lock`.

**Quelle der Lizenzangaben (alle Zeilen dieser Tabelle):** das `license`-Feld der
Cargo-Metadaten — also genau das Feld, das die Crate auf crates.io veröffentlicht
hat — für die exakte Version aus `Cargo.lock`, gelesen am 2026-08-02 im
`:latest-cross`-Container (dort liegt die vollständige Registry; der Host-Cache
ist unvollständig, weil die Crate-Builds im Container laufen) mit:

```sh
cargo metadata --format-version 1 --locked   # CARGO_HOME=/cargo-cache
```

Für jedes Paket wurde zusätzlich `source` geprüft; alle stehen auf
`registry+https://github.com/rust-lang/crates.io-index`. Kein Eintrag stammt aus
einem README, einer benachbarten Version oder einer Vermutung. Die Schreibweise
der Lizenzausdrücke ist unverändert aus dem Metadatenfeld übernommen (daher z. B.
das alte Trennzeichen bei `pollster`).

| Crate | Version (Lock) | Lizenz (`license`-Feld) | Geprüfte Quelle |
|---|---|---|---|
| anyhow | 1.0.104 | MIT OR Apache-2.0 | `cargo metadata` (exakte Lock-Version, Container) |
| log | 0.4.33 | MIT OR Apache-2.0 | `cargo metadata` (exakte Lock-Version, Container) |
| bytemuck | 1.25.2 | Zlib OR Apache-2.0 OR MIT | `cargo metadata` (exakte Lock-Version, Container) |
| env_logger | 0.11.11 | MIT OR Apache-2.0 | `cargo metadata` (exakte Lock-Version, Container) |
| wgpu | 29.0.4 | MIT OR Apache-2.0 | `cargo metadata` (exakte Lock-Version, Container) |
| winit | 0.30.13 | Apache-2.0 | `cargo metadata` (exakte Lock-Version, Container) |
| pollster | 0.4.0 | Apache-2.0/MIT | `cargo metadata` (exakte Lock-Version, Container) |
| glam | 0.30.10 | MIT OR Apache-2.0 | `cargo metadata` (exakte Lock-Version, Container) |
| gltf | 1.4.1 | MIT OR Apache-2.0 | `cargo metadata` (exakte Lock-Version, Container) |
| bevy_mikktspace | 1.0.0 | Zlib AND (MIT OR Apache-2.0) | `cargo metadata` (exakte Lock-Version, Container) |
| egui | 0.35.0 | MIT OR Apache-2.0 | `cargo metadata` (exakte Lock-Version, Container) |
| egui-wgpu | 0.35.0 | MIT OR Apache-2.0 | `cargo metadata` (exakte Lock-Version, Container) |
| egui-winit | 0.35.0 | MIT OR Apache-2.0 | `cargo metadata` (exakte Lock-Version, Container) |
| ktx2 | 0.5.0 | Apache-2.0 | `cargo metadata` (exakte Lock-Version, Container) |

Hinweise zu einzelnen Einträgen:

- **winit 0.30.13** und **ktx2 0.5.0** sind *nicht* dual lizenziert, sondern reines
  Apache-2.0 — anders als die übrigen Crates dieser Tabelle.
- **bevy_mikktspace 1.0.0** ist ein zusammengesetzter Ausdruck: der gevendorte
  MikkTSpace-C-Code steht unter Zlib, der Rust-Anteil dual MIT/Apache-2.0; beide
  Bedingungen gelten gleichzeitig (`AND`).
- **pollster 0.4.0** verwendet die veraltete Slash-Schreibweise `Apache-2.0/MIT`;
  gemeint ist eine Wahlmöglichkeit (entspricht `Apache-2.0 OR MIT`).

## Toolchain (Build-Zeit, nicht mit ausgeliefert)

| Tool | Herkunft | Lizenz |
|---|---|---|
| slangc (Slang-Shader-Compiler) | Aus dem installierten Vulkan SDK (`VULKAN_SDK\Bin` bzw. `PATH`), siehe `scripts/windows/Build-SlangShaders.ps1` / `scripts/linux/compile-slang-shaders.sh`; nicht im Repo gevendort | Toolchain, nicht mitgeliefert; Lizenz nicht aus dem Repo verifizierbar |

## Unverifiziert — zu prüfen

Derzeit keine offenen Punkte.

Am 2026-08-07 erledigt und daher aus dieser Liste entfernt:

- **ContainerHub, fehlende LICENSE**: das Submodul führt inzwischen
  eine `LICENSE`-Datei; der Pin oben ist zudem von `8687f0c7` auf den aktuellen
  Stand `6d5b1af5` korrigiert.
- **ContainerHub, MIT/Apache-Widerspruch**: aufgelöst zugunsten von
  **MIT**. Zuvor stand in `LICENSE` MIT, während 101 eigene Quelldateien
  `SPDX-License-Identifier: Apache-2.0` trugen und zwei der drei Image-Definitionen
  das OCI-Label `Apache-2.0` setzten. Alle drei Stellen sagen jetzt MIT; die
  MIT-widersprüchliche Formel „All rights reserved." wurde aus den Headern
  entfernt. Nicht angefasst: `linux/webserver/dist/assets/NOTICES` und
  `docs/deps/deps.json` — das sind Fremdlizenzangaben (u. a. Apache 2.0 für
  LLVM, TVM, OpenCV, Vulkan SDK) und bleiben inhaltlich korrekt.

Am 2026-08-02 erledigt und daher aus dieser Liste entfernt:

- Die 11 zuvor unverifizierten Rust-Crates (env_logger, wgpu, winit, pollster,
  glam, gltf, bevy_mikktspace, egui, egui-wgpu, egui-winit, ktx2) sind jetzt aus
  `cargo metadata` im Container verifiziert — siehe Crate-Tabelle oben.
- `anyhow`, `log` und `bytemuck` sind jetzt für die **exakte** Lock-Version
  belegt statt für eine benachbarte gecachte Version; die Lizenzausdrücke sind
  dabei unverändert geblieben.
- Die eingebetteten Third-Party-Anteile in `third_party/KTX/lib/` sind einzeln
  aus den Dateien auf der Festplatte belegt — siehe die Tabelle im Abschnitt
  "Gevendorte Quellen".

## Hinweise

- Entfernt gegenüber Stand 2026-03-26: **glad** (kein `third_party/glad`-Submodul mehr vorhanden, keine glad-/OpenGL-Loader-Referenzen unter `Src/`) und **KTX als Submodul** (jetzt Teil-Vendoring, siehe oben).
- Kompute wird nur mit `KATAGLYPHIS_BUILD_KOMPUTE_PLAYGROUND=ON` gebaut (Demo, nicht Teil der Engine).
- Die `build*/_deps/`-Pfade sind lokale Checkouts (nicht eingecheckt); sie dokumentieren, welche Datei bei der Verifikation tatsächlich gelesen wurde. Für die Rust-Crates tritt an diese Stelle die Registry im `:latest-cross`-Container (Volume `cargo-cache`, `CARGO_HOME=/cargo-cache`), aus der `cargo metadata` das `license`-Feld liest.

---
Erstellt automatisch im Repository auf Anforderung; zuletzt vollständig gegen den Datenträger verifiziert am 2026-08-02 — die Rust-Crate-Lizenzen zusätzlich gegen `cargo metadata --format-version 1 --locked` im `:latest-cross`-Container (vollständige crates.io-Registry) am 2026-08-02.
