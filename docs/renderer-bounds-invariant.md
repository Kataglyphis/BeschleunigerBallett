# The bounds invariant (WebGPU renderer) — moved

**This page moved to OxidANT.** Decision D6 gave OxidANT the WebGPU renderer's
code *and* its documentation, so the page now lives beside the crate it
describes:

- in a checkout of this repository: `third_party/OxidANT/crates/webgpu_renderer/docs/renderer-bounds-invariant.md`
- on the web: <https://github.com/Kataglyphis/OxidANT/blob/HEAD/crates/webgpu_renderer/docs/renderer-bounds-invariant.md>
  (`HEAD`, not a branch name: OxidANT's default branch is being moved from
  `develop` to `main` and this link has to survive the flip)

This file is a pointer, not a copy: there is one source of truth and it is the
one above. The renderer documents that stayed here are
`gpu-golden-testing.md`, `model-loading.md`, `shader-sharing.md` and
`webgpu-srgb-audit.md`.
