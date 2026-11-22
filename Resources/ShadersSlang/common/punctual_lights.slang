import scene_types;
import brdf;
import punctual_light;

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

// The C++ host's use of the fourth row's pad: .y is the cube slot plus one (0 unshadowed), .zw the depth range.
struct PunctualShadow
{
    int slot;
    float nearPlane;
    float farPlane;
};

PunctualShadow fetch_punctual_shadow(uint index)
{
    float4 row = punctualLights[1u + index * 4u + 3u];
    PunctualShadow shadow;
    shadow.slot = int(row.y + 0.5) - 1;
    shadow.nearPlane = row.z;
    shadow.farPlane = row.w;
    return shadow;
}

// The depth a face stores at `majorAxis` along its axis: perspectiveRH_ZO's z / w (PunctualLight.ixx's twin).
float punctual_shadow_depth(float majorAxis, float nearPlane, float farPlane)
{
    return farPlane * (majorAxis - nearPlane) / ((farPlane - nearPlane) * majorAxis);
}

// Fraction of the light reaching `worldPos`: a 2x2 hardware PCF tap at the centre and four around it.
float punctual_shadow_visibility(PunctualShadow shadow, float3 lightPos, float3 worldPos, float3 n, float3 l)
{
    uint width, height, cubes;
    punctualShadowMaps.GetDimensions(width, height, cubes);
    // A texel spans 2 * distance / width at the receiver; offsetting by it along n and l keeps acne off.
    float texel = 2.0 * length(worldPos - lightPos) / float(width);
    float3 d = worldPos + n * (1.5 * texel) + l * texel - lightPos;
    float majorAxis = max(abs(d.x), max(abs(d.y), abs(d.z)));
    if (majorAxis >= shadow.farPlane)
        return 1.0;
    float reference = punctual_shadow_depth(max(majorAxis, shadow.nearPlane), shadow.nearPlane, shadow.farPlane);

    float3 axis = normalize(d);
    float3 tangent = normalize(cross(abs(axis.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0), axis));
    float3 bitangent = cross(axis, tangent);
    float spread = 1.5 * 2.0 * majorAxis / float(width);
    static const float2 kTaps[5] = { float2(0.0, 0.0), float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0),
        float2(0.0, -1.0) };
    float lit = 0.0;
    for (int i = 0; i < 5; i++)
    {
        float3 tap = d + (tangent * kTaps[i].x + bitangent * kTaps[i].y) * spread;
        // A cube has no border: every tap direction lands on a face, so none needs the cascades' range check.
        lit += punctualShadowMaps.SampleCmpLevelZero(float4(tap, float(shadow.slot)), reference);
    }
    return lit * 0.2;
}

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
        PunctualShadow shadow = fetch_punctual_shadow(i);
        if (shadow.slot >= 0 && uint(shadow.slot) < shadowCubes)
            visibility = punctual_shadow_visibility(shadow, light.position, worldPos, n, incidence.l);
        total += brdf_direct(n, v, incidence.l, albedo, metallic, roughness, f0,
            light.radiance * incidence.attenuation * visibility, energy);
    }
    return total;
}
