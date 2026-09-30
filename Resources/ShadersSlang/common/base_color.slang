import scene_types;

// glTF base colour = baseColorFactor (material.diffuse) * sampled texture; binding-free so every path can import it.
float3 base_color(ObjMaterial material, float3 sampled)
{
    return sampled * material.diffuse;
}

// glTF KHR_texture_transform: applies the T*R*S UV matrix, given as its two rows.
float2 transform_uv(float2 uv, float3 row0, float3 row1)
{
    return float2(dot(float3(uv, 1.0), row0), dot(float3(uv, 1.0), row1));
}

// Base-colour slot only; the other slots use their own named accessors below.
float2 transform_uv(float2 uv, ObjMaterial material)
{
    return transform_uv(uv, material.uv_transform_row0, material.uv_transform_row1);
}

// Normal slot's KHR_texture_transform.
float2 normal_uv(float2 uv, ObjMaterial material)
{
    return transform_uv(uv, material.normal_uv_transform_row0, material.normal_uv_transform_row1);
}

// Metallic-roughness slot's KHR_texture_transform.
float2 metallic_roughness_uv(float2 uv, ObjMaterial material)
{
    return transform_uv(uv, material.metallic_roughness_uv_transform_row0, material.metallic_roughness_uv_transform_row1);
}

// Emissive slot's KHR_texture_transform.
float2 emissive_uv(float2 uv, ObjMaterial material)
{
    return transform_uv(uv, material.emissive_uv_transform_row0, material.emissive_uv_transform_row1);
}
