import scene_types;

// Binding-free, so a shader with its own objectDescription binding can import it without a conflict.

// Keeps the GGX lobe finite: at roughness 0 the path tracer's visible-normal density is 0/0.
static const float MIN_MATERIAL_ROUGHNESS = 0.045;

// Negative roughness (every OBJ material) maps from shininess.
float material_roughness(ObjMaterial material)
{
    return material.roughness >= 0.0
      ? clamp(material.roughness, MIN_MATERIAL_ROUGHNESS, 1.0)
      : clamp(sqrt(2.0 / (material.shininess + 2.0)), MIN_MATERIAL_ROUGHNESS, 1.0);
}

// glTF 2.0 SS3.9.2: G = roughness, B = metallic, each multiplying its factor; a G of 0 must not undo the floor.
float2 material_metallic_roughness(ObjMaterial material, float4 mrSample)
{
    return float2(material.metallic * mrSample.b, max(material_roughness(material) * mrSample.g, MIN_MATERIAL_ROUGHNESS));
}

// glTF MASK test on baseColorFactor.a * texture.a * COLOR_0.a; a negative alphaCutoff means not MASK.
bool alpha_masked_out(ObjMaterial material, float sampledAlpha)
{
    return material.alphaCutoff >= 0.0 && (sampledAlpha * material.dissolve) < material.alphaCutoff;
}

// glTF BLEND's alpha, baseColorFactor.a * texture.a * COLOR_0.a; 1 for OPAQUE and MASK, which cover their pixel.
float blend_coverage(ObjMaterial material, float sampledAlpha)
{
    return material.alphaBlend != 0 ? saturate(sampledAlpha * material.dissolve) : 1.0;
}
