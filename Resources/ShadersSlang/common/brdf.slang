// Epic/UE4 microfacet BRDF shared by both renderers; the 1e-6 guards prevent NaN/inf at grazing angles.

static const float PI = 3.14159265358979323846;

// Lambertian diffuse.
float3 lambert_diffuse(float3 albedo)
{
    return albedo / PI;
}

// GGX (Trowbridge-Reitz) normal distribution function.
float distribution_ggx(float n_dot_h, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-6);
}

// Smith geometry function (Epic Games approximation with k = (r+1)^2/8).
float geometry_smith(float n_dot_v, float n_dot_l, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float g1 = n_dot_v / max(n_dot_v * (1.0 - k) + k, 1e-6);
    float g2 = n_dot_l / max(n_dot_l * (1.0 - k) + k, 1e-6);
    return g1 * g2;
}

// Schlick Fresnel. cos_theta: dot(H, V), f0: reflectance at normal incidence.
float3 fresnel_schlick(float cos_theta, float3 f0)
{
    return f0 + (float3(1.0) - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// Cook-Torrance specular + Lambert diffuse scaled by (1-F)(1-metallic); v and l point away from the surface.
float3 brdf_direct(
    float3 n,
    float3 v,
    float3 l,
    float3 albedo,
    float metallic,
    float roughness,
    float3 f0,
    float3 radiance)
{
    float3 h = normalize(l + v);
    float n_dot_l = max(dot(n, l), 0.0);
    float n_dot_v = max(dot(n, v), 1e-4);
    float n_dot_h = max(dot(n, h), 0.0);
    float h_dot_v = max(dot(h, v), 0.0);

    float d = distribution_ggx(n_dot_h, roughness);
    float g = geometry_smith(n_dot_v, n_dot_l, roughness);
    float3 f = fresnel_schlick(h_dot_v, f0);
    float3 specular = (d * g * f) / max(4.0 * n_dot_v * n_dot_l, 1e-6);
    float3 diffuse = (float3(1.0) - f) * (1.0 - metallic) * lambert_diffuse(albedo);
    return (diffuse + specular) * radiance * n_dot_l;
}