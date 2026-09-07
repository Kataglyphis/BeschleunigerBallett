# Project: BeschleunigerBallett

The role, the critical rules, the refactor focus areas and the task-entry
template come from ContainerHub's shared Planner prompt, which the loop prepends
to this file automatically
(`third_party/ContainerHub/shared/agentic-loop/system-prompts/planner.md`).
Everything below is what is specific to **this** repo.

This is a Vulkan graphics engine: C++23/C17, CMake presets, optional Rust WebGPU
sibling renderer.

## BACKLOG sections

Place tasks under the appropriate existing heading — `## C++ Vulkan engine`,
`## Rust WebGPU renderer`, and the others already in the file.

## Which build to specify

- `clangcl-debug` — fast iteration (ASAN + UBSan). The default for most tasks.
- `clangcl-profile` — benchmarks (`perfTestSuite.exe`).
- `clangcl-release` — packaging.

The command to put in a task's **Build:** field:

```
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1 -Configurations clangcl-debug
```

## Test guidance a task must carry

Name the test in the task's **Test:** field and point at the existing
`Test/commit/` harness pattern; a GPU-dependent test must skip gracefully when
no adapter is present. File paths in **Files to read:** are repo-relative —
`Src/...` for engine sources, `Test/...` for the harness.

## Refactor focus: C++23 modernization

On top of the shared refactor focus areas, this repo's "language modernization"
means concretely:

- `std::span` where raw pointer + length pairs are still passed around,
- `std::expected` where a function returns an error code (remember exceptions
  are disabled, so this is the error channel),
- `constexpr` / `consteval` where a computation is knowable at compile time.

## Conventions a task must never violate

Read `AGENTS.md`. In particular, do not propose enabling exceptions
(the project compiles with `-fno-exceptions` / `/EHs-` and
`VULKAN_HPP_NO_EXCEPTIONS`), hand-rolling Vulkan pipelines instead of
`PipelineBuilder`, or bypassing VMA for buffer/image memory.

## Docs to reference rather than restate

`docs/cpp-renderer-improvements.md` for engine change history,
`docs/shader-build-pipeline.md` for anything shader-related,
`docs/gpu-golden-testing.md` for render-output verification.
