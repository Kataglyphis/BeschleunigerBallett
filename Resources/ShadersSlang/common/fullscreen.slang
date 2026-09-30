// Fullscreen triangle from the vertex index alone, shared so both renderers get identical UV math.

struct FullscreenVsOut
{
    float4 svPosition : SV_Position;
    float2 uv : TEXCOORD0;
};

// UV origin is top-left (y flipped) to match texture sampling.
FullscreenVsOut fullscreen_vs(uint vid)
{
    FullscreenVsOut o;
    float x = float(vid / 2) * 4.0 - 1.0;
    float y = float(vid % 2) * 4.0 - 1.0;
    o.svPosition = float4(x, y, 0.0, 1.0);
    o.uv = float2((x + 1.0) * 0.5, 1.0 - (y + 1.0) * 0.5);
    return o;
}