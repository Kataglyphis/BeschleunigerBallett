// Rust/WebGPU GUI-overlay quad: the checked-in tex_quad.wgsl is generated from this file.

struct TexQuadVsOut
{
    float4 svPosition : SV_Position;
    float2 uv : TEXCOORD0;
};

[vk::binding(0, 0)] Texture2D<float4> tex;
[vk::binding(1, 0)] SamplerState samp;

[shader("vertex")]
TexQuadVsOut vs_main(float2 position : POSITION, float2 uv_in : TEXCOORD0)
{
    TexQuadVsOut o;
    o.svPosition = float4(position, 0.0, 1.0);
    o.uv = uv_in;
    return o;
}

[shader("fragment")]
float4 fs_main(TexQuadVsOut In) : SV_Target
{
    return tex.Sample(samp, In.uv);
}