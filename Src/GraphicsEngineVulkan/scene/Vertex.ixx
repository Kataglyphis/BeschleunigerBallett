module;

#include "shared/scene/Vertex.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <vulkan/vulkan.hpp>

#include <array>
#include <cstddef>
#include <span>

export module kataglyphis.vulkan.vertex;

export using ::Vertex;

export namespace vertex {
// Location 5 is TEXCOORD_1, which only the scene shaders read.
inline constexpr std::size_t VERTEX_ATTRIBUTE_COUNT = 6;
std::array<vk::VertexInputAttributeDescription, VERTEX_ATTRIBUTE_COUNT> getVertexInputAttributeDesc();
}// namespace vertex
