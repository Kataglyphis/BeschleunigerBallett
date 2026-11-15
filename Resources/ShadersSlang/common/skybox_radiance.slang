// The skybox's linear radiance per view direction, shared by the sky pass and post's cloud composite.

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
