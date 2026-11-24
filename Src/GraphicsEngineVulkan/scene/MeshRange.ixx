module;

#include <cstddef>
#include <span>

export module kataglyphis.vulkan.mesh_range;

export namespace Kataglyphis {

/// One sub-mesh's slice of GltfLoader's flat vertex/index/material arrays: one
/// per glTF primitive or OBJ material, so uploadParsed builds one Mesh per range.
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
    // this mesh only. false for single-sided / absent material and for OBJ.
    bool doubleSided = false;
    // alphaMode BLEND (OBJ: d below 1 or Tr above 0) for this range's material, forwarded like doubleSided.
    bool alphaBlended = false;
};

/// Whether `range` lies within flat arrays of these lengths; the upload gives one that does not an empty mesh.
bool meshRangeFits(const MeshRange &range, std::size_t vertexCount, std::size_t indexCount, std::size_t triCount)
{
    return range.vertexBase <= vertexCount && range.vertexCount <= vertexCount - range.vertexBase
           && range.indexStart <= indexCount && range.indexCount <= indexCount - range.indexStart
           && range.triStart <= triCount && range.triCount <= triCount - range.triStart;
}

/// Writes each of `indices` minus `firstVertex` to `out`, up to the shorter of the two: the upload's re-base.
void rebaseIndices(std::span<const unsigned int> indices, unsigned int firstVertex, std::span<unsigned int> out)
{
    for (std::size_t i = 0; i < indices.size() && i < out.size(); ++i) { out[i] = indices[i] - firstVertex; }
}

}// namespace Kataglyphis
