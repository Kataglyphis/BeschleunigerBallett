import scene_types;
import rt_types;
import ambient;
import brdf;
import base_color;
import material_fetch;
import material_textures;
import normal_map;
import punctual_light;
import punctual_lights;
import trace_constants;

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
    uint objIndex = InstanceID() + GeometryIndex();
    ObjectDescription obj = fetch_object_description(objIndex);

    // Fetch geometry via buffer device addresses (T* = PhysicalStorageBuffer).
    Vertices* vertices = (Vertices*)obj.vertex_address;
    Indices* indices = (Indices*)obj.index_address;

    // Indices of the closest-hit triangle.
    int3 ind = indices->i[PrimitiveIndex()];

    // Vertices of the closest-hit triangle.
    Vertex v0 = vertices->v[ind.x];
    Vertex v1 = vertices->v[ind.y];
    Vertex v2 = vertices->v[ind.z];

    // Barycentric coordinates of the intersection.
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
                                 attribs.barycentrics.x,
                                 attribs.barycentrics.y);

    // Column multiply on purpose: ObjectToWorld() maps positions directly, unlike the normal below.
    float3 hitPos = v0.position * barycentrics.x + v1.position * barycentrics.y + v2.position * barycentrics.z;
    float3 worldHitPos = (float3)mul(ObjectToWorld(), float4(hitPos, 1.0));

    // Inverse-transpose = row-multiply WorldToObject: mul(v, M), not mul(M, v).
    float3 normalHit = v0.normal * barycentrics.x + v1.normal * barycentrics.y + v2.normal * barycentrics.z;
    float3 worldNormal = normalize(mul(normalHit, (float3x3)WorldToObject()));

    // The winding decides the side, as SV_IsFrontFace does in raster; near a silhouette the vertex normal can disagree.
    bool frontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
    float3 facingNormal = frontFace ? worldNormal : -worldNormal;

    // Both UV sets (base_color.slang's uv_sets).
    float4 texCoords = uv_sets(v0.texture_coords, v0.texture_coords1) * barycentrics.x
                     + uv_sets(v1.texture_coords, v1.texture_coords1) * barycentrics.y
                     + uv_sets(v2.texture_coords, v2.texture_coords1) * barycentrics.z;

    float4 vertexColor = v0.color * barycentrics.x + v1.color * barycentrics.y + v2.color * barycentrics.z;

    ObjMaterial material = fetch_material(obj, PrimitiveIndex());
    float3 albedo = float3(0.0);
    if (material.textureID >= 0)
    {
        int textureId = resolve_texture_slot(obj, material);
        // Explicit LOD: closesthit has no derivatives, so an implicit-LOD .Sample() fails SPIR-V validation.
        albedo += base_color(material, textures[NonUniformResourceIndex(textureId)].SampleLevel(textureSamplers[NonUniformResourceIndex(textureId)], base_color_uv(texCoords, material), 0.0).xyz);
    }
    else
    {
        albedo += material.diffuse;
    }
    albedo *= vertexColor.rgb;

    // The clouds' depth needs every primary hit's distance, an unlit one's too.
    payload.hit_t = RayTCurrent();

    // KHR_materials_unlit: return before any lighting or shadow ray.
    if (material.unlit != 0)
    {
        payload.hit_value = albedo;
        payload.is_hit = 1.0;
        return;
    }

    // Light direction (towards the light) and view direction (towards camera).
    float3 L = normalize(-sceneUBO.dirLight.direction.xyz);
    float3 N = facingNormal;
    if (material.normalTextureID >= 0)
    {
        // A tangent is a surface direction: ObjectToWorld(), not the inverse-transpose.
        float4 tangentHit = v0.tangent * barycentrics.x + v1.tangent * barycentrics.y + v2.tangent * barycentrics.z;
        float3 worldTangent = normalize((float3)mul(ObjectToWorld(), float4(tangentHit.xyz, 0.0)));
        // Explicit LOD: see the base-colour sample above for why.
        float handedness = mirrored_handedness(tangentHit.w, (float3x3)ObjectToWorld());
        N = resolved_normal_lod0(worldNormal, float4(worldTangent, handedness), frontFace, obj, material, texCoords);
    }
    float3 V = normalize(sceneUBO.cam_pos.xyz - worldHitPos);

    float3 shadowOrigin = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    bool shadowed = dot(facingNormal, L) <= 0 || occluded(shadowOrigin, L, TRACE_TMAX);
    // Explicit LOD: see the base-colour sample above for why.
    float2 metallicRoughness = resolved_metallic_roughness_lod0(obj, material, texCoords);
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;
    float3 lightColor = sceneUBO.dirLight.color.rgb;
    float lightIntensity = sceneUBO.dirLight.color.w;

    payload.hit_value = float3(0.0);
    payload.is_hit = 1.0;
    // The fit once per hit, not once per light: the sun, every punctual light and the ambient share it.
    float2 energy = ggx_energy(dot(N, V), roughness);
    float3 f0 = lerp(float3(0.04), albedo, metallic);

    if (!shadowed)
    {
        payload.hit_value += brdf_direct(
            N, V, L, albedo, metallic, roughness, f0,
            lightColor * lightIntensity, energy);
    }

    // Each punctual light with its own shadow ray, which stops short of the light.
    uint punctualCount = punctual_light_count();
    for (uint i = 0u; i < punctualCount; i++)
    {
        PunctualLight light = fetch_punctual_light(i);
        PunctualIncidence incidence = punctual_incidence(light, shadowOrigin);
        if (incidence.attenuation <= 0.0 || dot(facingNormal, incidence.l) <= 0.0)
            continue;
        if (occluded(shadowOrigin, incidence.l, incidence.distance * TRACE_LIGHT_SPAN))
            continue;
        payload.hit_value += brdf_direct(N, V, incidence.l, albedo, metallic, roughness, f0,
            light.radiance * incidence.attenuation, energy);
    }

    // As the raster passes add it, unshadowed: the sky lights what the sun cannot reach.
    float skyAmbient = sceneUBO.dirLight.direction.w;
    payload.hit_value += skyAmbient * analytic_ambient(N, V, albedo, metallic, roughness, f0, energy);

    // Emissive is added after shadowing (emitted light is not shadowed); explicit LOD as above.
    payload.hit_value += resolved_emission_lod0(obj, material, texCoords);
}