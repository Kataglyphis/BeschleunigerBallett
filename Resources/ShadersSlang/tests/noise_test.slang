import noise;

// CI-only entry point: proves noise.slang compiles to both SPIR-V and WGSL; no renderer loads it.

RWStructuredBuffer<float> outBuf;

[shader("compute")]
[numthreads(1, 1, 1)]
void noise_test_main(uint3 tid : SV_DispatchThreadID)
{
    float3 p = float3(float(tid.x) * 0.1, 0.0, 0.0);
    float n = snoise(p);
    float f = fbm(p, 1.0, 4);
    outBuf[tid.x] = n + f;
}