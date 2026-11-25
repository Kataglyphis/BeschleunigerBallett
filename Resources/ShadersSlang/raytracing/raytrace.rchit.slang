import scene_types;
import rt_types;
import ambient_maps;
import brdf;
import path_integrator;
import punctual_light;
import punctual_lights;
import surface_lighting;
import trace_constants;
import traced_hit;

[vk::binding(sceneUBO_BINDING, 0)] ConstantBuffer<SceneUBO> sceneUBO;

[vk::binding(TLAS_BINDING, 1)] RaytracingAccelerationStructure TLAS;

[[vk::push_constant]] PushConstantRaytracing pc_ray;

// A shadow ray towards `direction`; true when anything lies within `tMax`. The shadow miss clears the payload.
bool occluded(float3 origin, float3 direction, float tMax)
{
    RayDesc shadowRay;
    shadowRay.Origin = origin;
    shadowRay.Direction = direction;
    shadowRay.TMin = TRACE_TMIN;
    shadowRay.TMax = tMax;
    bool shadowed = true;
    TraceRay(TLAS,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
        0xFFu,
        0u, 0u, 1u,   // sbt offset, stride, miss index (shadow miss)
        shadowRay,
        shadowed);
    return shadowed;
}

[shader("closesthit")]
void rchit_main(
    inout HitPayload payload,
    in BuiltInTriangleIntersectionAttributes attribs)
{
    // InstanceID() is the model's first-mesh index (ASManager::createTLAS); GeometryIndex() the mesh within it.
    TracedSurface s = traced_surface(InstanceID() + GeometryIndex(), PrimitiveIndex(), attribs.barycentrics,
        ObjectToWorld3x4(), WorldToObject3x4(), HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE, WorldRayDirection());

    // The clouds' depth needs every primary hit's distance, an unlit one's too.
    payload.hit_t = RayTCurrent();
    payload.is_hit = 1.0;

    // KHR_materials_unlit: return before any lighting or shadow ray.
    if (s.unlit)
    {
        payload.hit_value = s.albedo;
        return;
    }

    float3 V = -WorldRayDirection();
    float3 N = s.normal;
    // Shadow rays leave the triangle's own plane, as the path tracer's do.
    float3 shadowOrigin = s.position + s.face_normal * TRACE_SURFACE_OFFSET;
    float3 L = normalize(-sceneUBO.dirLight.direction.xyz);
    float3 f0 = surface_f0(s.albedo, s.metallic);
    // The fit once per hit, not once per light: the sun, every punctual light and the ambient share it.
    float2 energy = ggx_energy(dot(N, V), s.roughness);

    payload.hit_value = float3(0.0);
    if (dot(N, L) > 0.0 && !occluded(shadowOrigin, L, TRACE_TMAX))
    {
        payload.hit_value += brdf_direct(N, V, L, s.albedo, s.metallic, s.roughness, f0,
            sceneUBO.dirLight.color.rgb * sceneUBO.dirLight.color.w, energy);
    }

    // Each punctual light with its own shadow ray, which stops short of the light.
    uint punctualCount = punctual_light_count();
    for (uint i = 0u; i < punctualCount; i++)
    {
        PunctualLight light = fetch_punctual_light(i);
        PunctualIncidence incidence = punctual_incidence(light, s.position);
        if (incidence.attenuation <= 0.0 || dot(N, incidence.l) <= 0.0)
            continue;
        if (occluded(shadowOrigin, incidence.l, incidence.distance * TRACE_LIGHT_SPAN))
            continue;
        payload.hit_value += brdf_direct(N, V, incidence.l, s.albedo, s.metallic, s.roughness, f0,
            light.radiance * incidence.attenuation, energy);
    }

    // As the raster passes add it, unshadowed: the sky lights what the sun cannot reach, as far as occlusion lets it.
    float skyAmbient = sceneUBO.dirLight.direction.w * s.occlusion;
    payload.hit_value += skyAmbient * scene_sky_ambient(sceneUBO, N, V, s.albedo, s.metallic, s.roughness, f0, energy);

    // Emissive is added after shadowing: emitted light is not shadowed.
    payload.hit_value += s.emissive;
}
