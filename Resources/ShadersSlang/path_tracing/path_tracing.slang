import scene_types;
import ambient_maps;
import camera_ray;
import drawn_sky;
import hdr_safe;
import path_estimator;
import path_integrator;
import punctual_light;
import punctual_lights;
import sky_model;
import trace_constants;
import traced_hit;

// No uniform twin: a device without non-uniform indexing gets no ray tracing (VulkanDevice.cpp).
export static const bool kUniformTextureIndex = false;

// Ray-query path tracer over OxidANT's path_integrator.slang, SPIR-V only (WebGPU has no ray query). See docs/source/path-tracing.md.

struct PushConstantPathTracing
{
    float4 clearColor;
    uint width;
    uint height;
    // The dispatch's tile of the frame, in pixels.
    uint tile_x;
    uint tile_y;
    uint tile_width;
    uint tile_height;
    // The first of this dispatch's samples among the frame's, the samples it adds, and the samples a pixel held before.
    uint first_sample;
    uint samples;
    uint accumulated;
    uint max_bounces;
    // Furnace mode's white surface: its metallic and roughness, for the dielectric and the metal furnace alike.
    float furnace_metallic;
    float furnace_roughness;
    // Nonzero when the clouds read this frame's traced depth.
    uint trace_cloud_depth;
    // Nonzero while the skybox is on: a camera ray that misses sees its cube map, else the disabled sky's navy.
    uint skybox_enabled;
};

[vk::binding(globalUBO_BINDING, 0)] ConstantBuffer<GlobalUBO> globalUBO;
[vk::binding(sceneUBO_BINDING, 0)] ConstantBuffer<SceneUBO> sceneUBO;

[vk::binding(TLAS_BINDING, 1)] RaytracingAccelerationStructure TLAS;
[vk::binding(OUT_IMAGE_BINDING, 1)] RWTexture2D<float4> image;
[vk::binding(ACCUMULATION_IMAGE_BINDING, 1)] RWTexture2D<float4> accumulationImage;
[vk::binding(TRACED_DEPTH_IMAGE_BINDING, 1)] RWTexture2D<float> tracedDepth;
[vk::binding(SKYBOX_CUBEMAP_BINDING, 1)] SamplerCube skyboxCubemap;

[[vk::push_constant]] PushConstantPathTracing pc_ray;

