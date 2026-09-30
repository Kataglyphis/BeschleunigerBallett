// Rust/WebGPU occlusion-query boxes: the checked-in occlusion_bbox.wgsl is generated from this file.

struct OcclusionUniforms
{
    float4x4 view_proj;
};
[vk::binding(0, 0)] ConstantBuffer<OcclusionUniforms> uniforms;

struct BboxInstance
{
    float3 aabb_min : POSITION;
    float3 aabb_max : INSTANCE0;
};

static const uint corners[36] = {
    0u, 2u, 1u, 1u, 2u, 3u,  // -Z
    4u, 5u, 6u, 5u, 7u, 6u,  // +Z
    0u, 1u, 4u, 1u, 5u, 4u,  // -Y
    2u, 6u, 3u, 3u, 6u, 7u,  // +Y
    0u, 4u, 2u, 2u, 4u, 6u,  // -X
    1u, 3u, 5u, 3u, 7u, 5u,  // +X
};

[shader("vertex")]
float4 vs_main(uint vertexIndex : SV_VertexID, BboxInstance instance) : SV_Position
{
    uint c = corners[vertexIndex];
    float3 sel = float3(float(c & 1u), float((c >> 1u) & 1u), float((c >> 2u) & 1u));
    // The margin stops a box z-fighting its own front face; forward.rs's aabb_contains_point must use the same one.
    float3 half3 = (instance.aabb_max - instance.aabb_min) * 0.5;
    float3 margin = half3 * 0.02 + float3(0.01);
    float3 expandedMin = instance.aabb_min - margin;
    float3 expandedMax = instance.aabb_max + margin;
    float3 pos = lerp(expandedMin, expandedMax, sel);
    return mul(uniforms.view_proj, float4(pos, 1.0));
}

// Empty: the pass has no color attachments, but WebGPU still wants a fragment stage.
[shader("fragment")]
void fs_main()
{
}