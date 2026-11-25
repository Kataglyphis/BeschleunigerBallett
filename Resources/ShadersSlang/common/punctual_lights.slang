import shared_constants;
import brdf;
import punctual_light;
import punctual_shadow;

// Owns the punctual-light binding: row 0 holds the count in x, then punctual_light.slang's four rows per light.

[vk::binding(PUNCTUAL_LIGHTS_BINDING, 0)] StructuredBuffer<float4> punctualLights;

// One cube per shadowed light (layer = slot * 6 + face), each face a 90-degree perspective; a comparison sampler.
[vk::binding(PUNCTUAL_SHADOW_MAP_BINDING, 0)] SamplerCubeArrayShadow punctualShadowMaps;

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

// The light's fourth row: punctual_shadow.slang's slot and depth range.
PunctualShadow fetch_light_shadow(uint index)
{
    return unpack_punctual_shadow(punctualLights[1u + index * 4u + 3u]);
}

// This engine's cube array, one 2x2 hardware PCF tap of a light's cube.
struct CubeTaps : IPunctualShadowTaps
{
    int slot;

    // A cube has no border: every tap direction lands on a face, so none needs the cascades' range check.
    float lit(float3 d, float reference)
    {
        return punctualShadowMaps.SampleCmpLevelZero(float4(d, float(slot)), reference);
    }
};

// Every light; a light with a cube among the `shadowCubes` rendered this frame is shadowed by it.
float3 punctual_lighting(float3 worldPos, float3 n, float3 v, float3 albedo, float metallic, float roughness, float3 f0,
    float2 energy, uint shadowCubes)
{
    float3 total = float3(0.0);
    uint count = punctual_light_count();
    for (uint i = 0u; i < count; i++)
    {
        PunctualLight light = fetch_punctual_light(i);
        PunctualIncidence incidence = punctual_incidence(light, worldPos);
        if (incidence.attenuation <= 0.0 || dot(n, incidence.l) <= 0.0)
            continue;
        float visibility = 1.0;
        PunctualShadow shadow = fetch_light_shadow(i);
        if (shadow.slot >= 0 && uint(shadow.slot) < shadowCubes)
        {
            uint width, height, cubes;
            punctualShadowMaps.GetDimensions(width, height, cubes);
            CubeTaps taps = { shadow.slot };
            visibility =
                punctual_shadow_visibility(taps, shadow, light.position, worldPos, n, incidence.l, float(width));
        }
        total += brdf_direct(n, v, incidence.l, albedo, metallic, roughness, f0,
            light.radiance * incidence.attenuation * visibility, energy);
    }
    return total;
}