// The engine's scene for path_integrator.slang: ray queries against the TLAS, the drawn sky, the IBL, the raster's lights.
struct EnginePathScene : ITracedScene
{
    bool closest(TracedSegment segment, out TracedSurface s)
    {
        RayQuery<0> query;
        RayDesc ray;
        ray.Origin = segment.origin;
        ray.Direction = segment.direction;
        ray.TMin = segment.tmin;
        ray.TMax = segment.tmax;
        query.TraceRayInline(TLAS, 0, 0xFFu, ray);
        commit_unmasked_candidates(query);
        bool found = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
        s = found
            ? traced_surface(query.CommittedInstanceID() + query.CommittedGeometryIndex(), query.CommittedPrimitiveIndex(),
                query.CommittedTriangleBarycentrics(), query.CommittedObjectToWorld3x4(), query.CommittedWorldToObject3x4(),
                query.CommittedTriangleFrontFace(), segment.direction)
            : no_surface();
        return found;
    }
    bool visible(TracedSurface s, float3 l, float distance)
    {
        // Not forced opaque: the search ends at the first committed hit, so masked-out candidates are skipped.
        RayQuery<0> query;
        RayDesc ray;
        ray.Origin = s.position + s.face_normal * TRACE_SURFACE_OFFSET;
        ray.Direction = l;
        ray.TMin = TRACE_TMIN;
        ray.TMax = min(distance, TRACE_TMAX);
        query.TraceRayInline(TLAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, ray);
        commit_unmasked_candidates(query);
        return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
    }
    float3 camera_sky(float3 dir)
    {
        // The engine's sky is always its cube map, the photo faces or AnalyticSky.hpp's bake, or the navy when off.
        float3 cube = skyboxCubemap.SampleLevel(normalize(dir), 0.0).rgb;
        return drawn_sky(pc_ray.skybox_enabled != 0u ? 1.0 : 2.0, cube, dir, float3(0.0, 1.0, 0.0), 0.0);
    }
    float3 bounce_sky(float3 dir)
    {
        // The Sun's Ambient slider scales it, as the raster's ambient term; the baked IBL while the skybox shows.
        float3 sky = sceneUBO.iblParams.x > 0.5 ? iblPrefiltered.SampleLevel(dir, 0.0).rgb : sky_gradient(dir);
        return sky * sceneUBO.dirLight.direction.w;
    }
    float3 sun_direction()
    {
        return normalize(-sceneUBO.dirLight.direction.xyz);
    }
    float3 sun_radiance()
    {
        return sceneUBO.dirLight.color.rgb * sceneUBO.dirLight.color.w;
    }
    uint punctual_count()
    {
        return punctual_light_count();
    }
    PunctualLight punctual(uint index)
    {
        return fetch_punctual_light(index);
    }
    // Furnace mode (KATAGLYPHIS_PT_FURNACE): one white surface everywhere and a uniform sky of clearColor.rgb.
    bool furnace_on()
    {
        return pc_ray.clearColor.w > 0.5;
    }
    float3 furnace_sky()
    {
        return pc_ray.clearColor.rgb;
    }
    float2 furnace_material()
    {
        return float2(pc_ray.furnace_metallic, pc_ray.furnace_roughness);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void path_tracing_main(uint3 tid : SV_DispatchThreadID)
{
    // One tile of the frame per dispatch (PathTracingDispatch.hpp's plan), so none outlasts a GPU watchdog.
    if (tid.x >= pc_ray.tile_width || tid.y >= pc_ray.tile_height)
        return;
    uint2 pixel = uint2(pc_ray.tile_x, pc_ray.tile_y) + tid.xy;
    uint2 resolution = uint2(pc_ray.width, pc_ray.height);
    uint pixelIndex = resolution.x * pixel.y + pixel.x;

    float4x4 clipToWorld = clip_to_world(globalUBO);
    EnginePathScene scene;

    // The clouds' depth: the unjittered centre ray, through the frame's first sample's pass, and only when they read it.
    bool depthWanted = pc_ray.trace_cloud_depth != 0u && pc_ray.first_sample == 0u;
    CameraRay centre = camera_ray_through_pixel(clipToWorld, float2(pixel) + 0.5, float2(resolution));
    PathDepth probe = no_depth();
    probe.wanted = depthWanted;
    probe.ray.origin = centre.origin;
    probe.ray.tmin = 0.0;
    probe.ray.direction = centre.direction;
    probe.ray.tmax = centre.length;

    float3 summedColor = float3(0.0);
    for (uint sampleIdx = 0u; sampleIdx < pc_ray.samples; sampleIdx++)
    {
        // The sample's index in the mean picks its stream, so tiles and chunks never change what a pixel draws.
        uint rngState = path_rng_seed(pixelIndex, pc_ray.accumulated + sampleIdx);
        // Jitter within the pixel so accumulation anti-aliases edges instead of only denoising.
        float2 pixelCenter = float2(pixel) + float2(rng_next(rngState), rng_next(rngState));
        // The camera segment runs from the near plane to the far plane, as raster sees; later ones leave a surface.
        CameraRay camera = camera_ray_through_pixel(clipToWorld, pixelCenter, float2(resolution));
        TracedSegment segment;
        segment.origin = camera.origin;
        segment.tmin = 0.0;
        segment.direction = camera.direction;
        segment.tmax = camera.length;
        // One NaN or Inf sample would poison the running mean until the next reset.
        summedColor += hdr_safe(trace_path(scene, segment, pc_ray.max_bounces, rngState, probe));
        probe.wanted = false;
    }

    if (depthWanted)
    {
        float4 clip = mul(globalUBO.projection, mul(globalUBO.view, float4(probe.position, 1.0)));
        // 1 on a miss, like a cleared depth buffer.
        tracedDepth[pixel] = probe.hit ? clip.z / clip.w : 1.0;
    }

    // The running mean weighs every sample alike, however the frames and their dispatches split them.
    float before = float(pc_ray.accumulated);
    float3 previous = pc_ray.accumulated > 0u ? accumulationImage[pixel].rgb : float3(0.0);
    float3 mean = (previous * before + summedColor) / (before + float(pc_ray.samples));
    accumulationImage[pixel] = float4(mean, 1.0);
    image[pixel] = float4(hdr_safe(mean), 1.0);
}
