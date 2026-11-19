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

/// Per-triangle geometric (flat) normal for every corner of the triangles in
/// indices[firstIndex..] whose normal is (near-)zero-length; a corner with a
/// normal keeps it, for OBJs where only some faces carry `vn`. Bounded by
/// `i + 2 < indices.size()` and skips zero-area (degenerate) triangles,
/// which would otherwise normalize a zero vector into NaN.
void fillMissingFlatNormals(std::span<Vertex> vertices,
  std::span<const unsigned int> indices,
  std::size_t firstIndex = 0);

/// Per-vertex tangent frame (xyz = tangent, w = handedness) from triangle UV
/// gradients (Lengyel's method), for every triangle in indices[firstIndex..].
/// Requires each touched vertex's `normal` to already be final - run this
/// after fillMissingFlatNormals, never before. Degenerate
/// UVs fall back to an arbitrary axis orthogonal to the normal instead of
/// producing a NaN. `uvSet` 1 takes the gradients from texture_coords1: glTF derives the frame from the UV set
/// the normal texture samples.
void computeTangents(std::span<Vertex> vertices,
  std::span<const unsigned int> indices,
  std::size_t firstIndex = 0,
  unsigned int uvSet = 0);
}// namespace vertex
