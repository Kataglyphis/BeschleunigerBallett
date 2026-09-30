// Color math shared by both renderers (SPIR-V for C++/Vulkan, WGSL for Rust/WebGPU).

// ACES filmic tonemap (Narkowicz 2015).
float3 aces_tonemap(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), float3(0.0), float3(1.0));
}

// Exact IEC 61966-2-1 sRGB encode, not pow(x, 1/2.2), which is visibly wrong in the darks (docs/webgpu-srgb-audit.md).
float3 linear_to_srgb(float3 c)
{
    float3 low = c * 12.92;
    float3 high = 1.055 * pow(max(c, float3(0.0)), float3(1.0 / 2.4)) - 0.055;
    return select(c < float3(0.0031308), low, high);
}