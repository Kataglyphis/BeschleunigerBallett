import scene_types;
import base_color;
import material_fetch;
import material_rules;
import material_textures;

// glTF MASK alpha test shared by raytrace.rahit.slang and path_tracing.slang's ray-query loop (no any-hit stage).

// True when the hit is a MASK material whose alpha falls below its cutoff; non-MASK (alphaCutoff < 0) never is.
bool ray_hit_masked_out(ObjectDescription obj, uint primitiveID, float2 bary)
{
    ObjMaterial material = fetch_material(obj, primitiveID);

    if (material.alphaCutoff < 0.0) { return false; }

    Vertices* vertices = (Vertices*)obj.vertex_address;
    Indices* indices = (Indices*)obj.index_address;
    int3 ind = indices->i[primitiveID];

    float3 barycentrics = float3(1.0 - bary.x - bary.y, bary.x, bary.y);

    float vertexAlpha = vertices->v[ind.x].color.a * barycentrics.x
                       + vertices->v[ind.y].color.a * barycentrics.y
                       + vertices->v[ind.z].color.a * barycentrics.z;

    float sampledAlpha = vertexAlpha;
    if (material.textureID >= 0 || material.alphaTextureID >= 0)
    {
        float2 texCoords = vertices->v[ind.x].texture_coords * barycentrics.x
                          + vertices->v[ind.y].texture_coords * barycentrics.y
                          + vertices->v[ind.z].texture_coords * barycentrics.z;
        // Explicit LOD: ray queries have no derivatives, so an implicit-LOD .Sample() fails SPIR-V validation.
        if (material.textureID >= 0)
        {
            int textureId = resolve_texture_slot(obj, material);
            sampledAlpha *= textures[textureId].SampleLevel(textureSamplers[textureId], transform_uv(texCoords, material), 0.0).a;
        }
        if (material.alphaTextureID >= 0)
        {
            sampledAlpha *= sample_alpha_lod0(obj, material, texCoords);
        }
    }

    return alpha_masked_out(material, sampledAlpha);
}
