// Must match ibl.rs's EnvironmentImage::sky: sky.slang, forward.slang's IBL fallback and the Rust panorama agree.

static const float3 SKY_ZENITH = float3(0.09, 0.16, 0.35);
static const float3 SKY_HORIZON = float3(0.55, 0.62, 0.72);
static const float3 SKY_GROUND = float3(0.18, 0.16, 0.15);

// Horizon/zenith/ground gradient, no sun. dir: view or reflection direction (normalized).
float3 sky_gradient(float3 dir)
{
    if (dir.y >= 0.0)
        return lerp(SKY_HORIZON, SKY_ZENITH, pow(clamp(dir.y, 0.0, 1.0), 0.7));
    else
        return lerp(SKY_HORIZON, SKY_GROUND, clamp(-dir.y * 3.0, 0.0, 1.0));
}

// Analytic sun disk, additive on top of sky_gradient; lightDir need not be normalized.
float3 sun_disk(float3 dir, float3 lightDir, float intensity)
{
    float3 l = normalize(lightDir);
    float cosSun = max(dot(dir, l), 0.0);
    float sun = pow(cosSun, 1200.0) * 24.0 + pow(cosSun, 48.0) * 0.5;
    return float3(1.0, 0.95, 0.85) * sun * (intensity * 0.4);
}

// Diffuse irradiance over the hemisphere around a surface normal, from the same gradient.
float3 hemisphere_irradiance(float3 n)
{
    float3 sky = lerp(SKY_HORIZON, SKY_ZENITH, pow(clamp(n.y, 0.0, 1.0), 0.7));
    return lerp(SKY_GROUND * 0.7, sky, clamp(n.y * 0.5 + 0.5, 0.0, 1.0));
}
