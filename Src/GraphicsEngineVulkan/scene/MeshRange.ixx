module;

#include <cstddef>
#include <vector>

export module kataglyphis.vulkan.mesh_range;

import kataglyphis.vulkan.vertex;

export namespace Kataglyphis {

/// One sub-mesh's slice of a loader's flat vertex/index/material arrays. Both
/// ObjLoader (one range per shape/group) and GltfLoader (one range per
/// primitive) record these during parse so uploadParsed can build one Mesh per
/// range. A single-shape / single-primitive model yields exactly one range
/// spanning everything, which is behaviour-identical to no split at all.
struct MeshRange
{
    std::size_t vertexBase;
    std::size_t vertexCount;
    std::size_t indexStart;
    std::size_t indexCount;
    std::size_t triStart;
    std::size_t triCount;
    // glTF material.doubleSided for this range's material; uploadParsed forwards
    // it to add_new_mesh so the raster pass can disable back-face culling for
    // this mesh only. false for single-sided / absent material (and always
    // false for OBJ, which has no double-sided concept).
    bool doubleSided = false;
    // glTF alphaMode BLEND for this range's material, forwarded like doubleSided; always false for OBJ.
    bool alphaBlended = false;
};

/// The extracted sub-arrays for one range: the range's vertices, its indices
/// re-based to that vertex subset (each index minus vertexBase), and its
/// per-triangle material ids.
struct MeshSlice
{
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<unsigned int> materialIndex;
};

/// Slices the flat arrays for a single range. Shared by both loaders'
/// uploadParsed so the re-basing arithmetic lives in one place. A range that
/// does not fit within the arrays (a malformed asset, or a slicer bug in the
/// loader) yields an empty MeshSlice instead of reading past the arrays.
MeshSlice sliceMeshRange(const MeshRange &range,
  const std::vector<Vertex> &vertices,
  const std::vector<unsigned int> &indices,
  const std::vector<unsigned int> &materialIndex)
{
    if (range.vertexBase > vertices.size() || range.vertexCount > vertices.size() - range.vertexBase
        || range.indexStart > indices.size() || range.indexCount > indices.size() - range.indexStart
        || range.triStart > materialIndex.size() || range.triCount > materialIndex.size() - range.triStart) {
        return {};
    }

    MeshSlice slice;
    slice.vertices.assign(vertices.begin() + static_cast<std::ptrdiff_t>(range.vertexBase),
      vertices.begin() + static_cast<std::ptrdiff_t>(range.vertexBase + range.vertexCount));

    slice.indices.reserve(range.indexCount);
    for (std::size_t i = 0; i < range.indexCount; ++i) {
        slice.indices.push_back(indices[range.indexStart + i] - static_cast<unsigned int>(range.vertexBase));
    }

    slice.materialIndex.assign(materialIndex.begin() + static_cast<std::ptrdiff_t>(range.triStart),
      materialIndex.begin() + static_cast<std::ptrdiff_t>(range.triStart + range.triCount));

    return slice;
}

}// namespace Kataglyphis
