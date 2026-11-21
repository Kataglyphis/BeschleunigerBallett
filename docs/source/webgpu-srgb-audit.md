# WebGPU Renderer — sRGB/Linear Audit

Phase A closing item: every texture and attachment in
`crates/webgpu_renderer`, its color space, and why. Rule of thumb: *color*
data is sRGB-encoded and decoded by the hardware on sample; *data* maps and
all render math stay linear. The last row is the C++/Vulkan engine's
swapchain, added 2026-08-04 because it had the web canvas's bug.

| Resource | Format | Space | Rationale |
| --- | --- | --- | --- |
| Base color texture | `Rgba8UnormSrgb` | sRGB | Authored color; hardware decode on sample |
| Emissive texture | `Rgba8UnormSrgb` | sRGB | Authored color |
| Normal map | `Rgba8Unorm` | linear | Vector data; `2n−1` decode in shader |
| Metallic-roughness | `Rgba8Unorm` | linear | Parameter data (B=metallic, G=roughness) |
| Occlusion | `Rgba8Unorm` | linear | Scalar data (R) |
| White / flat-normal fallbacks | `Rgba8Unorm` | linear | 255 encodes 1.0 identically in both spaces; normals must be linear |
| Mip generation | CPU box filter | mixed | sRGB textures averaged in linear space (decode→avg→encode); data maps averaged raw; alpha always linear |
| HDR scene target | `Rgba16Float` | linear | All lighting math is linear; values exceed 1.0 by design |
| Bloom chain (A/B) | `Rgba16Float` | linear | Operates on HDR energy above threshold |
| Shadow map | `Depth32Float` | n/a | Depth |
| Swapchain (native) | first sRGB format from surface caps | sRGB | Tonemap writes linear; hardware encodes |
| Swapchain (web) | browser-preferred (often `Bgra8Unorm`, non-sRGB) | sRGB | WebGPU canvases don't offer sRGB formats, so the tonemap shader applies the IEC 61966-2-1 transfer function itself when the target is non-sRGB (`TonemapPass::encode_srgb`, `params.w`). Fixed 2026-07-20 |
| Headless readback target | `Rgba8UnormSrgb` | sRGB | Golden tests assert sRGB-encoded bytes (documented in `tests/headless.rs`) |
| egui overlay | surface format | matches target | egui-wgpu handles its own color management |
| C++/Vulkan swapchain | UNORM (`SwapchainChoices.hpp::chooseBestSurfaceFormat` prefers `eR8G8B8A8Unorm`/`eB8G8R8A8Unorm`) | sRGB | Same bug as the web canvas case: no hardware encode on present, so `post/post.slang`, the only shader that writes the swapchain directly (the sky goes into the HDR target since 2026-11-21), applies `linear_to_srgb` itself. Fixed 2026-08-04 |

**No known deviations**: both swapchain caveats are closed (web 2026-07-20,
C++/Vulkan 2026-08-04).

**The exact piecewise transfer function, not `pow(x, 1/2.2)`.** The
approximation is visibly wrong in the darks and would make the web build differ
from native, the class of bug this table exists to prevent. The web encode is
guarded by `non_srgb_target_is_gamma_encoded_like_an_srgb_one`
(`crates/webgpu_renderer/tests/headless.rs`): it renders one scene to an sRGB
and a non-sRGB target and asserts their mean byte values agree within 2
levels. Without the encode it fails at 177.17 vs 127.77, a 49-level gap: the
"slightly dark on web" symptom, quantified.

**KTX2 container vs. material usage.** glTF usage decides a block-compressed
texture's GPU format (base colour/emissive sRGB; normal/metallic-roughness/
occlusion linear), never the KTX2 container's declared vkFormat. When a KTX2's
`*_SRGB_BLOCK`/`*_UNORM_BLOCK` vkFormat disagrees with the material's use,
`create_compressed_texture` (`render/texture.rs`) logs a `log::warn!` naming
the texture, the declared space and the used one; usage still wins.
