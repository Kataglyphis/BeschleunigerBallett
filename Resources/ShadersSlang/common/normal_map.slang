// glTF 3.9.3 normal mapping; flip geometricNormal for back faces before the call, never the result.
float3 apply_normal_map(float3 geometricNormal, float4 tangent, float3 sampledNormal, float scale = 1.0)
{
    float3 n = normalize(geometricNormal);
    float3 t = normalize(tangent.xyz - n * dot(n, tangent.xyz));
    float3 b = cross(n, t) * tangent.w;
    float3 nTs = sampledNormal * 2.0 - 1.0;
    nTs.xy *= scale;
    // Row-multiply mul(v, M): the matrix is built from rows (t, b, n).
    return normalize(mul(nTs, float3x3(t, b, n)));
}
