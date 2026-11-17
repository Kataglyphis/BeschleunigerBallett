import scene_types;
import brdf;
import punctual_light;

// Owns the punctual-light binding: row 0 holds the count in x, then punctual_light.slang's four rows per light.

[vk::binding(PUNCTUAL_LIGHTS_BINDING, 0)] StructuredBuffer<float4> punctualLights;

uint punctual_light_count()
{
    return uint(punctualLights[0].x);
}

PunctualLight fetch_punctual_light(uint index)
{
    uint row = 1u + index * 4u;
    return unpack_punctual_light(
        punctualLights[row], punctualLights[row + 1u], punctualLights[row + 2u], punctualLights[row + 3u]);
}

// Every light, unshadowed, as the Rust raster passes shade them.
float3 punctual_lighting(float3 worldPos, float3 n, float3 v, float3 albedo, float metallic, float roughness, float3 f0)
{
    float3 total = float3(0.0);
    uint count = punctual_light_count();
    for (uint i = 0u; i < count; i++)
    {
        PunctualLight light = fetch_punctual_light(i);
        PunctualIncidence incidence = punctual_incidence(light, worldPos);
        if (incidence.attenuation <= 0.0)
            continue;
        total += brdf_direct(n, v, incidence.l, albedo, metallic, roughness, f0, light.radiance * incidence.attenuation);
    }
    return total;
}
