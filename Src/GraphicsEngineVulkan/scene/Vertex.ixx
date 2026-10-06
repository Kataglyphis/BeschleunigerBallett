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
std::array<vk::VertexInputAttributeDescription, 5> getVertexInputAttributeDesc();

/// Per-triangle geometric (flat) normal, assigned to all three vertices of
/// every triangle in indices[firstIndex..]. The OBJ and glTF loaders each
/// hand-rolled this loop; this is the only copy left, so a third loader
/// adding its own is the bug this function exists to prevent. Bounded by
/// `i + 2 < indices.size()` and skips zero-area (degenerate) triangles,
/// which would otherwise normalize a zero vector into NaN.
void computeFlatNormals(std::span<Vertex> vertices, std::span<const unsigned int> indices, std::size_t firstIndex = 0);

/// Same per-triangle geometric normal as computeFlatNormals, but only fills
/// in a corner whose current normal is (near-)zero-length instead of
/// overwriting every corner - for OBJs where only some faces carry `vn`.
void fillMissingFlatNormals(std::span<Vertex> vertices,
  std::span<const unsigned int> indices,
  std::size_t firstIndex = 0);

/// Per-vertex tangent frame (xyz = tangent, w = handedness) from triangle UV
/// gradients (Lengyel's method), for every triangle in indices[firstIndex..].
/// Requires each touched vertex's `normal` to already be final - run this
/// after computeFlatNormals/fillMissingFlatNormals, never before. Degenerate
/// UVs fall back to an arbitrary axis orthogonal to the normal instead of
/// producing a NaN.
void computeTangents(std::span<Vertex> vertices, std::span<const unsigned int> indices, std::size_t firstIndex = 0);
}// namespace vertex
