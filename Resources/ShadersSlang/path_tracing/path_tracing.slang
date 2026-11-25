import scene_types;
import base_color;
import alpha_test;
import brdf;
import camera_ray;
import constants;
import hdr_safe;
import material_fetch;
import material_textures;
import gltf_material;
import path_estimator;
import skybox_radiance;
import punctual_light;
import punctual_lights;
import surface_lighting;
import trace_constants;

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
    // Nonzero while the skybox is on: a bounce that misses sees its cube map, else the disabled sky's navy.
    uint skybox_enabled;
};

// OxidANT's bounce_sky scale at its defaults (ambient 0.35, intensity 1); this kernel does not read the slider.
static const float BOUNCE_SKY_SCALE = 0.35;

[vk::binding(globalUBO_BINDING, 0)] ConstantBuffer<GlobalUBO> globalUBO;
[vk::binding(sceneUBO_BINDING, 0)] ConstantBuffer<SceneUBO> sceneUBO;
// material_textures is imported directly: Slang does not re-export a plain `import` transitively.

[vk::binding(TLAS_BINDING, 1)] RaytracingAccelerationStructure TLAS;
[vk::binding(OUT_IMAGE_BINDING, 1)] RWTexture2D<float4> image;
[vk::binding(ACCUMULATION_IMAGE_BINDING, 1)] RWTexture2D<float4> accumulationImage;
[vk::binding(TRACED_DEPTH_IMAGE_BINDING, 1)] RWTexture2D<float> tracedDepth;
[vk::binding(SKYBOX_CUBEMAP_BINDING, 1)] SamplerCube skyboxCubemap;

[[vk::push_constant]] PushConstantPathTracing pc_ray;

// NDC depth of the first surface through the pixel centre, for the clouds; 1 on a miss, like a cleared depth buffer.
float primary_hit_depth(uint2 pixel, uint2 resolution)
{
    CameraRay camera = camera_ray_through_pixel(clip_to_world(globalUBO), float2(pixel) + 0.5, float2(resolution));

    RayQuery<0> depthQuery;
    RayDesc ray;
    ray.Origin = camera.origin;
    ray.Direction = camera.direction;
    ray.TMin = 0.0;
    ray.TMax = camera.length;
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
    float3 hitPosition = camera.origin + camera.direction * depthQuery.CommittedRayT();
    float4 clip = mul(globalUBO.projection, mul(globalUBO.view, float4(hitPosition, 1.0)));
    return clip.z / clip.w;
}

