module;

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include <glm/glm.hpp>

export module kataglyphis.vulkan.punctual_light;

export namespace Kataglyphis {

/// A glTF `KHR_lights_punctual` light posed by its node; `punctual_light.slang`'s kind codes.
enum class PunctualLightKind : unsigned int { Point = 1, Spot = 2, Directional = 3 };

/// One light in its model's space, as the glTF loader read it.
struct PunctualLight
{
    PunctualLightKind kind = PunctualLightKind::Point;
    glm::vec3 color{ 1.0F };
    float intensity = 1.0F;
    /// 0 means unbounded, as glTF's absent `range`.
    float range = 0.0F;
    glm::vec3 position{ 0.0F };
    /// Where the light points (the node's -Z); unused by a point light.
    glm::vec3 direction{ 0.0F, 0.0F, -1.0F };
    float cosInner = 1.0F;
    float cosOuter = 1.0F;
};

/// The most lights the buffer carries; Rust's `MAX_PUNCTUAL_LIGHTS`.
inline constexpr std::size_t MAX_PUNCTUAL_LIGHTS = 256;

/// Rows per light in the packed buffer, `unpack_punctual_light`'s four arguments.
inline constexpr std::size_t PUNCTUAL_LIGHT_ROWS = 4;

/// One model's lights with that model's matrix applied.
struct PlacedPunctualLights
{
    std::span<const PunctualLight> lights;
    glm::mat4 model{ 1.0F };
};

/// Row 0 holds the count in x; then four rows per light as `render/lights.rs` packs them:
/// [pos, kind], [color * intensity, range], [dir, cos_inner], [cos_outer, 0, 0, 0].
/// Never empty, so the buffer always has its header row.
std::vector<glm::vec4> packPunctualLights(std::span<const PlacedPunctualLights> models)
{
    std::vector<glm::vec4> rows(1, glm::vec4(0.0F));
    std::size_t count = 0;
    for (const PlacedPunctualLights &placed : models) {
        const glm::mat3 linear(placed.model);
        for (const PunctualLight &light : placed.lights) {
            if (count == MAX_PUNCTUAL_LIGHTS) { break; }
            const glm::vec3 position = glm::vec3(placed.model * glm::vec4(light.position, 1.0F));
            glm::vec3 direction = linear * light.direction;
            const float length = glm::length(direction);
            direction = length > 0.0F ? direction / length : glm::vec3(0.0F, 0.0F, -1.0F);
            const glm::vec3 radiance = light.color * light.intensity;
            rows.emplace_back(position, static_cast<float>(light.kind));
            rows.emplace_back(radiance, std::max(light.range, 0.0F));
            rows.emplace_back(direction, light.cosInner);
            rows.emplace_back(light.cosOuter, 0.0F, 0.0F, 0.0F);
            ++count;
        }
    }
    rows[0].x = static_cast<float>(count);
    return rows;
}

}// namespace Kataglyphis
