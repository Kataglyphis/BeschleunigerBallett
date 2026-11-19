module;

#include <cstddef>
#include <span>
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

/// Whether `range` lies within flat arrays of these lengths; both upload paths give one that does not an empty mesh.
bool meshRangeFits(const MeshRange &range, std::size_t vertexCount, std::size_t indexCount, std::size_t triCount)
{
    return range.vertexBase <= vertexCount && range.vertexCount <= vertexCount - range.vertexBase
           && range.indexStart <= indexCount && range.indexCount <= indexCount - range.indexStart
           && range.triStart <= triCount && range.triCount <= triCount - range.triStart;
}

/// Writes each of `indices` minus `firstVertex` to `out`, up to the shorter of the two: both upload paths' re-base.
void rebaseIndices(std::span<const unsigned int> indices, unsigned int firstVertex, std::span<unsigned int> out)
{
    for (std::size_t i = 0; i < indices.size() && i < out.size(); ++i) { out[i] = indices[i] - firstVertex; }
}

/// The OBJ upload's copy of one range (glTF uploads in place); a range that does not fit yields an empty MeshSlice.
MeshSlice sliceMeshRange(const MeshRange &range,
  const std::vector<Vertex> &vertices,
  const std::vector<unsigned int> &indices,
  const std::vector<unsigned int> &materialIndex)
{
    if (!meshRangeFits(range, vertices.size(), indices.size(), materialIndex.size())) { return {}; }

    MeshSlice slice;
    slice.vertices.assign(vertices.begin() + static_cast<std::ptrdiff_t>(range.vertexBase),
      vertices.begin() + static_cast<std::ptrdiff_t>(range.vertexBase + range.vertexCount));

    slice.indices.resize(range.indexCount);
    rebaseIndices(std::span(indices).subspan(range.indexStart, range.indexCount),
      static_cast<unsigned int>(range.vertexBase),
      slice.indices);

    slice.materialIndex.assign(materialIndex.begin() + static_cast<std::ptrdiff_t>(range.triStart),
      materialIndex.begin() + static_cast<std::ptrdiff_t>(range.triStart + range.triCount));

    return slice;
}

}// namespace Kataglyphis
