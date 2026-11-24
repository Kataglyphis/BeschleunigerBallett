# Container Build Caching & Incremental Builds

How builds in *this repo* stay fast; `AGENTS.md` links here. The mechanism, its safety rails and
the traps behind them are the hub's:
[`windows-container-build-performance.md`](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/windows-container-build-performance.md).

## Measured here (2026-07-19, `clangcl-debug`)

| Scenario | Build step | Wall clock |
| --- | --- | --- |
| Fresh container (`-FreshContainer`, host tree seeded) | 327.9 s | 364 s |
| Incremental, a few files changed | ~14 s | ~50 s |
| **No source changes** | **9.6 s** | **44 s** |
| Previously: fresh container every build | — | 352–484 s |

The wall clock exceeds the build step because sources stream in and artifacts
stream back out on every run; that transport, not compilation, is now the
dominant term. The incrementally built binaries passed 21/21 commit tests and
18/18 Pester tests at the time.

**The tar-pipe is still the right trade, and the script's default.** A bind mount removes the
transport but measured **slower** (2026-07-19): 32.7 s ninja / 159 s wall for the same no-change
build, the build tree then sitting on the Dev Drive. `-UseBindMount` opts in; measure before
switching
([why, and when it inverts](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/windows-container-build-performance.md#why-the-bind-mount-lost-here)).

## What is actually cached

**Build state, not compiler output.** One reused container,
`bb-build-persistent`, keeps ninja's dependency graph, the C++23 module BMIs
and the object files where they are. sccache is wired to a persistent volume
but contributes nothing here (0.00 % hit rate on a modules build; the
measurement is in ANTfrastructure).

## Repo-specific wiring

- **`KATAGLYPHIS_KEEP_BUILD_ROOT=1`** is passed into the container; without it
  `Invoke-CmakeConfigureAndBuild` wipes the build root before configuring
  (`Remove-BuildRootSafe`). Nothing sets it on the host, so host builds are
  unaffected.
- **The `cargo/` subtree is excluded in both directions.** The Rust bridge
  (corrosion/cxx) generates cxxbridge paths past the Windows path limit, and
  one such failure aborts the *whole* tar transfer; the exclusion turned a
  partial transfer into a working one. Cargo artifacts rebuild cheaply.
- **Outbound copies only what the host runs**: executables, debug info,
  `compile_commands.json`, logs, selected by exclusion.

## Delivery verification

`Build-Windows-Container.ps1` compares the executables in the container with those that reached the
host and **fails the build** if the container produced none or one did not arrive; both halves
once failed silently here
([safety rails](https://github.com/Kataglyphis/ANTfrastructure/blob/develop/docs/windows-container-build-performance.md#safety-rails-this-needs)).
Do not "fix" that check.

## Gotchas

- The script takes `-Configurations` (comma-separated), **not** `-Preset`. An
  unknown parameter such as `-Preset` fails parameter binding before anything
  builds. Without `-Configurations` it builds all three of
  `clangcl-debug,clangcl-profile,clangcl-release`.
- Host `cmake` (3.29) cannot read this repo's `CMakePresets.json`
  (`version: 10`); only the container's newer CMake can.
- **A reused container is pruned before the sources stream in**: every top-level directory under
  `C:\ws` except `build*`, `logs` and `sccache-local` goes. Top-level files are only overwritten,
  so a file deleted at the repo root still needs `-FreshContainer`, as does any C++23
  module-interface change (the fresh-container rule in
  [`gpu-golden-testing.md`](gpu-golden-testing.md)).
- Reset an incremental build that behaves strangely:
  `docker rm -f bb-build-persistent`, or
  `Remove-Item -Recurse -Force build-clangcl-debug` for a cold but clean host
  tree.
