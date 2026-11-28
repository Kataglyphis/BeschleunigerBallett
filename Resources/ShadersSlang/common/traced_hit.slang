import scene_types;
import alpha_test;
import base_color;
import material_fetch;
import material_textures;
import gltf_material;
import path_integrator;

// The C++ ray-traced hit as both raytrace.rchit.slang and path_tracing.slang shade it, and the ray queries' MASK test.

// Commits every candidate triangle a MASK cut-out keeps: with no any-hit stage, a ray query alpha-tests its candidates here.
void commit_unmasked_candidates(inout RayQuery<0> query)
{
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint candidateObjIndex = query.CandidateInstanceID() + query.CandidateGeometryIndex();
            ObjectDescription candidateObj = fetch_object_description(candidateObjIndex);
            if (!ray_hit_masked_out(candidateObj, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics()))
            {
                query.CommitNonOpaqueTriangleHit();
            }
        }
    }
}

// A committed hit's surface at level 0; objIndex must add the geometry index, or a multi-mesh hit reads mesh 0.
TracedSurface traced_surface(uint objIndex, uint primitiveID, float2 bary, float3x4 objToWorld, float3x4 worldToObject,
    bool frontFace, float3 rayDirection)
{
    ObjectDescription obj = fetch_object_description(objIndex);
    Vertices* vertices = (Vertices*)obj.vertex_address;
    Indices* indices = (Indices*)obj.index_address;

    int3 tri = indices->i[primitiveID];
    Vertex v0 = vertices->v[tri.x];
    Vertex v1 = vertices->v[tri.y];
    Vertex v2 = vertices->v[tri.z];

    float3 barycentrics = float3(1.0 - bary.x - bary.y, bary.x, bary.y);

    float3 objectPos = v0.position * barycentrics.x + v1.position * barycentrics.y + v2.position * barycentrics.z;
    // float3x4 is 3 rows × 4 cols; mul(float3x4, float4) = float3.
    float3 hitWorldPosition = mul(objToWorld, float4(objectPos, 1.0));

    float3 normalHit = v0.normal * barycentrics.x + v1.normal * barycentrics.y + v2.normal * barycentrics.z;
    // Inverse-transpose = row-multiply worldToObject: mul(v, M), not mul(M, v).
    float3 authoredNormal = normalize(mul(normalHit, (float3x3)worldToObject));
    // The winding decides the side, as SV_IsFrontFace does in raster; near a silhouette the vertex normal can disagree.
    float3 hitWorldNormal = frontFace ? authoredNormal : -authoredNormal;
    // The triangle's own plane, facing the ray: what the next ray must leave from and stay above.
    float3 triangleNormal = mul(cross(v1.position - v0.position, v2.position - v0.position), (float3x3)worldToObject);
    float3 faceNormal = dot(triangleNormal, triangleNormal) > 1e-30 ? normalize(triangleNormal) : hitWorldNormal;
    faceNormal = dot(faceNormal, rayDirection) <= 0.0 ? faceNormal : -faceNormal;

    float4 texCoords = uv_sets(v0.texture_coords, v0.texture_coords1) * barycentrics.x
                     + uv_sets(v1.texture_coords, v1.texture_coords1) * barycentrics.y
                     + uv_sets(v2.texture_coords, v2.texture_coords1) * barycentrics.z;

    ObjMaterial material = fetch_material(obj, primitiveID);

    // The mapped normal shades as raster's does; the triangle's face normal offsets the next ray and bounds it.
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
    if (material.textureID >= 0)
    {
        int textureId = resolve_texture_slot(obj, material);
        // Explicit LOD: compute and closesthit have no derivatives, so an implicit-LOD .Sample() fails SPIR-V validation.
        albedo = base_color(material, textures[material_texture_index(textureId)].SampleLevel(textureSamplers[material_texture_index(textureId)], base_color_uv(texCoords, material), 0.0).xyz);
    }
    else
    {
        albedo = material.diffuse;
    }
    float4 vertexColor = v0.color * barycentrics.x + v1.color * barycentrics.y + v2.color * barycentrics.z;
    albedo *= vertexColor.rgb;

    // Factor times texture, as raster reads them; explicit LOD as above.
    float2 metallicRoughness = resolved_metallic_roughness_lod0(obj, material, texCoords);

    TracedSurface s;
    s.position = hitWorldPosition;
    s.face_normal = faceNormal;
    s.normal = shadingNormal;
    s.albedo = albedo;
    s.emissive = resolved_emission_lod0(obj, material, texCoords);
    s.metallic = metallicRoughness.x;
    s.roughness = metallicRoughness.y;
    s.occlusion = resolved_occlusion_lod0(obj, material, texCoords);
    // KHR_materials_unlit ends shading at its albedo, before metallic can darken it.
    s.unlit = material.unlit != 0;
    return s;
}
