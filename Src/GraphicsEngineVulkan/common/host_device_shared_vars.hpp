#ifndef HOST_DEVICE_SHARED_VARS
#define HOST_DEVICE_SHARED_VARS

// Hand-mirrored by Resources/ShadersSlang/common/scene_types.slang.

const int MAX_TEXTURE_COUNT = 128;
const int MAX_CASCADES = 3;
// Past 5, +1 moves under 2% of near-shadow pixels while taps grow as (2r+1)^2 (docs/source/gpu-golden-testing.md).
const int MAX_PCF_RADIUS = 5;

// ----- MAIN RENDER DESCRIPTOR SET ----- START (rasterizer and raytracer)
#define globalUBO_BINDING 0
#define sceneUBO_BINDING 1
#define OBJECT_DESCRIPTION_BINDING 2
#define TEXTURES_BINDING 3
#define SAMPLER_BINDING 4
#define SHADOW_MAP_BINDING 5
// KHR_lights_punctual lights: a count row, then four rows per light (punctual_light.slang's layout).
#define PUNCTUAL_LIGHTS_BINDING 6
// ----- MAIN RENDER DESCRIPTOR SET ----- END

// ---- RAYTRACING BINDING ---- START
#define TLAS_BINDING 0
#define OUT_IMAGE_BINDING 1
// Path-tracing history: one per renderer, not per swapchain image, as it persists across frames.
#define ACCUMULATION_IMAGE_BINDING 2
// Primary-hit NDC depth of a ray- or path-traced frame, which the clouds read in place of a raster depth buffer.
#define TRACED_DEPTH_IMAGE_BINDING 3
// The skybox's cube map, which a path-traced bounce that misses samples as the sky it draws.
#define SKYBOX_CUBEMAP_BINDING 4
// ---- RAYTRACING BINDING ---- END

// ----- GBUFFER TEXTURE SET ----- START (sampled by the deferred lighting pass)
#define GBUFFER_NORMAL_BINDING 0
#define GBUFFER_ALBEDO_BINDING 1
#define GBUFFER_MATERIAL_BINDING 2
#define GBUFFER_DEPTH_BINDING 3
// ----- GBUFFER TEXTURE SET ----- END

#endif