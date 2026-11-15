import scene_types;
import base_color;
import alpha_test;
import brdf;
import hdr_safe;
import material_fetch;
import material_textures;
import normal_map;

// Ray-query path tracer, SPIR-V only (WebGPU has no ray query). See docs/source/path-tracing.md.

struct PushConstantPathTracing
{
    float4 clearColor;
    uint width;
    uint height;
    uint frame_index;
    uint samples_per_pixel;
    uint max_bounces;
    // Furnace mode's white surface: its metallic and roughness, for the dielectric and the metal furnace alike.
    float furnace_metallic;
    float furnace_roughness;
    // Nonzero when the clouds read this frame's traced depth.
    uint trace_cloud_depth;
};

[vk::binding(globalUBO_BINDING, 0)] ConstantBuffer<GlobalUBO> globalUBO;
[vk::binding(sceneUBO_BINDING, 0)] ConstantBuffer<SceneUBO> sceneUBO;
// material_textures is imported directly: Slang does not re-export a plain `import` transitively.

[vk::binding(TLAS_BINDING, 1)] RaytracingAccelerationStructure TLAS;
[vk::binding(OUT_IMAGE_BINDING, 1)] RWTexture2D<float4> image;
[vk::binding(ACCUMULATION_IMAGE_BINDING, 1)] RWTexture2D<float4> accumulationImage;
[vk::binding(TRACED_DEPTH_IMAGE_BINDING, 1)] RWTexture2D<float> tracedDepth;

[[vk::push_constant]] PushConstantPathTracing pc_ray;

// PCG RNG: condensed pcg_output_rxs_mxs_32_32.
float stepAndOutputRNGFloat(inout uint rngState)
{
    rngState = rngState * 747796405u + 1u;
    uint word = ((rngState >> ((rngState >> 28u) + 4u)) ^ rngState) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word) / 4294967295.0;
}

