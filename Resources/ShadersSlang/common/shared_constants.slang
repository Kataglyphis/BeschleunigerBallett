// Limits and binding slots both compilers read: C++ includes this file, so only `static const int NAME = N;` lines.

// The cascade count and PCF radius both renderers share are OxidANT's limits.slang.

static const int MAX_TEXTURE_COUNT = 128;

// Main render descriptor set (rasterizer and raytracer).
static const int globalUBO_BINDING = 0;
static const int sceneUBO_BINDING = 1;
static const int OBJECT_DESCRIPTION_BINDING = 2;
static const int TEXTURES_BINDING = 3;
static const int SAMPLER_BINDING = 4;
static const int SHADOW_MAP_BINDING = 5;
// KHR_lights_punctual lights: a count row, then four rows per light (punctual_light.slang's layout).
static const int PUNCTUAL_LIGHTS_BINDING = 6;
// Point and spot light shadows: a cube-array depth map, one cube per shadowed light.
static const int PUNCTUAL_SHADOW_MAP_BINDING = 7;
// The skybox's baked environment (AmbientMaps): irradiance and prefiltered cubes, then the split sum's table.
static const int IBL_IRRADIANCE_BINDING = 8;
static const int IBL_PREFILTERED_BINDING = 9;
static const int IBL_BRDF_LUT_BINDING = 10;

// Ray-tracing set (ray- and path-traced shaders only).
static const int TLAS_BINDING = 0;
static const int OUT_IMAGE_BINDING = 1;
// Path-tracing history: one per renderer, not per swapchain image, as it persists across frames.
static const int ACCUMULATION_IMAGE_BINDING = 2;
// Primary-hit NDC depth of a ray- or path-traced frame, which the clouds read in place of a raster depth buffer.
static const int TRACED_DEPTH_IMAGE_BINDING = 3;
// The skybox's cube map, which a path-traced bounce that misses samples as the sky it draws.
static const int SKYBOX_CUBEMAP_BINDING = 4;

// G-buffer textures (set 1), sampled by the deferred lighting pass.
static const int GBUFFER_NORMAL_BINDING = 0;
static const int GBUFFER_ALBEDO_BINDING = 1;
static const int GBUFFER_MATERIAL_BINDING = 2;
static const int GBUFFER_DEPTH_BINDING = 3;
