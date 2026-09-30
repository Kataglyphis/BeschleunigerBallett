import scene_types;

// Binding-free, so a shader with its own objectDescription binding can import it without a conflict.

// Negative roughness (every OBJ material) maps from shininess; the 0.045 floor keeps the specular lobe finite.
float material_roughness(ObjMaterial material)
{
    return material.roughness >= 0.0
      ? clamp(material.roughness, 0.045, 1.0)
      : clamp(sqrt(2.0 / (material.shininess + 2.0)), 0.045, 1.0);
}

// glTF 2.0 SS3.9.2: G = roughness, B = metallic, each multiplying its factor.
float2 material_metallic_roughness(ObjMaterial material, float4 mrSample)
{
    return float2(material.metallic * mrSample.b, material_roughness(material) * mrSample.g);
}

// glTF MASK test on baseColorFactor.a * texture.a * COLOR_0.a; a negative alphaCutoff means not MASK.
bool alpha_masked_out(ObjMaterial material, float sampledAlpha)
{
    return material.alphaCutoff >= 0.0 && (sampledAlpha * material.dissolve) < material.alphaCutoff;
}
