import scene_types;
import rt_types;
import brdf;
import base_color;
import material_fetch;
import material_textures;

[vk::binding(sceneUBO_BINDING, 0)] ConstantBuffer<SceneUBO> sceneUBO;

[vk::binding(TLAS_BINDING, 1)] RaytracingAccelerationStructure TLAS;

[[vk::push_constant]] PushConstantRaytracing pc_ray;

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

    // faceforward: the normal must oppose the ray.
    if (dot(worldNormal, WorldRayDirection()) > 0.0)
    {
        worldNormal = -worldNormal;
    }

    // Texture coordinates.
    float2 texCoords = v0.texture_coords * barycentrics.x
                     + v1.texture_coords * barycentrics.y
                     + v2.texture_coords * barycentrics.z;

    float4 vertexColor = v0.color * barycentrics.x + v1.color * barycentrics.y + v2.color * barycentrics.z;

    ObjMaterial material = fetch_material(obj, PrimitiveIndex());
    float3 albedo = float3(0.0);
    if (material.textureID >= 0)
    {
        int textureId = resolve_texture_slot(obj, material);
        // Explicit LOD: closesthit has no derivatives, so an implicit-LOD .Sample() fails SPIR-V validation.
        albedo += base_color(material, textures[textureId].SampleLevel(textureSamplers[textureId], transform_uv(texCoords, material), 0.0).xyz);
    }
    else
    {
        albedo += material.diffuse;
    }
    albedo *= vertexColor.rgb;

    // KHR_materials_unlit: return before any lighting or shadow ray.
    if (material.unlit != 0)
    {
        payload.hit_value = albedo;
        payload.is_hit = 1.0;
        return;
    }

    // Light direction (towards the light) and view direction (towards camera).
    float3 L = normalize(-sceneUBO.dirLight.direction.xyz);
    float3 N = worldNormal;
    if (material.normalTextureID >= 0)
    {
        // A tangent is a surface direction: ObjectToWorld(), not the inverse-transpose.
        float4 tangentHit = v0.tangent * barycentrics.x + v1.tangent * barycentrics.y + v2.tangent * barycentrics.z;
        float3 worldTangent = normalize((float3)mul(ObjectToWorld(), float4(tangentHit.xyz, 0.0)));
        // Explicit LOD: see the base-colour sample above for why.
        N = resolved_normal_lod0(N, float4(worldTangent, tangentHit.w), obj, material, texCoords);
    }
    float3 V = normalize(sceneUBO.cam_pos.xyz - worldHitPos);

    // Shadow ray: trace from the hit point towards the light.
    bool shadowed = true;
    if (dot(worldNormal, L) > 0)
    {
        RayDesc shadowRay;
        shadowRay.Origin = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
        shadowRay.Direction = L;
        shadowRay.TMin = 0.001;
        shadowRay.TMax = 10000.0;

        TraceRay(TLAS,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
            0xFFu,
            0u, 0u, 1u,   // sbt offset, stride, miss index (shadow miss)
            shadowRay,
            shadowed);
    }

    // Explicit LOD: see the base-colour sample above for why.
    float2 metallicRoughness = resolved_metallic_roughness_lod0(obj, material, texCoords);
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;
    float3 lightColor = sceneUBO.dirLight.color.rgb;
    float lightIntensity = sceneUBO.dirLight.color.w;

    // Start from zero like forward and deferred: the engine has no ambient term to seed with albedo.
    payload.hit_value = float3(0.0);
    payload.is_hit = 1.0;

    if (!shadowed)
    {
        float3 f0 = lerp(float3(0.04), albedo, metallic);
        payload.hit_value += brdf_direct(
            N, V, L, albedo, metallic, roughness, f0,
            lightColor * lightIntensity);
    }

    // Emissive is added after shadowing (emitted light is not shadowed); explicit LOD as above.
    payload.hit_value += resolved_emission_lod0(obj, material, texCoords);
}