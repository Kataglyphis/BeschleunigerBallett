import scene_types;

// Volumetric clouds: Henyey-Greenstein phase and Beer-Lambert transmittance over a 3D noise volume.

static const float PI = 3.14159265358979323846;

[vk::binding(0, 0)] RWTexture2D<float4> outputImage;
[vk::binding(1, 0)] Sampler3D noiseTexture;

[vk::binding(0, 1)] ConstantBuffer<GlobalUBO> globalUBO;
[vk::binding(1, 1)] ConstantBuffer<SceneUBO> scene;

struct Clouds
{
    int num_march_steps;
    int num_march_steps_to_light;
    float3 radius;
    float3 offset;
    float4x4 inv_model_to_world;
    float threshold;
    float scale;
    float pillowness;
    float cirrus_effect;
    bool powder_effect;
};

float sample_density(float3 position, Clouds cloud)
{
    // frac() wraps negative inputs; the absolute value of fmod mirrors them across the origin instead.
    float3 uvwCoarse = frac((position + cloud.offset) / 256.0);
    // Explicit LOD: compute has no derivatives, so an implicit-LOD .Sample() fails SPIR-V validation.
    float4 noiseCoarse = noiseTexture.SampleLevel(uvwCoarse, 0.0);

    float baseDensity = ((cloud.pillowness * 0.25)) * max(0.0, noiseCoarse.r - cloud.threshold) * cloud.scale
                      + ((1.0 - cloud.pillowness) * 0.125) * max(0.0, noiseCoarse.g - cloud.threshold) * cloud.scale
                      + ((1.0 - cloud.pillowness) * 0.125) * max(0.0, noiseCoarse.b - cloud.threshold) * cloud.scale;

    baseDensity = baseDensity * (1.0 - cloud.cirrus_effect)
                + max(0.0, noiseCoarse.a - cloud.threshold) * cloud.scale * cloud.cirrus_effect;

    float3 uvwFine = frac((position + cloud.offset) / 64.0);
    float4 noiseFine = noiseTexture.SampleLevel(uvwFine, 0.0);

    float fineDensity = 0.6 * max(0.0, noiseFine.r - cloud.threshold) * cloud.scale
                      + 0.15 * max(0.0, noiseFine.g - cloud.threshold) * cloud.scale
                      + 0.15 * max(0.0, noiseFine.b - cloud.threshold) * cloud.scale;

    return baseDensity + fineDensity * 0.3;
}

// cosTheta is between the view ray and the vector toward the sun; positive g peaks at +1 (forward scattering).
float phase_HG(float cosTheta, float g)
{
    float denom = 1.0 + g * g - 2.0 * g * cosTheta;
    return (1.0 / (4.0 * PI)) * ((1.0 - g * g) / (denom * sqrt(denom)));
}

// Ray-box intersection (iquilezles). Returns entry/exit in oT.
bool box_intersect(float3 ro, float3 rd, float4x4 txx, float3 rad, out float2 oT)
{
    float3 rdd = (mul(txx, float4(rd, 0.0))).xyz;
    float3 roo = (mul(txx, float4(ro, 1.0))).xyz;

    float3 m = 1.0 / rdd;
    float3 s = float3(rdd.x < 0.0 ? 1.0 : -1.0, rdd.y < 0.0 ? 1.0 : -1.0, rdd.z < 0.0 ? 1.0 : -1.0);
    float3 t1 = m * (-roo + s * rad);
    float3 t2 = m * (-roo - s * rad);

    float tN = max(max(t1.x, t1.y), t1.z);
    float tF = min(min(t2.x, t2.y), t2.z);

    if (tN > tF || tF < 0.0) return false;

    oT = float2(tN, tF);
    return true;
}

