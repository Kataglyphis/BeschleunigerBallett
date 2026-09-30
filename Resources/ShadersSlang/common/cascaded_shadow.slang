import scene_types;

// Imports scene_types, not rt_types, to keep ray tracing capabilities out of rasterization shaders.

// Comparison sampler: each SampleCmpLevelZero is a bilinear 2x2 PCF tap, blending visibility rather than depth.
[vk::binding(SHADOW_MAP_BINDING, 0)] Sampler2DArrayShadow directionalShadowMaps;

// Fraction of direct light blocked: 0 = fully lit, 1 = fully shadowed.
float calc_cascaded_shadow(float3 worldPos, float3 N, float3 L, ConstantBuffer<SceneUBO> sceneUBO)
{
    // Second clamp after the host's, so a stray write cannot index cascadeLightSpaceMatrices out of bounds.
    int cascadeCount = clamp(int(sceneUBO.numCascades), 0, MAX_CASCADES);
    if (cascadeCount <= 0) return 0.0;

    float fragDistance = dot(worldPos - sceneUBO.cam_pos.xyz, normalize(sceneUBO.view_dir.xyz));
    int cascadeIndex = cascadeCount - 1;
    for (int i = 0; i < cascadeCount; i++)
    {
        if (fragDistance < sceneUBO.cascadeSplits[i])
        {
            cascadeIndex = i;
            break;
        }
    }

    float4 lightSpacePos = mul(sceneUBO.cascadeLightSpaceMatrices[cascadeIndex], float4(worldPos, 1.0));
    float3 proj = lightSpacePos.xyz / lightSpacePos.w;
    float2 shadowUV = proj.xy * 0.5 + 0.5;
    float currentDepth = proj.z;
    if (currentDepth > 1.0 || currentDepth < 0.0 ||
        shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0)
        return 0.0;

    float bias = max(0.002 * (1.0 - dot(N, L)), 0.0005);
    // Second clamp after the host's: zero iterations would read as fully shadowed.
    int radius = clamp(int(sceneUBO.pcfRadius), 0, MAX_PCF_RADIUS);
    uint shadowWidth, shadowHeight, shadowLayers;
    directionalShadowMaps.GetDimensions(shadowWidth, shadowHeight, shadowLayers);
    float2 texelSize = 1.0 / float2(float(shadowWidth), float(shadowHeight));
    float visible = 0.0;
    float taps = 0.0;
    // Out-of-map taps count as lit: the sampler clamps to edge, and its hard-coded black border reads as shadow.
    for (int x = -radius; x <= radius; x++)
    {
        for (int y = -radius; y <= radius; y++)
        {
            float2 uv = shadowUV + float2(x, y) * texelSize;
            visible += (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0)
              ? directionalShadowMaps.SampleCmpLevelZero(float3(uv, float(cascadeIndex)), currentDepth - bias)
              : 1.0;
            taps += 1.0;
        }
    }
    return 1.0 - visible / max(taps, 1.0);
}