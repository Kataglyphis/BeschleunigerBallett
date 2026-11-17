module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
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

/// Point and spot lights that get a cube shadow map; the rest, and every directional light, stay unshadowed.
inline constexpr std::size_t MAX_SHADOWED_PUNCTUAL_LIGHTS = 4;

/// One shadowed light's cube: where it sits and the depth range its six faces project.
struct PunctualShadowSlot
{
    glm::vec3 position{ 0.0F };
    float nearPlane = 0.01F;
    float farPlane = 100.0F;
};

/// The far plane of a light without a range: past the farthest corner of the scene, or 100 without geometry.
inline float unboundedShadowFar(const glm::vec3 &position, const glm::vec3 &boundsMin, const glm::vec3 &boundsMax)
{
    if (boundsMin.x > boundsMax.x || boundsMin.y > boundsMax.y || boundsMin.z > boundsMax.z) { return 100.0F; }
    const glm::vec3 farthest = glm::max(glm::abs(boundsMin - position), glm::abs(boundsMax - position));
    return std::max(glm::length(farthest) * 1.05F, 1.0F);
}

/// Gives the first MAX_SHADOWED_PUNCTUAL_LIGHTS point and spot lights of packed `rows` a cube: their row 3
/// becomes [cos_outer, slot + 1, near, far], 0 in .y meaning unshadowed (Rust's zero pad).
std::vector<PunctualShadowSlot>
  assignPunctualShadowSlots(std::span<glm::vec4> rows, const glm::vec3 &boundsMin, const glm::vec3 &boundsMax)
{
    std::vector<PunctualShadowSlot> slots;
    if (rows.empty()) { return slots; }
    const auto count = static_cast<std::size_t>(rows[0].x);
    for (std::size_t i = 0; i < count && 1 + (i + 1) * PUNCTUAL_LIGHT_ROWS <= rows.size(); ++i) {
        const std::size_t row = 1 + i * PUNCTUAL_LIGHT_ROWS;
        const auto kind = static_cast<PunctualLightKind>(static_cast<unsigned int>(rows[row].w));
        if (kind == PunctualLightKind::Directional || slots.size() == MAX_SHADOWED_PUNCTUAL_LIGHTS) { continue; }
        PunctualShadowSlot slot;
        slot.position = glm::vec3(rows[row]);
        const float range = rows[row + 1].w;
        slot.farPlane = range > 0.0F ? range : unboundedShadowFar(slot.position, boundsMin, boundsMax);
        // A thousandth of the far plane keeps D32's precision at the far end finer than a texel.
        slot.nearPlane = std::clamp(slot.farPlane * 1.0e-3F, 0.01F, 0.5F);
        rows[row + 3] = glm::vec4(rows[row + 3].x, static_cast<float>(slots.size() + 1), slot.nearPlane, slot.farPlane);
        slots.push_back(slot);
    }
    return slots;
}

/// View-projections of a cube's faces in Vulkan's layer order (+X, -X, +Y, -Y, +Z, -Z), each a 90-degree
/// perspective whose NDC xy is the face's (sc, tc) / |ma| of the cube-map selection table, depth zero to one.
std::array<glm::mat4, 6> punctualShadowFaceMatrices(const PunctualShadowSlot &slot)
{
    static constexpr std::array<std::array<glm::vec3, 2>, 6> kFaces{ {
      { glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, -1.0F, 0.0F) },
      { glm::vec3(-1.0F, 0.0F, 0.0F), glm::vec3(0.0F, -1.0F, 0.0F) },
      { glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F) },
      { glm::vec3(0.0F, -1.0F, 0.0F), glm::vec3(0.0F, 0.0F, -1.0F) },
      { glm::vec3(0.0F, 0.0F, 1.0F), glm::vec3(0.0F, -1.0F, 0.0F) },
      { glm::vec3(0.0F, 0.0F, -1.0F), glm::vec3(0.0F, -1.0F, 0.0F) },
    } };
    const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(90.0F), 1.0F, slot.nearPlane, slot.farPlane);
    std::array<glm::mat4, 6> faces{};
    for (std::size_t face = 0; face < faces.size(); ++face) {
        faces[face] = projection * glm::lookAtRH(slot.position, slot.position + kFaces[face][0], kFaces[face][1]);
    }
    return faces;
}

/// The depth a face stores for a point `majorAxis` along its axis; punctual_lights.slang's punctual_shadow_depth.
constexpr float punctualShadowDepth(float majorAxis, float nearPlane, float farPlane)
{ return farPlane * (majorAxis - nearPlane) / ((farPlane - nearPlane) * majorAxis); }

}// namespace Kataglyphis
