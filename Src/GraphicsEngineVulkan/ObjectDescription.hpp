#ifndef KATAGLYPHIS_VULKAN_OBJECT_DESCRIPTION_HPP
#define KATAGLYPHIS_VULKAN_OBJECT_DESCRIPTION_HPP

#include <cstdint>

struct ObjectDescription
{
    uint64_t vertex_address;
    uint64_t index_address;
    uint64_t material_index_address;
    uint64_t material_address;
    // First global texture slot of this model (textureIDs are model-local); layout pinned to scene_types.slang.
    uint64_t texture_offset;
};

#endif
