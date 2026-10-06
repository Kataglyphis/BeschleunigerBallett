module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <glm/glm.hpp>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.vertex;

namespace vertex {

// The loaders validate indices; an out-of-range one here is an unchecked out-of-bounds write.
void computeFlatNormals(std::span<Vertex> vertices, std::span<const unsigned int> indices, std::size_t firstIndex)
{
    for (std::size_t i = firstIndex; i + 2 < indices.size(); i += 3) {
        Vertex &v0 = vertices[indices[i + 0]];
        Vertex &v1 = vertices[indices[i + 1]];
        Vertex &v2 = vertices[indices[i + 2]];

        const glm::vec3 faceNormal = glm::cross(v1.position - v0.position, v2.position - v0.position);
        // Zero-area triangle: keep the old normal instead of normalizing a zero vector to NaN.
        if (glm::dot(faceNormal, faceNormal) <= 0.0F) { continue; }
        const glm::vec3 n = glm::normalize(faceNormal);
        v0.normal = n;
        v1.normal = n;
        v2.normal = n;
    }
}

void fillMissingFlatNormals(std::span<Vertex> vertices, std::span<const unsigned int> indices, std::size_t firstIndex)
{
    for (std::size_t i = firstIndex; i + 2 < indices.size(); i += 3) {
        Vertex &v0 = vertices[indices[i + 0]];
        Vertex &v1 = vertices[indices[i + 1]];
        Vertex &v2 = vertices[indices[i + 2]];

        const glm::vec3 faceNormal = glm::cross(v1.position - v0.position, v2.position - v0.position);
        if (glm::dot(faceNormal, faceNormal) <= 0.0F) { continue; }
        const glm::vec3 n = glm::normalize(faceNormal);
        if (glm::dot(v0.normal, v0.normal) <= 1e-12F) { v0.normal = n; }
        if (glm::dot(v1.normal, v1.normal) <= 1e-12F) { v1.normal = n; }
        if (glm::dot(v2.normal, v2.normal) <= 1e-12F) { v2.normal = n; }
    }
}

auto getVertexInputAttributeDesc() -> std::array<vk::VertexInputAttributeDescription, 5>
{
    std::array<vk::VertexInputAttributeDescription, 5> attribute_describtions{};

    attribute_describtions[0] =
      vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position));

    attribute_describtions[1] =
      vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal));

    attribute_describtions[2] =
      vk::VertexInputAttributeDescription(2, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(Vertex, color));

    attribute_describtions[3] =
      vk::VertexInputAttributeDescription(3, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, texture_coords));

    attribute_describtions[4] =
      vk::VertexInputAttributeDescription(4, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(Vertex, tangent));

    return attribute_describtions;
}

// Lengyel tangents with glTF handedness in .w; not MikkTSpace, so a mirrored UV seam gets one averaged frame.
void computeTangents(std::span<Vertex> vertices, std::span<const unsigned int> indices, std::size_t firstIndex)
{
    if (firstIndex + 2 >= indices.size()) { return; }

    // Size accumulators to the referenced range: called per primitive over the growing array, full size is quadratic.
    unsigned int minCorner = indices[firstIndex];
    unsigned int maxCorner = indices[firstIndex];
    for (std::size_t i = firstIndex; i + 2 < indices.size(); i += 3) {
        for (unsigned int corner : { indices[i + 0], indices[i + 1], indices[i + 2] }) {
            minCorner = std::min(minCorner, corner);
            maxCorner = std::max(maxCorner, corner);
        }
    }
    const std::size_t rangeSize = static_cast<std::size_t>(maxCorner - minCorner) + 1;

    std::vector<glm::vec3> tangentAccum(rangeSize, glm::vec3(0.0F));
    std::vector<glm::vec3> bitangentAccum(rangeSize, glm::vec3(0.0F));
    std::vector<uint8_t> finalized(rangeSize, 0);

    for (std::size_t i = firstIndex; i + 2 < indices.size(); i += 3) {
        const unsigned int corners[3] = { indices[i + 0], indices[i + 1], indices[i + 2] };
        const Vertex &v0 = vertices[corners[0]];
        const Vertex &v1 = vertices[corners[1]];
        const Vertex &v2 = vertices[corners[2]];

        const glm::vec3 e1 = v1.position - v0.position;
        const glm::vec3 e2 = v2.position - v0.position;
        const glm::vec2 d1 = v1.texture_coords - v0.texture_coords;
        const glm::vec2 d2 = v2.texture_coords - v0.texture_coords;

        const float det = (d1.x * d2.y) - (d2.x * d1.y);
        // Zero-area UV triangle: skip it, or its NaN/Inf would survive normalization.
        if (glm::abs(det) < 1e-8F) { continue; }
        const float invDet = 1.0F / det;
        const glm::vec3 tangent = ((e1 * d2.y) - (e2 * d1.y)) * invDet;
        const glm::vec3 bitangent = ((e2 * d1.x) - (e1 * d2.x)) * invDet;

        for (unsigned int corner : corners) {
            tangentAccum[corner - minCorner] += tangent;
            bitangentAccum[corner - minCorner] += bitangent;
        }
    }

    // A vertex's frame depends only on its accumulators, so `finalized` computes it once, not per incident triangle.
    for (std::size_t i = firstIndex; i + 2 < indices.size(); i += 3) {
        for (unsigned int corner : { indices[i + 0], indices[i + 1], indices[i + 2] }) {
            const std::size_t slot = corner - minCorner;
            if (finalized[slot] != 0) { continue; }
            finalized[slot] = 1;

            Vertex &v = vertices[corner];
            const glm::vec3 n = v.normal;
            const glm::vec3 &accumTangent = tangentAccum[slot];
            const glm::vec3 &accumBitangent = bitangentAccum[slot];

            glm::vec3 t = accumTangent - (n * glm::dot(n, accumTangent));
            if (glm::dot(t, t) > 1e-12F) {
                t = glm::normalize(t);
            } else {
                // Nothing survived Gram-Schmidt: any axis orthogonal to the normal beats a NaN.
                glm::vec3 fallback = glm::cross(n, glm::vec3(0.0F, 1.0F, 0.0F));
                if (glm::dot(fallback, fallback) <= 1e-12F) { fallback = glm::cross(n, glm::vec3(1.0F, 0.0F, 0.0F)); }
                t = glm::normalize(fallback);
            }

            const float w = glm::dot(glm::cross(n, t), accumBitangent) < 0.0F ? -1.0F : 1.0F;
            v.tangent = glm::vec4(t, w);
        }
    }
}

}// namespace vertex