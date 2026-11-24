module;

#include <array>
#include <cstddef>
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.vertex;

namespace vertex {

auto getVertexInputAttributeDesc() -> std::array<vk::VertexInputAttributeDescription, VERTEX_ATTRIBUTE_COUNT>
{
    std::array<vk::VertexInputAttributeDescription, VERTEX_ATTRIBUTE_COUNT> attribute_describtions{};

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

    attribute_describtions[5] =
      vk::VertexInputAttributeDescription(5, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, texture_coords1));

    return attribute_describtions;
}

}// namespace vertex
