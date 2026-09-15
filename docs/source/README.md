# Project Overview

BeschleunigerBallett is a renderer and graphics-engine playground for experimenting with Vulkan, a companion Rust WebGPU renderer, modern CMake, testing, packaging, and optional Rust integration. This documentation focuses on the build and maintenance workflow around the engine, not only on the rendering features themselves.

## What is in Scope?

- Vulkan rendering with rasterization, ray tracing, path tracing, PBR, OBJ loading, and mip mapping
- A companion Rust WebGPU renderer (native and browser) sharing Slang shader sources with the Vulkan engine
- Tooling around CMake presets, CI, coverage, benchmarks, fuzzing, packaging, Sphinx, Doxygen, and Graphviz
- Linux and Windows as the primary development platforms

## Repository Map

| Path | Purpose |
| --- | --- |
| `Src/` | Engine and renderer source code |
| `Resources/` | Shaders and runtime assets |
| `scripts/linux/` | Linux build, test, and docs automation |
| `scripts/windows/` | Windows build, run, and dependency helpers |
| `Test/` | Tests |
| `docs/` | Engineering docs: roadmaps, audits, pipeline notes; see the Docs table in `AGENTS.md` for the full inventory |
| `docs/source/` | Hand-written Sphinx pages |
| `Documents/` | Generated PDF and other reference artifacts |
| `third_party/` | Third-party dependencies and submodules |

## Supported Environments

- C++23 and C17 toolchains
- CMake 4.1 or newer
- Vulkan SDK 1.4 compatible environment for Vulkan builds
- Optional Rust toolchain for the experimental Rust-enabled build path

## Where to Go Next

- Read [Getting Started](getting_started.md) for clone, configure, build, and run instructions
- Read [Documentation Workflow](documentation_workflow.md) for Sphinx, Doxygen, and Graphviz details
- Open **Graphviz Include Graphs** (`graphviz_files.rst`) for the stable entry page for generated diagrams
- Open **API Reference** (`api/library_root.rst`) for the C++ API landing page

The last two pages are WRITTEN BY THE DOCS BUILD - `graphviz_generator.py`, and
exhale or the placeholder `conf.py` writes when no Doxygen XML is present - so
they exist in a built tree, not in the checkout. `index.rst` links them there,
which is why they are named here rather than linked.

If Doxygen XML has already been generated, the API page is automatically replaced with the generated Exhale output.