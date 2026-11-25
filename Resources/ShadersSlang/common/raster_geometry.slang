import scene_types;
import push_constants;
import gltf_material;
import base_color;

// One vertex stage for forward and deferred: their parity golden compares the outputs pixel for pixel.

struct RasterVsOut
{
    float4 svPosition : SV_Position;
    // xy = TEXCOORD_0, zw = TEXCOORD_1 (base_color.slang's uv_sets).
    float4 texCoords : TEXCOORD0;
    float3 shadingNormal : TEXCOORD1;
    float4 fragmentColor : TEXCOORD2;
    float3 worldPosition : TEXCOORD3;
    float4 worldTangent : TEXCOORD4;
};

RasterVsOut raster_geometry_vs(
    ConstantBuffer<GlobalUBO> globalUBO,
    PushConstantRasterizer pc,
    float3 position,
    float3 normal,
    float4 color,
    float2 texCoords,
    float4 tangent,
    float2 texCoords1)
{
    RasterVsOut o;
    o.svPosition = mul(mul(mul(globalUBO.projection, globalUBO.view), pc.model), float4(position, 1.0));
    o.worldPosition = mul(pc.model, float4(position, 1.0)).xyz;
    o.shadingNormal = mul(normal_transform((float3x3)pc.model), normal);
    // A tangent is a surface direction, so it takes the model matrix, not the inverse-transpose.
    float handedness = tangent.w * mirror_sign((float3x3)pc.model);
    o.worldTangent = float4(normalize(mul(pc.model, float4(tangent.xyz, 0.0)).xyz), handedness);
    o.texCoords = uv_sets(texCoords, texCoords1);
    o.fragmentColor = color;
    return o;
}
