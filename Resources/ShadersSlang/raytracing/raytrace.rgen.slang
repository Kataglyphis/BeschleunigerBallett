import camera_ray;
import scene_types;
import rt_types;
import hdr_safe;

[vk::binding(globalUBO_BINDING, 0)] ConstantBuffer<GlobalUBO> globalUBO;

[vk::binding(TLAS_BINDING, 1)] RaytracingAccelerationStructure TLAS;
[vk::binding(OUT_IMAGE_BINDING, 1)] RWTexture2D<float4> image;
[vk::binding(TRACED_DEPTH_IMAGE_BINDING, 1)] RWTexture2D<float> tracedDepth;

[[vk::push_constant]] PushConstantRaytracing pc_ray;

[shader("raygeneration")]
void rgen_main()
{
    // From the near plane through the pixel centre to the far plane, where raster clips.
    CameraRay camera = camera_ray_through_pixel(clip_to_world(globalUBO), float2(DispatchRaysIndex().xy) + 0.5,
        float2(DispatchRaysDimensions().xy));

    HitPayload payload;
    payload.hit_value = float3(0);
    payload.is_hit = 0.0;
    payload.hit_t = 0.0;

    RayDesc ray;
    ray.Origin = camera.origin;
    ray.Direction = camera.direction;
    ray.TMin = 0.0;
    ray.TMax = camera.length;

    // Not RAY_FLAG_FORCE_OPAQUE: that would skip raytrace.rahit.slang's MASK alpha test for primary rays.
    TraceRay(TLAS,
        RAY_FLAG_NONE,
        0xFFu,      // cull mask
        0u,         // sbt record offset
        0u,         // record stride
        0u,         // miss index
        ray,
        payload);

    image[DispatchRaysIndex().xy] = float4(hdr_safe(payload.hit_value), payload.is_hit);

    // NDC depth for the clouds; 1 on a miss, like a cleared depth buffer.
    float tracedNdcDepth = 1.0;
    if (payload.is_hit > 0.5)
    {
        float4 clip = mul(globalUBO.projection, mul(globalUBO.view, float4(ray.Origin + ray.Direction * payload.hit_t, 1.0)));
        tracedNdcDepth = clip.z / clip.w;
    }
    tracedDepth[DispatchRaysIndex().xy] = tracedNdcDepth;
}
