import aces;
import fullscreen;

// C++/Vulkan post pass (tonemap + clouds); the Rust renderer composites in tonemap.slang instead.

// Combined image+sampler (Vulkan combined image sampler descriptor).
[vk::binding(0, 0)] Sampler2D noisyTxt;
[vk::binding(1, 0)] Sampler2D cloudTxt;

struct PushConstantPost
{
    uint clouds_enabled;
};
[[vk::push_constant]] PushConstantPost pc_post;

struct VsOut
{
    float4 svPosition : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VsOut vs_main(uint vid : SV_VertexID)
{
    // Shared fullscreen-triangle vertex (no vertex buffer).
    FullscreenVsOut o = fullscreen_vs(vid);
    VsOut r;
    r.svPosition = o.svPosition;
    r.uv = o.uv;
    return r;
}

[shader("fragment")]
float4 fs_main(VsOut In) : SV_Target
{
    float4 sampled = noisyTxt.Sample(In.uv);
    float3 color = sampled.rgb;
    float  alpha = sampled.a;

    if (bool(pc_post.clouds_enabled))
    {
        float4 cloud = cloudTxt.Sample(In.uv);
        color = cloud.rgb + color * (1.0 - cloud.a);
        alpha = cloud.a + alpha * (1.0 - cloud.a);
    }

    // ACES filmic tonemap (shared with Rust WebGPU renderer via aces.slang).
    float3 tonemapped = aces_tonemap(color);

    // The swapchain is UNORM (SwapchainChoices.hpp), so the shader must encode sRGB itself.
    return float4(linear_to_srgb(tonemapped), alpha);
}