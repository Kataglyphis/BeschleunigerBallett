# WebGPU Demo (Rust, in your browser)

A live demo of the Rust `kataglyphis_webgpu_renderer` crate
(`third_party/OxidANT/crates/webgpu_renderer`),
compiled to WebAssembly and running on the WebGPU API — the same renderer
that runs natively on Vulkan/DX12/Metal. It renders a glTF scene with the
metallic-roughness PBR pipeline, directional shadow mapping (3×3 PCF), and
ACES tonemapping from an HDR target.

Requires a WebGPU-capable browser (Chrome/Edge 113+, Firefox 141+).

<a class="sd-btn sd-btn-primary" href="webgpu-demo/index.html" target="_blank" rel="noopener">
Open the demo full-page ↗
</a>

```{raw} html
<iframe
  src="webgpu-demo/index.html"
  title="Kataglyphis WebGPU glTF demo"
  style="width: 100%; aspect-ratio: 16 / 10; border: 1px solid #444; border-radius: 8px; margin-top: 1rem;"
  allow="fullscreen"
  loading="lazy"></iframe>
```

## About

- **Features shipped**: the renderer is well past the original milestones —
  the authoritative per-feature status (IBL, skybox, animation/skinning/morph
  targets, egui overlay, bloom, SSAO, auto-exposure, LOD, GPU occlusion
  culling, and more) is `webgpu-renderer-roadmap.md` in the repository that owns
  the renderer, under `third_party/OxidANT/crates/webgpu_renderer/docs/`.
- **Rebuild the demo**: `scripts/linux/docs-build-web.sh` compiles
  `kataglyphis_webgpu_renderer` to wasm32, runs `wasm-bindgen`, and refreshes
  this folder before Sphinx runs; the CI docs deploy runs it automatically,
  so the deployed demo always tracks the current renderer.