// A shadow query towards `direction`; true when an unmasked triangle lies within `tMax`.
bool occluded_inline(float3 origin, float3 direction, float tMax)
{
    // Not forced opaque: the search ends at the first committed hit, so masked-out candidates are skipped.
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> shadowQuery;
    RayDesc shadowRay;
    shadowRay.Origin = origin;
    shadowRay.Direction = direction;
    shadowRay.TMin = TRACE_TMIN;
    shadowRay.TMax = tMax;
    shadowQuery.TraceRayInline(TLAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, shadowRay);
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
    return shadowQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void path_tracing_main(uint3 tid : SV_DispatchThreadID)
{
    uint2 resolution = uint2(pc_ray.width, pc_ray.height);
    if (tid.x >= resolution.x || tid.y >= resolution.y)
        return;

    // RNG state: frame index folded in so every frame draws different samples.
    uint rngState = path_rng_seed(resolution.x * tid.y + tid.x, pc_ray.frame_index);

    float4x4 clipToWorld = clip_to_world(globalUBO);

    int numSamples = int(pc_ray.samples_per_pixel);
    int maxBounces = int(pc_ray.max_bounces);
    float3 summedColor = float3(0.0);
    // Furnace mode (KATAGLYPHIS_PT_FURNACE): one white surface everywhere and a uniform sky of clearColor.rgb.
    bool furnace = pc_ray.clearColor.w > 0.5;

    for (int sampleIdx = 0; sampleIdx < numSamples; sampleIdx++)
    {
        // Jitter within the pixel so accumulation anti-aliases edges instead of only denoising.
        float2 pixelCenter = float2(tid.xy) + float2(rng_next(rngState), rng_next(rngState));
        // The camera segment runs from the near plane to the far plane, as raster sees; later ones leave a surface.
        CameraRay camera = camera_ray_through_pixel(clipToWorld, pixelCenter, float2(resolution));
        float3 rayOrigin = camera.origin;
        float3 rayDirection = camera.direction;
        float rayTMin = 0.0;
        float rayTMax = camera.length;

        float3 throughput = float3(1.0);
        float3 radiance = float3(0.0);

        for (int bounce = 0; bounce < maxBounces; bounce++)
        {
            // Russian roulette from the fourth segment; the 1/survival reweight keeps the estimator unbiased.
            if (!russian_roulette_survives(uint(bounce), throughput, rngState))
            {
                break;
            }

            RayQuery<0> rayQuery;
            RayDesc ray;
            ray.Origin = rayOrigin;
            ray.Direction = rayDirection;
            ray.TMin = rayTMin;
            ray.TMax = rayTMax;
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
                float3 skyRadiance;
                if (furnace)
                {
                    skyRadiance = pc_ray.clearColor.rgb;
                }
                else if (bounce == 0)
                {
                    // A camera ray keeps the soft gradient at half intensity, as OxidANT's keeps its procedural sky.
                    float skyT = 0.5 * (rayDirection.y + 1.0);
                    skyRadiance = 0.5 * lerp(float3(1.0), float3(0.5, 0.7, 1.0), skyT);
                }
                else
                {
                    // A bounce sees the sky the raster draws, as OxidANT's bounce_sky lights it.
                    skyRadiance = BOUNCE_SKY_SCALE * skybox_radiance_lod0(skyboxCubemap, rayDirection, pc_ray.skybox_enabled != 0u);
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

            float4 texCoords = uv_sets(v0.texture_coords, v0.texture_coords1) * barycentrics.x
                             + uv_sets(v1.texture_coords, v1.texture_coords1) * barycentrics.y
                             + uv_sets(v2.texture_coords, v2.texture_coords1) * barycentrics.z;

            ObjMaterial material = fetch_material(obj, primitiveID);

            // The mapped normal shades and orients the lobes; the triangle's face normal offsets the next ray and bounds it.
            float3 shadingNormal = hitWorldNormal;
            if (material.normalTextureID >= 0)
            {
                float4 tangentHit = v0.tangent * barycentrics.x + v1.tangent * barycentrics.y + v2.tangent * barycentrics.z;
                // A tangent is a surface direction: objToWorld, not the inverse-transpose.
                float3 worldTangent = normalize(mul(objToWorld, float4(tangentHit.xyz, 0.0)));
                // Explicit LOD: see the base-colour sample below for why.
                float handedness = tangentHit.w * mirror_sign((float3x3)objToWorld);
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
                    albedo = base_color(material, textures[NonUniformResourceIndex(textureId)].SampleLevel(textureSamplers[NonUniformResourceIndex(textureId)], base_color_uv(texCoords, material), 0.0).xyz);
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
                ? float2(pc_ray.furnace_metallic, clamp(pc_ray.furnace_roughness, MIN_ROUGHNESS, 1.0))
                : resolved_metallic_roughness_lod0(obj, material, texCoords);
            float metallic = metallicRoughness.x;
            float roughness = metallicRoughness.y;
            // NEE's brdf_direct takes it; sample_bounce derives the same one.
            float3 f0 = surface_f0(albedo, metallic);

            float3 V = -rayDirection;
            // A map or a vertex normal tilted away from the viewer leaves no lobe to sample, so the face normal shades.
            if (dot(shadingNormal, V) < 1e-4)
            {
                shadingNormal = faceNormal;
            }

            // The fit once per path vertex, after the normal is final: both lights and the bounce share it.
            float2 energy = ggx_energy(dot(shadingNormal, V), roughness);

            // Offset along the normal against self-intersection.
            rayOrigin = hitWorldPosition + TRACE_SURFACE_OFFSET * faceNormal;
            rayTMin = TRACE_TMIN;
            rayTMax = TRACE_TMAX;

            // NEE cannot double-count with the sky term: the delta light is unreachable by a bounce ray.
            float3 lightDir = normalize(-sceneUBO.dirLight.direction.xyz);
            if (dot(shadingNormal, lightDir) > 0.0)
            {
                if (!occluded_inline(rayOrigin, lightDir, TRACE_TMAX))
                {
                    // brdf_direct is raster's BRDF times n.l and the light, so an unshadowed hit matches forward's.
                    float3 lightRadiance = sceneUBO.dirLight.color.rgb * sceneUBO.dirLight.color.w;
                    radiance += throughput * brdf_direct(shadingNormal, V, lightDir, albedo, metallic, roughness, f0, lightRadiance, energy);
                }
            }

            // One punctual light per bounce, picked uniformly and weighted by the count; no draw without lights.
            uint punctualCount = punctual_light_count();
            if (punctualCount > 0u)
            {
                uint pick = min(uint(rng_next(rngState) * float(punctualCount)), punctualCount - 1u);
                PunctualLight light = fetch_punctual_light(pick);
                PunctualIncidence incidence = punctual_incidence(light, rayOrigin);
                if (incidence.attenuation > 0.0 && dot(shadingNormal, incidence.l) > 0.0
                    && !occluded_inline(rayOrigin, incidence.l, incidence.distance * TRACE_LIGHT_SPAN))
                {
                    float3 lightRadiance = light.radiance * incidence.attenuation * float(punctualCount);
                    radiance += throughput * brdf_direct(shadingNormal, V, incidence.l, albedo, metallic, roughness, f0, lightRadiance, energy);
                }
            }

            // One-sample MIS over both lobes, weighted by brdf_sampled's f * n.l over the mixture's density.
            Bounce next = sample_bounce(shadingNormal, faceNormal, V, albedo, metallic, roughness, energy, rngState);
            // A direction below either surface ends the path: past the triangle, a tilted normal would leak light.
            if (all(next.weight == float3(0.0)))
            {
                break;
            }
            throughput *= next.weight;
            // sample_bounce normalizes it, which TMin and TMax assume.
            rayDirection = next.direction;
        }

        // One NaN or Inf sample would poison the running mean until the next reset.
        summedColor += hdr_safe(radiance);
    }

    float3 avgColor = summedColor / float(numSamples);

    // A whole extra primary ray per pixel, so only when the clouds read it.
    if (pc_ray.trace_cloud_depth != 0u)
    {
        tracedDepth[tid.xy] = primary_hit_depth(tid.xy, resolution);
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
