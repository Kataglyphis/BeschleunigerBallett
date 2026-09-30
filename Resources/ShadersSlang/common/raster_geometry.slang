import scene_types;
import push_constants;

// One vertex stage for forward and deferred: their parity golden compares the outputs pixel for pixel.

struct RasterVsOut
{
    float4 svPosition : SV_Position;
    float2 texCoords : TEXCOORD0;
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
    float4 tangent)
{
    RasterVsOut o;
    o.svPosition = mul(mul(mul(globalUBO.projection, globalUBO.view), pc.model), float4(position, 1.0));
    o.worldPosition = mul(pc.model, float4(position, 1.0)).xyz;
    o.shadingNormal = transform_normal(pc, normal);
    // A tangent is a surface direction, so it takes the model matrix, not the inverse-transpose.
    o.worldTangent = float4(normalize(mul(pc.model, float4(tangent.xyz, 0.0)).xyz), tangent.w);
    o.texCoords = texCoords;
    o.fragmentColor = color;
    return o;
}