float light_march(float3 samplePos, Clouds cloud)
{
    float totalDensity = 0.0;
    // dirLight.direction is where the light travels, so the vector toward it is the negation.
    float3 directionToLight = normalize(-scene.dirLight.direction.xyz);
    float2 oT;
    bool intersection = box_intersect(samplePos, directionToLight, cloud.inv_model_to_world, cloud.radius, oT);

    if (!intersection) return 1.0;

    // samplePos is inside the box, so the near intersection can be behind it.
    float t0 = max(oT.x, 0.0);
    float dtL = (oT.y - t0) / float(cloud.num_march_steps_to_light);

    for (int i = 0; i < cloud.num_march_steps_to_light; i++)
    {
        float3 p = samplePos + directionToLight * (t0 + (float(i) + 0.5) * dtL);
        totalDensity += sample_density(p, cloud) * dtL;
    }
    return exp(-totalDensity);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void clouds_main(uint3 tid : SV_DispatchThreadID)
{
    int2 texelCoord = int2(tid.xy);
    uint width, height;
    outputImage.GetDimensions(width, height);
    int2 size = int2(int(width), int(height));

    if (texelCoord.x >= size.x || texelCoord.y >= size.y)
        return;

    float2 uv = float2(texelCoord) / float2(size);
    float2 ndc = uv * 2.0 - 1.0;

    float4 clip = float4(ndc, 1.0, 1.0);
    float4 eyeCoords = mul(globalUBO.inv_projection, clip);
    eyeCoords = float4(eyeCoords.xy, -1.0, 0.0);
    float3 rayDirection = normalize(mul(globalUBO.inv_view, eyeCoords).xyz);
    float3 eyePosition = scene.cam_pos.xyz;

    Clouds cloud;
    // Clamped again although the host clamps; BuildIntegrity pins these literals to CloudDispatch.hpp's bounds.
    cloud.num_march_steps = int(clamp(scene.cloudParameters.w, 4.0, 128.0));
    cloud.num_march_steps_to_light = int(clamp(scene.cloudLightMarch.x, 1.0, 128.0));
    cloud.scale = scene.cloudMeshScale.w;
    cloud.threshold = scene.cloudMeshOffset.w;
    cloud.pillowness = scene.cloudParameters.x;
    cloud.cirrus_effect = scene.cloudParameters.y;
    cloud.powder_effect = scene.cloudParameters.z > 0.5;

    // cloud.scale is also the density multiplier; the host keeps both factors non-zero for the division below.
    cloud.radius = scene.cloudMeshScale.xyz * cloud.scale * 10.0;
    cloud.offset = scene.cloudMeshOffset.xyz;

    // Built directly: Slang has no inverse() for SPIR-V.
    cloud.inv_model_to_world = float4x4(1.0);
    cloud.inv_model_to_world[0][0] = 1.0 / cloud.radius.x;
    cloud.inv_model_to_world[1][1] = 1.0 / cloud.radius.y;
    cloud.inv_model_to_world[2][2] = 1.0 / cloud.radius.z;
    cloud.inv_model_to_world[3][0] = -cloud.offset.x / cloud.radius.x;
    cloud.inv_model_to_world[3][1] = -cloud.offset.y / cloud.radius.y;
    cloud.inv_model_to_world[3][2] = -cloud.offset.z / cloud.radius.z;

    float2 oT;
    bool intersection = box_intersect(eyePosition, rayDirection, cloud.inv_model_to_world, cloud.radius, oT);

    float lightEnergy = 0.0;
    float transmittance = 1.0;

    if (intersection && oT.y > 0.0)
    {
        oT.x = max(0.0, oT.x);
        float marchLength = oT.y - oT.x;
        float dt = marchLength / float(cloud.num_march_steps);
        for (int i = 0; i < cloud.num_march_steps; i++)
        {
            float3 samplePos = eyePosition + rayDirection * (oT.x + (float(i) + 0.5) * dt);

            float densityOfSample = sample_density(samplePos, cloud);
            float lightTransmittance = light_march(samplePos, cloud);
            // Powder only dims in-scattering; transmittance changes solely through the `*=` below.
            float powder = cloud.powder_effect ? (1.0 - exp(-densityOfSample * dt * 2.0)) : 1.0;

            lightEnergy += densityOfSample * dt * transmittance * lightTransmittance * powder
                         * phase_HG(dot(normalize(rayDirection), normalize(-scene.dirLight.direction.xyz)), 0.5);

            transmittance *= exp(-densityOfSample * dt);

            if (transmittance < 0.01) break;
        }
    }

    float3 cloudColor = lightEnergy * scene.dirLight.color.xyz * scene.dirLight.color.w;
    outputImage[tid.xy] = float4(cloudColor, 1.0 - transmittance);
}