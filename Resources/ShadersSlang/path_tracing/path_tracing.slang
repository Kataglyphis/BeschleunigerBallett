import scene_types;
import base_color;
import alpha_test;
import material_fetch;
import material_rules;
import material_textures;

// Ray-query path tracer, SPIR-V only (WebGPU has no ray query). See docs/path-tracing.md.

struct PushConstantPathTracing
{
    float4 clearColor;
    uint width;
    uint height;
    uint frame_index;
    uint samples_per_pixel;
    uint max_bounces;
};

[vk::binding(globalUBO_BINDING, 0)] ConstantBuffer<GlobalUBO> globalUBO;
[vk::binding(sceneUBO_BINDING, 0)] ConstantBuffer<SceneUBO> sceneUBO;
// material_textures is imported directly: Slang does not re-export a plain `import` transitively.

[vk::binding(TLAS_BINDING, 1)] RaytracingAccelerationStructure TLAS;
[vk::binding(OUT_IMAGE_BINDING, 1)] RWTexture2D<float4> image;
[vk::binding(ACCUMULATION_IMAGE_BINDING, 1)] RWTexture2D<float4> accumulationImage;

[[vk::push_constant]] PushConstantPathTracing pc_ray;

// PCG RNG: condensed pcg_output_rxs_mxs_32_32.
float stepAndOutputRNGFloat(inout uint rngState)
{
    rngState = rngState * 747796405u + 1u;
    uint word = ((rngState >> ((rngState >> 28u) + 4u)) ^ rngState) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word) / 4294967295.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void path_tracing_main(uint3 tid : SV_DispatchThreadID)
{
    uint2 resolution = uint2(pc_ray.width, pc_ray.height);
    if (tid.x >= resolution.x || tid.y >= resolution.y)
        return;

    // RNG state: frame index folded in so every frame draws different samples.
    uint rngState = (resolution.x * tid.y + tid.x) + pc_ray.frame_index * 2654435769u;

    float4 cameraOrigin = mul(globalUBO.inv_view, float4(0, 0, 0, 1));

    int numSamples = int(pc_ray.samples_per_pixel);
    int maxBounces = int(pc_ray.max_bounces);
    float3 summedColor = float3(0.0);
    // Furnace mode (KATAGLYPHIS_PT_FURNACE): albedo 1 and a uniform sky, so every pixel converges to clearColor.rgb.
    bool furnace = pc_ray.clearColor.w > 0.5;

    for (int sampleIdx = 0; sampleIdx < numSamples; sampleIdx++)
    {
        // Jitter within the pixel so accumulation anti-aliases edges instead of only denoising.
        float2 pixelCenter = float2(tid.xy) + float2(stepAndOutputRNGFloat(rngState), stepAndOutputRNGFloat(rngState));
        float2 uv = pixelCenter / float2(resolution);
        float2 cs = uv * 2.0 - 1.0;

        float4 target = mul(globalUBO.inv_projection, float4(cs.x, cs.y, 1, 1));
        float3 rayOrigin = cameraOrigin.xyz;
        float3 rayDirection = normalize(mul(globalUBO.inv_view, float4(normalize(target.xyz), 0)).xyz);

        float3 throughput = float3(1.0);
        float3 radiance = float3(0.0);

        for (int bounce = 0; bounce < maxBounces; bounce++)
        {
            // Russian roulette from the fourth segment; the 1/survival reweight keeps the estimator unbiased.
            if (bounce >= 3)
            {
                float survival = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
                if (stepAndOutputRNGFloat(rngState) > survival)
                {
                    break;
                }
                throughput /= survival;
            }

            RayQuery<0> rayQuery;
            RayDesc ray;
            ray.Origin = rayOrigin;
            ray.Direction = rayDirection;
            ray.TMin = 0.001;
            ray.TMax = 10000.0;
            rayQuery.TraceRayInline(TLAS, 0, 0xFFu, ray);
            // Loop Proceed() to completion; with no any-hit stage, MASK cut-outs are alpha-tested per candidate here.
            while (rayQuery.Proceed())
            {
                if (rayQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
                {
                    uint candidateObjIndex = rayQuery.CandidateInstanceID() + rayQuery.CandidateGeometryIndex();
                    ObjectDescription candidateObj = fetch_object_description(candidateObjIndex);
                    if (!ray_hit_masked_out(candidateObj, rayQuery.CandidatePrimitiveIndex(), rayQuery.CandidateTriangleBarycentrics()))
                    {
                        rayQuery.CommitNonOpaqueTriangleHit();
                    }
                }
            }

            if (rayQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            {
                // Sky kept well below the directional light so the NEE term dominates the look.
                float3 skyRadiance;
                if (furnace)
                {
                    skyRadiance = pc_ray.clearColor.rgb;
                }
                else
                {
                    float skyT = 0.5 * (rayDirection.y + 1.0);
                    skyRadiance = 0.5 * lerp(float3(1.0), float3(0.5, 0.7, 1.0), skyT);
                }
                radiance += throughput * skyRadiance;
                break;
            }

            // The geometry index is required: without it a multi-mesh hit reads mesh 0's buffers out of bounds.
            uint objIndex = rayQuery.CommittedInstanceID() + rayQuery.CommittedGeometryIndex();
            float3x4 objToWorld = rayQuery.CommittedObjectToWorld3x4();
            float3x4 worldToObject = rayQuery.CommittedWorldToObject3x4();

            ObjectDescription obj = fetch_object_description(objIndex);
            Vertices* vertices = (Vertices*)obj.vertex_address;
            Indices* indices = (Indices*)obj.index_address;

            uint primitiveID = rayQuery.CommittedPrimitiveIndex();

            int3 tri = indices->i[primitiveID];
            Vertex v0 = vertices->v[tri.x];
            Vertex v1 = vertices->v[tri.y];
            Vertex v2 = vertices->v[tri.z];

            float2 bary = rayQuery.CommittedTriangleBarycentrics();
            float3 barycentrics = float3(1.0 - bary.x - bary.y, bary.x, bary.y);

            float3 objectPos = v0.position * barycentrics.x + v1.position * barycentrics.y + v2.position * barycentrics.z;
            // float3x4 is 3 rows × 4 cols; mul(float3x4, float4) = float3.
            float3 hitWorldPosition = mul(objToWorld, float4(objectPos, 1.0));

            float3 normalHit = v0.normal * barycentrics.x + v1.normal * barycentrics.y + v2.normal * barycentrics.z;
            // Inverse-transpose = row-multiply worldToObject: mul(v, M), not mul(M, v).
            float3 hitWorldNormal = normalize(mul(normalHit, (float3x3)worldToObject));
            // faceforward: the normal must oppose the ray.
            if (dot(hitWorldNormal, rayDirection) > 0.0)
            {
                hitWorldNormal = -hitWorldNormal;
            }

            float2 texCoords = v0.texture_coords * barycentrics.x
                             + v1.texture_coords * barycentrics.y
                             + v2.texture_coords * barycentrics.z;

            ObjMaterial material = fetch_material(obj, primitiveID);

            // Normal map only affects NEE shading; the geometric normal still drives ray offset and bounce sampling.
            float3 shadingNormal = hitWorldNormal;
            if (material.normalTextureID >= 0)
            {
                float4 tangentHit = v0.tangent * barycentrics.x + v1.tangent * barycentrics.y + v2.tangent * barycentrics.z;
                // A tangent is a surface direction: objToWorld, not the inverse-transpose.
                float3 worldTangent = normalize(mul(objToWorld, float4(tangentHit.xyz, 0.0)));
                // Explicit LOD: see the base-colour sample below for why.
                shadingNormal = resolved_normal_lod0(hitWorldNormal, float4(worldTangent, tangentHit.w), obj, material, texCoords);
            }

            float3 hitColor;
            if (furnace)
            {
                // Exactly 1, or PathTracingPassesTheWhiteFurnaceTest's convergence invariant breaks.
                hitColor = float3(1.0);
            }
            else
            {
                if (material.textureID >= 0)
                {
                    int textureId = resolve_texture_slot(obj, material);
                    // Explicit LOD: compute has no derivatives, so an implicit-LOD .Sample() fails SPIR-V validation.
                    hitColor = base_color(material, textures[textureId].SampleLevel(textureSamplers[textureId], transform_uv(texCoords, material), 0.0).xyz);
                }
                else
                {
                    hitColor = material.diffuse;
                }

                float4 vertexColor = v0.color * barycentrics.x + v1.color * barycentrics.y + v2.color * barycentrics.z;
                hitColor *= vertexColor.rgb;

                // Gated on the texture, not the factor: this Lambertian kernel uses only the metallic half.
                if (material.metallicRoughnessTextureID >= 0)
                {
                    // Explicit LOD: see the base-colour sample above for why.
                    float4 mrSample = sample_metallic_roughness_lod0(obj, material, texCoords);
                    hitColor *= (1.0 - material_metallic_roughness(material, mrSample).x);
                }
            }

            // KHR_materials_unlit ends the path here; not in furnace mode, where it would break convergence.
            if (!furnace && material.unlit != 0)
            {
                radiance += throughput * hitColor;
                break;
            }

            // Emission is added before throughput absorbs this albedo; never in furnace mode.
            if (!furnace)
            {
                // Explicit LOD: see the base-colour sample above for why.
                radiance += throughput * resolved_emission_lod0(obj, material, texCoords);
            }

            // Apply color absorption.
            throughput *= hitColor;

            // Offset along the normal against self-intersection.
            rayOrigin = hitWorldPosition + 0.0001 * hitWorldNormal;

            // NEE cannot double-count with the sky term: the delta light is unreachable by a bounce ray.
            float3 lightDir = normalize(-sceneUBO.dirLight.direction.xyz);
            float nDotL = dot(shadingNormal, lightDir);
            if (nDotL > 0.0)
            {
                // Not forced opaque: the search ends at the first committed hit, so masked-out candidates are skipped.
                RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> shadowQuery;
                RayDesc shadowRay;
                shadowRay.Origin = rayOrigin;
                shadowRay.Direction = lightDir;
                shadowRay.TMin = 0.001;
                shadowRay.TMax = 10000.0;
                shadowQuery.TraceRayInline(
                    TLAS,
                    RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                    0xFFu,
                    shadowRay);
                while (shadowQuery.Proceed())
                {
                    if (shadowQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
                    {
                        uint candidateObjIndex = shadowQuery.CandidateInstanceID() + shadowQuery.CandidateGeometryIndex();
                        ObjectDescription candidateObj = fetch_object_description(candidateObjIndex);
                        if (!ray_hit_masked_out(candidateObj, shadowQuery.CandidatePrimitiveIndex(), shadowQuery.CandidateTriangleBarycentrics()))
                        {
                            shadowQuery.CommitNonOpaqueTriangleHit();
                        }
                    }
                }
                if (shadowQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
                {
                    // NEE carries the Lambertian 1/pi; cosine-weighted bounce sampling cancels it.
                    const float INV_PI = 0.31830988;
                    float3 lightColor = sceneUBO.dirLight.color.rgb;
                    float lightIntensity = sceneUBO.dirLight.color.w;
                    radiance += throughput * nDotL * INV_PI * lightColor * lightIntensity;
                }
            }

            // Cosine-weighted bounce: a random unit-sphere point around the normal tip (RTIOW 8.5).
            float theta = 6.2831853 * stepAndOutputRNGFloat(rngState);
            float u = 2.0 * stepAndOutputRNGFloat(rngState) - 1.0;
            float r = sqrt(max(1.0 - u * u, 0.0));
            float3 newDirection = hitWorldNormal + float3(r * cos(theta), r * sin(theta), u);

            // RTIOW 9.4: a vector landing on -normal degenerates to zero length.
            if (dot(newDirection, newDirection) < 1e-8)
            {
                newDirection = hitWorldNormal;
            }
            // TMin, TMax and skyT assume a unit rayDirection.
            rayDirection = normalize(newDirection);
        }

        summedColor += radiance;
    }

    float3 avgColor = summedColor / float(numSamples);

    // Temporal accumulation: running mean across frames.
    if (pc_ray.frame_index > 0)
    {
        float4 prev = accumulationImage[tid.xy];
        float3 accumColor = (prev.rgb * float(pc_ray.frame_index) + avgColor) / float(pc_ray.frame_index + 1);
        accumulationImage[tid.xy] = float4(accumColor, 1.0);
        image[tid.xy] = float4(accumColor, 1.0);
    }
    else
    {
        accumulationImage[tid.xy] = float4(avgColor, 1.0);
        image[tid.xy] = float4(avgColor, 1.0);
    }
}
