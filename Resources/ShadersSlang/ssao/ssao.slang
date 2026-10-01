import fullscreen;

// Rust/WebGPU depth-only SSAO: the checked-in ssao.wgsl is generated from this file.

struct SsaoUniforms
{
    float4x4 proj;
    float4x4 inv_proj;
    float4 params; // x: radius, y: bias, z: intensity, w: unused
};
[vk::binding(0, 0)] Texture2D<float> depthTex;
[vk::binding(1, 0)] ConstantBuffer<SsaoUniforms> u;

static const int KERNEL_SIZE = 12;
static const float3 KERNEL[12] = {
    float3(0.204, 0.104, 0.301),
    float3(-0.322, 0.170, 0.222),
    float3(0.109, -0.372, 0.284),
    float3(-0.114, -0.184, 0.451),
    float3(0.451, 0.322, 0.339),
    float3(-0.532, -0.281, 0.240),
    float3(0.320, -0.564, 0.353),
    float3(-0.310, 0.610, 0.346),
    float3(0.671, -0.166, 0.474),
    float3(-0.144, -0.702, 0.482),
    float3(-0.658, 0.397, 0.514),
    float3(0.442, 0.679, 0.531),
};

float load_depth(float2 uv)
{
    uint w, h;
    depthTex.GetDimensions(w, h);
    int2 coords = clamp(int2(uv * float2(float(w), float(h))), int2(0, 0), int2(int(w), int(h)) - 1);
    return depthTex.Load(int3(coords, 0));
}

float3 view_pos_at(float2 uv, float depth)
{
    float4 ndc = float4(fullscreen_uv_to_ndc(uv), depth, 1.0);
    float4 v = mul(u.inv_proj, ndc);
    return v.xyz / v.w;
}

[shader("vertex")]
FullscreenVsOut vs_main(uint vid : SV_VertexID)
{
    return fullscreen_vs(vid);
}

[shader("fragment")]
float4 fs_ssao(FullscreenVsOut In) : SV_Target
{
    float depth = load_depth(In.uv);
    if (depth >= 1.0) return float4(1.0);

    float3 p = view_pos_at(In.uv, depth);
    uint w, h;
    depthTex.GetDimensions(w, h);
    float2 du = float2(1.0 / float(w), 0.0);
    float2 dv = float2(0.0, 1.0 / float(h));
    float3 px = view_pos_at(In.uv + du, load_depth(In.uv + du));
    float3 py = view_pos_at(In.uv + dv, load_depth(In.uv + dv));
    // view_pos_at flips uv.y, so this operand order yields the camera-facing +Z normal.
    float3 n = normalize(cross(py - p, px - p));

    float3 up = float3(0.0, 1.0, 0.0);
    if (abs(n.y) > 0.98) up = float3(1.0, 0.0, 0.0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);

    float radius = u.params.x;
    float bias = u.params.y;
    float occlusion = 0.0;
    for (int i = 0; i < KERNEL_SIZE; i++)
    {
        float3 k = KERNEL[i];
        float3 dir = t * k.x + b * k.y + n * k.z;
        float3 samplePos = p + dir * radius;
        float4 clip = mul(u.proj, float4(samplePos, 1.0));
        if (clip.w <= 0.0) continue;
        float2 sampleUV = float2(
            (clip.x / clip.w) * 0.5 + 0.5,
            0.5 - (clip.y / clip.w) * 0.5);
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) continue;
        float sceneZ = view_pos_at(sampleUV, load_depth(sampleUV)).z;
        float rangeCheck = smoothstep(0.0, 1.0, radius / max(abs(p.z - sceneZ), 1e-4));
        if (sceneZ >= samplePos.z + bias) occlusion += rangeCheck;
    }
    float ao = 1.0 - u.params.z * (occlusion / float(KERNEL_SIZE));
    return float4(clamp(ao, 0.0, 1.0));
}

[vk::binding(2, 0)] Texture2D<float4> aoTex;

[shader("fragment")]
float4 fs_blur(FullscreenVsOut In) : SV_Target
{
    uint w, h;
    aoTex.GetDimensions(w, h);
    int2 center = int2(In.uv * float2(float(w), float(h)));
    int2 maxCoord = int2(int(w), int(h)) - 1;
    float total = 0.0;
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            int2 c = clamp(center + int2(x, y), int2(0, 0), maxCoord);
            total += aoTex.Load(int3(c, 0)).r;
        }
    }
    return float4(total / 9.0);
}