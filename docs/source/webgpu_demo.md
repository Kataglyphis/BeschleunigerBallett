# WebGPU Demo (Rust, in your browser)

The Rust `kataglyphis_webgpu_renderer` crate (`third_party/OxidANT/crates/webgpu_renderer`)
compiled to WebAssembly on the WebGPU API: the same renderer and editor that run natively on
Vulkan/DX12/Metal. It starts on an embedded cube; the scene picker switches to the other embedded
showcase scenes or fetches the Colosseum (58 MiB), and a self-contained `.glb` dropped on the page
loads too.

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

- **Features shipped**: well past the original milestones. The authoritative per-feature status (IBL,
  skybox, animation/skinning/morph targets, egui overlay, bloom, SSAO, auto-exposure, LOD, GPU
  occlusion culling, and more) is `webgpu-renderer-roadmap.md` in the renderer's own repository,
  under `third_party/OxidANT/crates/webgpu_renderer/docs/`.
- **Rebuild the demo**: `scripts/linux/docs-build-web.sh` compiles `kataglyphis_webgpu_renderer`
  to wasm32, runs `wasm-bindgen` and writes the demo (untracked) next to these pages before
  Sphinx runs. CI's docs build runs it, so the published demo tracks the current renderer.
- **The Colosseum** is AssetANT's `Colosseum-web` release, pinned in `Resources/assets.lock`. The
  docs build fetches it with the hub's fetcher and puts `Colosseum-web.glb` beside the demo's page,
  where the picker loads it from; without the archive the demo publishes without it, with a
  warning. Colosseum by [Sulo456](https://github.com/Sulo456), CC BY 4.0, converted to glTF by
  AssetANT; its `ATTRIBUTION.txt` and `LICENSES/` sit in `webgpu-demo/Colosseum-web/`.
- **Fonts**: the editor embeds Dear ImGui's Roboto Medium (Apache-2.0) and Cousine (OFL-1.1); their
  notice and texts sit in `webgpu-demo/fonts/`.
- **Crates**: the wasm is the renderer's crate graph for `wasm32-unknown-unknown`. The hub's
  `stage-cargo-licenses.py` stages each crate's original texts, reviewed by OxidANT's
  `packaging/cargo-license-policy.json`, into `webgpu-demo/licenses/cargo/` with a `manifest.json`;
  a crate the policy has not reviewed stops the stager, and the docs then publish without the demo.
