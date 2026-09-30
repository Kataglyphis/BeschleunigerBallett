import fullscreen;

// Rust/WebGPU split-sum IBL precompute: the checked-in ibl.wgsl is generated from this file.

static const float PI = 3.14159265359;
static const uint IRRADIANCE_PHI_STEPS = 128u;
static const uint IRRADIANCE_THETA_STEPS = 64u;

struct IblParams
{
    float4 face_roughness_samples_mip; // x: face, y: roughness, z: samples, w: mip
    float4 source_resolution;
};
[vk::binding(0, 0)] ConstantBuffer<IblParams> params;
[vk::binding(1, 0)] Texture2D<float4> srcEquirect;
[vk::binding(2, 0)] TextureCube<float4> srcCube;
[vk::binding(3, 0)] SamplerState srcSampler;

[shader("vertex")]
FullscreenVsOut vs_fullscreen(uint vid : SV_VertexID)
{
    // IBL uses a different fullscreen triangle winding than fullscreen_vs.
    float x = float((vid << 1u) & 2u);
    float y = float(vid & 2u);
    FullscreenVsOut o;
    o.svPosition = float4(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
    o.uv = float2(x, y);
    return o;
}

float3 cube_direction(uint face, float2 uv)
{
    float a = 2.0 * uv.x - 1.0;
    float b = 1.0 - 2.0 * uv.y;
    float3 dir;
    switch (face)
    {
        case 0u: dir = float3(1.0, b, -a); break;
        case 1u: dir = float3(-1.0, b, a); break;
        case 2u: dir = float3(a, 1.0, -b); break;
        case 3u: dir = float3(a, -1.0, b); break;
        case 4u: dir = float3(a, b, 1.0); break;
        default: dir = float3(-a, b, -1.0); break;
    }
    return normalize(dir);
}

float2 equirect_uv(float3 dir)
{
    float longitude = atan2(dir.z, dir.x);
    float latitude = asin(clamp(dir.y, -1.0, 1.0));
    return float2(longitude / (2.0 * PI) + 0.5, 0.5 - latitude / PI);
}

// Manual bilinear Load: the source is bound non-filterable (no FLOAT32_FILTERABLE), and longitude must wrap.
float3 sample_equirect(float3 dir)
{
    uint dimsX, dimsY;
    srcEquirect.GetDimensions(dimsX, dimsY);
    int2 dims = int2(int(dimsX), int(dimsY));
    float2 texel = equirect_uv(dir) * float2(dims) - 0.5;
    float2 base = floor(texel);
    float2 frac = texel - base;

    int x0 = int(base.x);
    int y0 = int(base.y);
    int xa = ((x0 % dims.x) + dims.x) % dims.x;
    int xb = (((x0 + 1) % dims.x) + dims.x) % dims.x;
    int ya = clamp(y0, 0, dims.y - 1);
    int yb = clamp(y0 + 1, 0, dims.y - 1);

    float3 c00 = srcEquirect.Load(int3(xa, ya, 0)).rgb;
    float3 c10 = srcEquirect.Load(int3(xb, ya, 0)).rgb;
    float3 c01 = srcEquirect.Load(int3(xa, yb, 0)).rgb;
    float3 c11 = srcEquirect.Load(int3(xb, yb, 0)).rgb;
    return lerp(lerp(c00, c10, frac.x), lerp(c01, c11, frac.x), frac.y);
}

[shader("fragment")]
float4 fs_equirect_to_cube(FullscreenVsOut In) : SV_Target
{
    float3 dir = cube_direction(uint(params.face_roughness_samples_mip.x), In.uv);
    return float4(sample_equirect(dir), 1.0);
}

// A texel sits on the corner of its four parents, so one bilinear tap is their box average.
[shader("fragment")]
float4 fs_downsample_cube(FullscreenVsOut In) : SV_Target
{
    float3 dir = cube_direction(uint(params.face_roughness_samples_mip.x), In.uv);
    return srcCube.SampleLevel(srcSampler, dir, params.face_roughness_samples_mip.w);
}

[shader("fragment")]
float4 fs_irradiance(FullscreenVsOut In) : SV_Target
{
    float3 dir = cube_direction(uint(params.face_roughness_samples_mip.x), In.uv);
    float3 up = abs(dir.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 right = normalize(cross(up, dir));
    up = cross(dir, right);
    float3 irradiance = float3(0.0);
    for (uint phi_i = 0u; phi_i < IRRADIANCE_PHI_STEPS; phi_i++)
    {
        float phi = 2.0 * PI * (float(phi_i) + 0.5) / float(IRRADIANCE_PHI_STEPS);
        for (uint theta_i = 0u; theta_i < IRRADIANCE_THETA_STEPS; theta_i++)
        {
            float theta = 0.5 * PI * (float(theta_i) + 0.5) / float(IRRADIANCE_THETA_STEPS);
            float3 tangent = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 sampleDir = tangent.x * right + tangent.y * up + tangent.z * dir;
            irradiance += srcCube.SampleLevel(srcSampler, sampleDir, 0.0).rgb * cos(theta) * sin(theta);
        }
    }
    float sampleCount = float(IRRADIANCE_PHI_STEPS * IRRADIANCE_THETA_STEPS);
    return float4(PI * irradiance / sampleCount, 1.0);
}

// Van der Corput radical inverse, base 2.
float radical_inverse_vdc(uint bits_in)
{
    uint bits = bits_in;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 hammersley(uint i, uint count)
{
    return float2(float(i) / float(count), radical_inverse_vdc(i));
}

// GGX/Trowbridge-Reitz importance sample around `normal`.
float3 importance_sample_ggx(float2 xi, float3 normal, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    // Both clamps matter: near roughness 0 the ratio rounds above 1 and sqrt(-epsilon) NaNs drop samples.
    float cosTheta = sqrt(clamp((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y), 0.0, 1.0));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    float3 halfLocal = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(halfLocal.x * tangent + halfLocal.y * bitangent + halfLocal.z * normal);
}

float distribution_ggx(float nDotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-6);
}

// Prefiltered specular for one roughness mip, under the split-sum N = V = R simplification.
[shader("fragment")]
float4 fs_prefilter(FullscreenVsOut In) : SV_Target
{
    float3 normal = cube_direction(uint(params.face_roughness_samples_mip.x), In.uv);
    float roughness = params.face_roughness_samples_mip.y;
    uint sampleCount = uint(params.face_roughness_samples_mip.z);
    float resolution = params.source_resolution.x;

    // At roughness 0 importance sampling degenerates to identical taps; this is the exact answer.
    if (roughness <= 0.0)
    {
        return srcCube.SampleLevel(srcSampler, normal, 0.0);
    }

    float3 color = float3(0.0);
    float totalWeight = 0.0;
    for (uint i = 0u; i < sampleCount; i++)
    {
        float2 xi = hammersley(i, sampleCount);
        float3 halfVector = importance_sample_ggx(xi, normal, roughness);
        float3 sampleDir = normalize(2.0 * dot(normal, halfVector) * halfVector - normal);
        float nDotL = dot(normal, sampleDir);
        if (nDotL > 0.0)
        {
            // Krivanek/Colbert mip selection prevents fireflies; with V = N the pdf collapses to D / 4.
            float nDotH = max(dot(normal, halfVector), 0.0);
            float pdf = distribution_ggx(nDotH, roughness) * 0.25 + 1e-4;
            float texelSolidAngle = 4.0 * PI / (6.0 * resolution * resolution);
            float sampleSolidAngle = 1.0 / (float(sampleCount) * pdf);
            float mip = max(0.5 * log2(sampleSolidAngle / texelSolidAngle), 0.0);

            color += srcCube.SampleLevel(srcSampler, sampleDir, mip).rgb * nDotL;
            totalWeight += nDotL;
        }
    }
    return float4(color / max(totalWeight, 1e-4), 1.0);
}

static const uint BRDF_SAMPLE_COUNT = 1024u;

[shader("fragment")]
float4 fs_brdf_lut(FullscreenVsOut In) : SV_Target
{
    float2 uv = In.uv;
    float nDotV = uv.x;
    float roughness = uv.y;
    float3 v = float3(sqrt(1.0 - nDotV * nDotV), 0.0, nDotV);
    float3 normal = float3(0.0, 0.0, 1.0);
    float2 result = float2(0.0);
    for (uint i = 0u; i < BRDF_SAMPLE_COUNT; i++)
    {
        float2 xi = hammersley(i, BRDF_SAMPLE_COUNT);
        float3 halfVector = importance_sample_ggx(xi, normal, roughness);
        float3 light = normalize(2.0 * dot(v, halfVector) * halfVector - v);
        float nDotL = max(light.z, 0.0);
        if (nDotL > 0.0)
        {
            float nDotH = max(halfVector.z, 0.0);
            float vDotH = max(dot(v, halfVector), 0.0);
            float g = geometry_smith_ibl(max(nDotV, 1e-4), nDotL, roughness);
            float gVis = (g * vDotH) / max(nDotH * max(nDotV, 1e-4), 1e-6);
            // Fresnel as (1 - fc) * F0 + fc splits the integral into an F0 scale and a bias.
            float fc = pow(1.0 - vDotH, 5.0);
            result += float2((1.0 - fc) * gVis, fc * gVis);
        }
    }
    return float4(result / float(BRDF_SAMPLE_COUNT), 0.0, 1.0);
}

float geometry_smith_ibl(float nDotV, float nDotL, float roughness)
{
    float k = (roughness * roughness) / 2.0;
    float gv = nDotV / max(nDotV * (1.0 - k) + k, 1e-4);
    float gl = nDotL / max(nDotL * (1.0 - k) + k, 1e-4);
    return gv * gl;
}