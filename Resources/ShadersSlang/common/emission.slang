import scene_types;

// glTF emissive: emissiveFactor (strength folded in by the loader) * texture; pass float3(1.0) when untextured.
float3 material_emission(ObjMaterial material, float3 sampled)
{
    return material.emission * sampled;
}
