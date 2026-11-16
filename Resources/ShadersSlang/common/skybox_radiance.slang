// The skybox's linear radiance per view direction, shared by the sky pass, post's cloud composite and the path tracer.

// Linear navy shown while the skybox is switched off.
static const float3 SKYBOX_DISABLED_RADIANCE = float3(0.0, 0.0, 0.5);

// World direction through a clip-space point; affine in clipXY, so it interpolates exactly across the sky quad.
float3 skybox_view_direction(float2 clipXY, float4x4 invProjection, float4x4 invView)
{
    float4 viewPos = mul(invProjection, float4(clipXY, 1.0, 1.0));
    return mul((float3x3)invView, float3(viewPos.xy, -1.0));
}

float3 skybox_radiance(SamplerCube cubemap, float3 direction, bool enabled)
{
    return enabled ? cubemap.Sample(normalize(direction)).xyz : SKYBOX_DISABLED_RADIANCE;
}

// Explicit LOD for compute, which has no derivatives: the path tracer's bounce that misses.
float3 skybox_radiance_lod0(SamplerCube cubemap, float3 direction, bool enabled)
{
    return enabled ? cubemap.SampleLevel(normalize(direction), 0.0).xyz : SKYBOX_DISABLED_RADIANCE;
}
