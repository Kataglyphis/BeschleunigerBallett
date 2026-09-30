import scene_types;
import rt_types;

[vk::binding(globalUBO_BINDING, 0)] ConstantBuffer<GlobalUBO> globalUBO;

[vk::binding(TLAS_BINDING, 1)] RaytracingAccelerationStructure TLAS;
[vk::binding(OUT_IMAGE_BINDING, 1)] RWTexture2D<float4> image;

[[vk::push_constant]] PushConstantRaytracing pc_ray;

[shader("raygeneration")]
void rgen_main()
{
    float2 pixelCenter = float2(DispatchRaysIndex().xy) + float2(0.5);
    float2 pixelCenterUV = pixelCenter / float2(DispatchRaysDimensions().xy);
    float2 pixelCenterCS = pixelCenterUV * 2.0 - 1.0;

    // Camera ray from inv_view/inv_projection (precomputed on CPU).
    float4 origin = mul(globalUBO.inv_view, float4(0, 0, 0, 1));
    float4 target = mul(globalUBO.inv_projection, float4(pixelCenterCS.x, pixelCenterCS.y, 1, 1));
    float4 direction = mul(globalUBO.inv_view, float4(normalize(target.xyz), 0));

    HitPayload payload;
    payload.hit_value = float3(0);
    payload.is_hit = 0.0;

    RayDesc ray;
    ray.Origin = origin.xyz;
    ray.Direction = direction.xyz;
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    // Not RAY_FLAG_FORCE_OPAQUE: that would skip raytrace.rahit.slang's MASK alpha test for primary rays.
    TraceRay(TLAS,
        RAY_FLAG_NONE,
        0xFFu,      // cull mask
        0u,         // sbt record offset
        0u,         // record stride
        0u,         // miss index
        ray,
        payload);

    image[DispatchRaysIndex().xy] = float4(payload.hit_value, payload.is_hit);
}