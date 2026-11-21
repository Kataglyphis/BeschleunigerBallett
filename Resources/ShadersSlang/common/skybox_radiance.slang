// The skybox's linear radiance per direction for the path tracer's bounces; the drawn sky is OxidANT's sky.slang.

// Linear navy shown while the skybox is switched off: sky.slang's SKYBOX_DISABLED.
static const float3 SKYBOX_DISABLED_RADIANCE = float3(0.0, 0.0, 0.5);

// Explicit LOD for compute, which has no derivatives: the path tracer's bounce that misses.
float3 skybox_radiance_lod0(SamplerCube cubemap, float3 direction, bool enabled)
{
    return enabled ? cubemap.SampleLevel(normalize(direction), 0.0).xyz : SKYBOX_DISABLED_RADIANCE;
}