// Unit t and b completing n to a right-handed frame, without a branch on n (Duff et al. 2017).
void orthonormal_basis(float3 n, out float3 t, out float3 b)
{
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// A GGX microfacet normal visible from v (Heitz 2018), in the frame where n is +z; alpha is distribution_ggx's.
float3 sample_ggx_visible_normal(float3 v, float alpha, float2 u)
{
    float3 vh = normalize(float3(alpha * v.x, alpha * v.y, v.z));
    float lengthSquared = vh.x * vh.x + vh.y * vh.y;
    float3 t1 = lengthSquared > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(lengthSquared) : float3(1.0, 0.0, 0.0);
    float3 t2 = cross(vh, t1);
    float r = sqrt(u.x);
    float phi = 2.0 * PI * u.y;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * p2;
    float3 nh = p1 * t1 + p2 * t2 + sqrt(max(1.0 - p1 * p1 - p2 * p2, 0.0)) * vh;
    return normalize(float3(alpha * nh.x, alpha * nh.y, max(nh.z, 1e-6)));
}

// Density of the direction sample_ggx_visible_normal reflects to: D(h) G1(v) / (4 n.v), exact GGX and Smith G1.
float ggx_visible_normal_pdf(float nDotV, float nDotH, float alpha)
{
    float a2 = alpha * alpha;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    // Unfloored, unlike distribution_ggx: the density must be the one the sampler draws.
    float ndf = a2 / (PI * d * d);
    float g1 = 2.0 * nDotV / (nDotV + sqrt(a2 + (1.0 - a2) * nDotV * nDotV));
    return ndf * g1 / (4.0 * nDotV);
}

// Chance to sample the GGX lobe: its Fresnel share at n.v; the floor keeps a white dielectric's highlight sampled.
float specular_lobe_probability(float3 albedo, float metallic, float3 f0, float nDotV)
{
    const float3 LUMA = float3(0.2126, 0.7152, 0.0722);
    float3 fresnel = fresnel_schlick(nDotV, f0);
    float specular = dot(fresnel, LUMA);
    float diffuse = dot((float3(1.0) - fresnel) * (1.0 - metallic) * albedo, LUMA);
    return clamp(specular / max(specular + diffuse, 1e-6), 0.1, 1.0);
}

// NDC depth of the first surface through the pixel centre, for the clouds; 1 on a miss, like a cleared depth buffer.
float primary_hit_depth(uint2 pixel, uint2 resolution, float3 origin)
{
    float2 cs = (float2(pixel) + 0.5) / float2(resolution) * 2.0 - 1.0;
    float4 target = mul(globalUBO.inv_projection, float4(cs.x, cs.y, 1, 1));
    float3 direction = normalize(mul(globalUBO.inv_view, float4(normalize(target.xyz), 0)).xyz);

    RayQuery<0> depthQuery;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = 10000.0;
    depthQuery.TraceRayInline(TLAS, 0, 0xFFu, ray);
    while (depthQuery.Proceed())
    {
        if (depthQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint candidateObjIndex = depthQuery.CandidateInstanceID() + depthQuery.CandidateGeometryIndex();
            ObjectDescription candidateObj = fetch_object_description(candidateObjIndex);
            if (!ray_hit_masked_out(candidateObj, depthQuery.CandidatePrimitiveIndex(), depthQuery.CandidateTriangleBarycentrics()))
            {
                depthQuery.CommitNonOpaqueTriangleHit();
            }
        }
    }
    if (depthQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return 1.0;
    }
    float3 hitPosition = origin + direction * depthQuery.CommittedRayT();
    float4 clip = mul(globalUBO.projection, mul(globalUBO.view, float4(hitPosition, 1.0)));
    return clip.z / clip.w;
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
    // Furnace mode (KATAGLYPHIS_PT_FURNACE): one white surface everywhere and a uniform sky of clearColor.rgb.
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
            float3 authoredNormal = normalize(mul(normalHit, (float3x3)worldToObject));
            // The winding decides the side, as SV_IsFrontFace does in raster; near a silhouette the vertex normal can disagree.
            bool frontFace = rayQuery.CommittedTriangleFrontFace();
            float3 hitWorldNormal = frontFace ? authoredNormal : -authoredNormal;
            // The triangle's own plane, facing the ray: what the next ray must leave from and stay above.
            float3 triangleNormal = mul(cross(v1.position - v0.position, v2.position - v0.position), (float3x3)worldToObject);
            float3 faceNormal = dot(triangleNormal, triangleNormal) > 1e-30 ? normalize(triangleNormal) : hitWorldNormal;
            faceNormal = dot(faceNormal, rayDirection) <= 0.0 ? faceNormal : -faceNormal;

            float2 texCoords = v0.texture_coords * barycentrics.x
                             + v1.texture_coords * barycentrics.y
                             + v2.texture_coords * barycentrics.z;

            ObjMaterial material = fetch_material(obj, primitiveID);

            // The mapped normal shades and orients the lobes; the triangle's face normal offsets the next ray and bounds it.
            float3 shadingNormal = hitWorldNormal;
            if (material.normalTextureID >= 0)
            {
                float4 tangentHit = v0.tangent * barycentrics.x + v1.tangent * barycentrics.y + v2.tangent * barycentrics.z;
                // A tangent is a surface direction: objToWorld, not the inverse-transpose.
                float3 worldTangent = normalize(mul(objToWorld, float4(tangentHit.xyz, 0.0)));
                // Explicit LOD: see the base-colour sample below for why.
                float handedness = mirrored_handedness(tangentHit.w, (float3x3)objToWorld);
                shadingNormal = resolved_normal_lod0(authoredNormal, float4(worldTangent, handedness), frontFace, obj, material, texCoords);
            }

            float3 albedo;
            if (furnace)
            {
                // Exactly 1, or the furnace goldens' energy bounds stop describing the surface.
                albedo = float3(1.0);
            }
            else
            {
                if (material.textureID >= 0)
                {
                    int textureId = resolve_texture_slot(obj, material);
                    // Explicit LOD: compute has no derivatives, so an implicit-LOD .Sample() fails SPIR-V validation.
                    albedo = base_color(material, textures[textureId].SampleLevel(textureSamplers[textureId], transform_uv(texCoords, material), 0.0).xyz);
                }
                else
                {
                    albedo = material.diffuse;
                }

                float4 vertexColor = v0.color * barycentrics.x + v1.color * barycentrics.y + v2.color * barycentrics.z;
                albedo *= vertexColor.rgb;
            }

            // KHR_materials_unlit ends the path at its albedo, before metallic can darken it; not in furnace mode.
            if (!furnace && material.unlit != 0)
            {
                radiance += throughput * albedo;
                break;
            }

            // Emission is added before throughput absorbs this surface; never in furnace mode.
            if (!furnace)
            {
                // Explicit LOD: see the base-colour sample above for why.
                radiance += throughput * resolved_emission_lod0(obj, material, texCoords);
            }

            // Factor times texture, as raster reads them; the furnace's white surface brings its own pair.
            float2 metallicRoughness = furnace
                ? float2(pc_ray.furnace_metallic, clamp(pc_ray.furnace_roughness, 0.045, 1.0))
                : resolved_metallic_roughness_lod0(obj, material, texCoords);
            float metallic = metallicRoughness.x;
            float roughness = metallicRoughness.y;
            float3 f0 = lerp(float3(0.04), albedo, metallic);

            float3 V = -rayDirection;
            // A map or a vertex normal tilted away from the viewer leaves no lobe to sample, so the face normal shades.
            if (dot(shadingNormal, V) < 1e-4)
            {
                shadingNormal = faceNormal;
            }
            float nDotV = max(dot(shadingNormal, V), 1e-4);

            // Offset along the normal against self-intersection.
            rayOrigin = hitWorldPosition + 0.0001 * faceNormal;

            // NEE cannot double-count with the sky term: the delta light is unreachable by a bounce ray.
            float3 lightDir = normalize(-sceneUBO.dirLight.direction.xyz);
            if (dot(shadingNormal, lightDir) > 0.0)
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
                    // brdf_direct is raster's BRDF times n.l and the light, so an unshadowed hit matches forward's.
                    float3 lightRadiance = sceneUBO.dirLight.color.rgb * sceneUBO.dirLight.color.w;
                    radiance += throughput * brdf_direct(shadingNormal, V, lightDir, albedo, metallic, roughness, f0, lightRadiance);
                }
            }

            // One-sample MIS over both lobes: draw from one, weight by the density of their mixture.
            float specularChance = specular_lobe_probability(albedo, metallic, f0, nDotV);
            float alpha = roughness * roughness;
            float3 frameTangent;
            float3 frameBitangent;
            orthonormal_basis(shadingNormal, frameTangent, frameBitangent);
            float3 newDirection;
            if (stepAndOutputRNGFloat(rngState) < specularChance)
            {
                float3 viewLocal = float3(dot(V, frameTangent), dot(V, frameBitangent), dot(V, shadingNormal));
                float2 u = float2(stepAndOutputRNGFloat(rngState), stepAndOutputRNGFloat(rngState));
                float3 h = mul(sample_ggx_visible_normal(viewLocal, alpha, u), float3x3(frameTangent, frameBitangent, shadingNormal));
                newDirection = reflect(-V, h);
            }
            else
            {
                // Cosine-weighted: a random unit-sphere point around the normal tip (RTIOW 8.5).
                float theta = 6.2831853 * stepAndOutputRNGFloat(rngState);
                float u = 2.0 * stepAndOutputRNGFloat(rngState) - 1.0;
                float r = sqrt(max(1.0 - u * u, 0.0));
                newDirection = shadingNormal + float3(r * cos(theta), r * sin(theta), u);

                // RTIOW 9.4: a vector landing on -normal degenerates to zero length.
                if (dot(newDirection, newDirection) < 1e-8)
                {
                    newDirection = shadingNormal;
                }
            }
            // TMin, TMax and skyT assume a unit rayDirection.
            rayDirection = normalize(newDirection);

            // Below either surface ends the path: past the triangle, a tilted map or vertex normal would leak light through it.
            float nDotL = dot(shadingNormal, rayDirection);
            if (nDotL <= 0.0 || dot(faceNormal, rayDirection) <= 0.0)
            {
                break;
            }
            float nDotH = max(dot(shadingNormal, normalize(V + rayDirection)), 0.0);
            float pdf = specularChance * ggx_visible_normal_pdf(nDotV, nDotH, alpha) + (1.0 - specularChance) * nDotL / PI;
            // brdf_direct under a unit light is f * n.l: the BRDF NEE and raster use, so no lobe is lit twice.
            throughput *= brdf_direct(shadingNormal, V, rayDirection, albedo, metallic, roughness, f0, float3(1.0)) / max(pdf, 1e-6);
        }

        // One NaN or Inf sample would poison the running mean until the next reset.
        summedColor += hdr_safe(radiance);
    }

    float3 avgColor = summedColor / float(numSamples);

    // A whole extra primary ray per pixel, so only when the clouds read it.
    if (pc_ray.trace_cloud_depth != 0u)
    {
        tracedDepth[tid.xy] = primary_hit_depth(tid.xy, resolution, cameraOrigin.xyz);
    }

    // Temporal accumulation: running mean across frames.
    if (pc_ray.frame_index > 0)
    {
        float4 prev = accumulationImage[tid.xy];
        float3 accumColor = (prev.rgb * float(pc_ray.frame_index) + avgColor) / float(pc_ray.frame_index + 1);
        accumulationImage[tid.xy] = float4(accumColor, 1.0);
        image[tid.xy] = float4(hdr_safe(accumColor), 1.0);
    }
    else
    {
        accumulationImage[tid.xy] = float4(avgColor, 1.0);
        image[tid.xy] = float4(hdr_safe(avgColor), 1.0);
    }
}
