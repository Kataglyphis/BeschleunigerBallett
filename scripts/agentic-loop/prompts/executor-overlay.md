# Project: BeschleunigerBallett

The role, the headless-session discipline, the per-task workflow and the generic
rules come from ANTfrastructure's shared Executor prompt, which the loop prepends to
this file automatically
(`third_party/ANTfrastructure/shared/agentic-loop/system-prompts/executor.md`).
Everything below is what is specific to **this** repo.

This is a Vulkan graphics engine: C++23/C17, CMake presets, optional Rust WebGPU
sibling renderer.

## Build commands

Windows — always via the container script, never CMake/Ninja/MSBuild on the host:

```
pwsh -ExecutionPolicy Bypass -File .\scripts\windows\Build-Windows-Container.ps1 -Configurations clangcl-debug
```

Linux:

```
scripts/linux/cmake-configure-build.sh --preset linux-debug-clang --build-dir build
```

When running that inside a container with the repo bind-mounted, pass a
**container-native** build dir instead (`--build-dir /tmp/bb`): CMake's
FetchContent rename and cargo's temp cleanup both fail on the mounted host
filesystem, and the build dies partway through on a stale artifact.

After building in the container you can copy a binary out with `docker cp` if you
need to run it on the host.

## Running tests

The Windows build runs in a container, so its CTest metadata carries container
paths and host `ctest --test-dir` cannot read it. Run the delivered executable
from the **repo root** (cwd matters — some shaders resolve relative to it):

```
./build-clangcl-debug/commitTestSuite.exe --gtest_filter='<Suite>.*'
```

`ctest --test-dir <build-dir> --output-on-failure` is the right call on Linux, or
inside the container via `docker exec`. New tests follow the existing
`Test/commit/` harness pattern; GPU-dependent tests must skip gracefully when no
adapter is present.

## Build configuration guide

| Preset | When to use |
| --- | --- |
| `clangcl-debug` | Fast iteration. ASAN + UBSan enabled. Default for most tasks. |
| `clangcl-profile` | Benchmarks (`perfTestSuite.exe`). Optimized with debug info. |
| `clangcl-release` | Packaging. No sanitizers. |
| `linux-debug-clang` | Linux fast iteration. |
| `linux-debug-tsan-clang` | Race detection (Linux only — no Windows TSan). |

## Code conventions that bite

- Exceptions are disabled project-wide (`/EHs-`, `-fno-exceptions`,
  `VULKAN_HPP_NO_EXCEPTIONS`). **Never add `throw`/`try`.** Use
  `ASSERT_VULKAN(val, "msg")` for creation/allocation calls.
- Graphics pipelines via `PipelineBuilder` — never hand-roll.
- Buffer/image memory via VMA — never raw `vkAllocateMemory`.
- `VulkanBuffer`/`VulkanImage` are move-only with destructor release.

## Shaders

If you touch any shader source, **recompile shaders** before trusting a rendered
measurement. Stale SPIR-V is a known trap — see `docs/shader-build-pipeline.md`.

## Blocked tasks in this repo

The shared `- [b]` protocol applies unchanged. The blocker that comes up most
here is "not reachable from a test": device-loss simulation, driver-specific
behaviour, anything needing a second GPU. Mark those `- [b]` with the reason
rather than faking a test around them.
