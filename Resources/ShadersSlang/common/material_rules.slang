import gltf_material;
import scene_types;

// Binding-free, so a shader with its own objectDescription binding can import it without a conflict.

// The roughness factor; a negative one (an OBJ material without Pr) maps from shininess.
float roughness_factor(ObjMaterial material)
{
    return material.roughness >= 0.0 ? material.roughness : sqrt(2.0 / (material.shininess + 2.0));
}

// glTF 2.0 SS3.9.2 through gltf_material.slang: B is metallic and G roughness, each times its factor.
float2 material_metallic_roughness(ObjMaterial material, float4 mrSample)
{
    return float2(material_metallic(material.metallic, mrSample.b), material_roughness(roughness_factor(material), mrSample.g));
}

// glTF MASK on baseColorFactor.a * texture.a * COLOR_0.a; the alpha mode decides, never the cutoff's sign.
bool material_masked_out(ObjMaterial material, float sampledAlpha)
{
    return alpha_masked_out(material.alphaMode, sampledAlpha * material.dissolve, material.alphaCutoff);
}

// glTF BLEND's alpha, baseColorFactor.a * texture.a * COLOR_0.a; 1 for OPAQUE and MASK, which cover their pixel.
float blend_coverage(ObjMaterial material, float sampledAlpha)
{
    return material.alphaMode == ALPHA_BLEND ? saturate(sampledAlpha * material.dissolve) : 1.0;
}
