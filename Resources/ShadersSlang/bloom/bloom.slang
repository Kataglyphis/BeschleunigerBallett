import fullscreen;

// Rust/WebGPU bloom: the checked-in bloom.wgsl is generated from this file.

[vk::binding(0, 0)] Texture2D<float4> srcTex;
[vk::binding(1, 0)] SamplerState srcSampler;

// [adapted EV, target EV] from cs_reduce_exposure, shared with tonemap.slang as the one exposure source.
[vk::binding(2, 0)] StructuredBuffer<float> exposureState;

static const float THRESHOLD = 1.0;

static const float W0 = 0.227027;
static const float W1 = 0.1945946;
static const float W2 = 0.1216216;
static const float W3 = 0.054054;
static const float W4 = 0.016216;

float3 blur(float2 uv, float2 dir)
{
    uint w, h;
    srcTex.GetDimensions(w, h);
    float2 texel = dir / float2(float(w), float(h));
    float3 color = srcTex.Sample(srcSampler, uv).rgb * W0;
    color += srcTex.Sample(srcSampler, uv + texel * 1.0).rgb * W1;
    color += srcTex.Sample(srcSampler, uv - texel * 1.0).rgb * W1;
    color += srcTex.Sample(srcSampler, uv + texel * 2.0).rgb * W2;
    color += srcTex.Sample(srcSampler, uv - texel * 2.0).rgb * W2;
    color += srcTex.Sample(srcSampler, uv + texel * 3.0).rgb * W3;
    color += srcTex.Sample(srcSampler, uv - texel * 3.0).rgb * W3;
    color += srcTex.Sample(srcSampler, uv + texel * 4.0).rgb * W4;
    color += srcTex.Sample(srcSampler, uv - texel * 4.0).rgb * W4;
    return color;
}

[shader("vertex")]
FullscreenVsOut vs_main(uint vid : SV_VertexID)
{
    return fullscreen_vs(vid);
}

[shader("fragment")]
float4 fs_brightpass(FullscreenVsOut In) : SV_Target
{
    float3 hdr = srcTex.Sample(srcSampler, In.uv).rgb * exp2(exposureState[0]);
    float3 bright = max(hdr - float3(THRESHOLD), float3(0.0));
    return float4(bright, 1.0);
}

[shader("fragment")]
float4 fs_blur_h(FullscreenVsOut In) : SV_Target
{
    return float4(blur(In.uv, float2(1.0, 0.0)), 1.0);
}

[shader("fragment")]
float4 fs_blur_v(FullscreenVsOut In) : SV_Target
{
    return float4(blur(In.uv, float2(0.0, 1.0)), 1.0);
}