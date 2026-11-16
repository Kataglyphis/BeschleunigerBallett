import brdf;

// CI-only entry point: proves every public brdf.slang function compiles to both SPIR-V and WGSL; no renderer loads it.

RWStructuredBuffer<float4> outBuf;

[shader("compute")]
[numthreads(1, 1, 1)]
void brdf_test_main(uint3 tid : SV_DispatchThreadID)
{
    float3 n = float3(0.0, 1.0, 0.0);
    float3 v = float3(0.0, 1.0, 0.0);
    float3 l = normalize(float3(1.0, 1.0, 0.0));
    float3 albedo = float3(0.8, 0.2, 0.1);
    float metallic = 0.0;
    float3 f0 = lerp(float3(0.04), albedo, metallic);
    float3 direct = brdf_direct(n, v, l, albedo, metallic, 0.5, f0, float3(1.0));
    float3 sampled = brdf_sampled(n, v, l, albedo, metallic, 0.5, f0);
    float masking = geometry_smith_k(dot(n, v), dot(n, l), smith_k_analytic(0.5));
    outBuf[tid.x] = float4(direct + sampled, masking);
}
