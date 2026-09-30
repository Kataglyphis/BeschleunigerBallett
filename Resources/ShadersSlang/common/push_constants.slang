// Hand-mirrored by PushConstantRasterizer.hpp; invModelRows holds only 3 rows to fit the 128-byte push budget.
struct PushConstantRasterizer
{
    float4x4 model;
    float4 invModelRows[3];
    uint objectIndex;
};

// Equivalent to mul(fullInvTransposeModel, float4(normal, 0.0)).xyz.
float3 transform_normal(PushConstantRasterizer pc, float3 normal)
{
    return float3(
      dot(pc.invModelRows[0].xyz, normal), dot(pc.invModelRows[1].xyz, normal), dot(pc.invModelRows[2].xyz, normal));
}
