import scene_types;
import ambient;

// The environment AmbientMaps bakes from the drawn skybox, read through OxidANT's sky_ambient as Rust's passes read theirs.

[vk::binding(IBL_IRRADIANCE_BINDING, 0)] SamplerCube iblIrradiance;
[vk::binding(IBL_PREFILTERED_BINDING, 0)] SamplerCube iblPrefiltered;
[vk::binding(IBL_BRDF_LUT_BINDING, 0)] Sampler2D iblBrdfLut;

// Explicit levels throughout, so the ray-tracing stages, which have no derivatives, read them too.
struct EngineAmbientMaps : IAmbientMaps
{
    float prefilteredTopMip;

    float3 irradiance(float3 n)
    {
        return iblIrradiance.SampleLevel(n, 0.0).rgb;
    }
    float3 reflection(float3 r, float roughness)
    {
        return iblPrefiltered.SampleLevel(r, roughness * prefilteredTopMip).rgb;
    }
    float2 energy(float nDotV, float roughness)
    {
        return iblBrdfLut.SampleLevel(float2(nDotV, roughness), 0.0).rg;
    }
};

// The sky's light on a surface: the baked maps while the skybox is shown, else the analytic sky.
float3 scene_sky_ambient(SceneUBO ubo, float3 n, float3 v, float3 albedo, float metallic, float roughness, float3 f0,
    float2 energy)
{
    EngineAmbientMaps maps;
    maps.prefilteredTopMip = ubo.iblParams.y;
    return sky_ambient(maps, ubo.iblParams.x > 0.5, n, v, albedo, metallic, roughness, f0, energy);
}
