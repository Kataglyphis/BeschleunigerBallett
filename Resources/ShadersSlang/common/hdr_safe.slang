// The C++ renderer's HDR targets are RGBA16F; every pass that writes one ends in hdr_safe().

// Below fp16's 65504, as OxidANT's forward pass: a larger radiance stores as +Inf, and ACES turns Inf into NaN.
static const float HDR_MAX = 65000.0;

// NaN slips through clamp(), whose min and max may return either operand, so it is zeroed first.
float3 hdr_safe(float3 radiance)
{
    return clamp(select(isnan(radiance), float3(0.0), radiance), float3(0.0), float3(HDR_MAX));
}
