// glTF 3.9.3 normal mapping on the authored frame; a doubleSided back face reverses the whole frame (glTF 3.9.2).
float3 apply_normal_map(float3 geometricNormal, float4 tangent, float3 sampledNormal, bool frontFace, float scale = 1.0)
{
    float3 n = normalize(geometricNormal);
    float3 t = normalize(tangent.xyz - n * dot(n, tangent.xyz));
    float3 b = cross(n, t) * tangent.w;
    float3 nTs = sampledNormal * 2.0 - 1.0;
    nTs.xy *= scale;
    // Row-multiply mul(v, M): the matrix is built from rows (t, b, n).
    float3 mapped = normalize(mul(nTs, float3x3(t, b, n)));
    // Negating n before the call keeps t, which mirrored the relief along it; t, b and n must flip together.
    return frontFace ? mapped : -mapped;
}
