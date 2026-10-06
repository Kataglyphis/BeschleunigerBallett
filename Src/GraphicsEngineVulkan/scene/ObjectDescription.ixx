module;

#include "ObjectDescription.hpp"
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

export module kataglyphis.vulkan.object_description;

export using ::ObjectDescription;

export namespace Kataglyphis {

/// Stamps texture_offset onto `descriptions`, which holds one entry per MESH
/// flattened across models (see Scene::add_model). Every mesh of a model
/// shares that model's offset into the flattened global texture array; the
/// offset advances by that model's texture count once its meshes are done.
/// Stops cleanly (leaving any trailing descriptions untouched) if the
/// per-model mesh counts would run past the end of `descriptions` - a
/// mesh-count/description-count mismatch must not read out of range.
inline void assignTextureOffsets(std::span<ObjectDescription> descriptions,
  std::span<const uint32_t> meshCountPerModel,
  std::span<const uint32_t> textureCountPerModel)
{
    const std::size_t modelCount = std::min(meshCountPerModel.size(), textureCountPerModel.size());
    std::size_t descIndex = 0;
    uint64_t offset = 0;
    for (std::size_t m = 0; m < modelCount; ++m) {
        for (uint32_t k = 0; k < meshCountPerModel[m]; ++k) {
            if (descIndex >= descriptions.size()) { return; }
            descriptions[descIndex].texture_offset = offset;
            ++descIndex;
        }
        offset += textureCountPerModel[m];
    }
}

/// One entry per model: the running sum of preceding models' mesh counts.
/// This is the value written to
/// `VkAccelerationStructureInstanceKHR::instanceCustomIndex` for each model's
/// TLAS instance, and read back by the traced shaders (via `InstanceID()`)
/// as the object-index base they add `GeometryIndex()` to. It walks the same
/// `meshCountPerModel` counts as assignTextureOffsets, just summing mesh
/// counts instead of texture counts - the two must agree on model order.
inline std::vector<uint32_t> meshBaseOffsets(std::span<const uint32_t> meshCountPerModel)
{
    std::vector<uint32_t> offsets;
    offsets.reserve(meshCountPerModel.size());
    uint32_t offset = 0;
    for (const uint32_t meshCount : meshCountPerModel) {
        offsets.push_back(offset);
        offset += meshCount;
    }
    return offsets;
}

/// One slot in the flattened global texture array: which model the texture
/// came from, and its index within that model's local texture list.
struct FlattenedTextureSlot
{
    uint32_t model;
    uint32_t indexInModel;

    bool operator==(const FlattenedTextureSlot &) const = default;
};

/// Result of flattening every model's textures into the global array, in
/// model order - the same order assignTextureOffsets advances `offset` in.
/// `slots` is either empty (no textures at all) or padded to exactly
/// `maxSlots` entries by repeating `slots.front()` (every fixed-size
/// descriptor binding slot must hold a valid descriptor). `requestedCount`
/// is the sum of textures across every model, even past the cap, so the
/// caller can report the true overflow instead of just the collected count.
struct FlattenedTexturePlan
{
    std::vector<FlattenedTextureSlot> slots;
    uint32_t requestedCount;
    bool exhausted;
};

/// Mirror image of assignTextureOffsets: that function stamps each mesh's
/// offset into the flattened array, this one builds the array itself, model
/// by model, capping at `maxSlots` and padding the tail with slot 0.
inline FlattenedTexturePlan planFlattenedTextureSlots(std::span<const uint32_t> textureCountPerModel, uint32_t maxSlots)
{
    FlattenedTexturePlan plan{ .slots = {}, .requestedCount = 0, .exhausted = false };

    for (uint32_t model = 0; model < textureCountPerModel.size(); ++model) {
        plan.requestedCount += textureCountPerModel[model];
    }

    for (uint32_t model = 0; model < textureCountPerModel.size(); ++model) {
        const uint32_t modelTextureCount = textureCountPerModel[model];
        for (uint32_t t = 0; t < modelTextureCount; ++t) {
            if (plan.slots.size() >= maxSlots) {
                plan.exhausted = true;
                break;
            }
            plan.slots.push_back(FlattenedTextureSlot{ .model = model, .indexInModel = t });
        }
        if (plan.exhausted) { break; }
    }

    if (plan.slots.empty()) { return plan; }

    const FlattenedTextureSlot firstSlot = plan.slots.front();
    while (plan.slots.size() < maxSlots) { plan.slots.push_back(firstSlot); }

    return plan;
}

}// namespace Kataglyphis
