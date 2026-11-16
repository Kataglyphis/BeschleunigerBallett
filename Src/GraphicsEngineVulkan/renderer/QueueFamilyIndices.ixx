module;
#include <algorithm>
#include <cstdint>
#include <vector>

export module kataglyphis.vulkan.queue_family_indices;

export namespace Kataglyphis::VulkanRendererInternals {
struct QueueFamilyIndices
{
    int graphics_family = -1;
    int presentation_family = -1;
    int compute_family = -1;

    bool is_valid() const { return graphics_family >= 0 && presentation_family >= 0 && compute_family >= 0; }

    // compute_family only qualifies the device: compute work runs on the graphics queue (Clouds.cpp).
    [[nodiscard]] std::vector<uint32_t> queueFamiliesToCreate() const
    {
        std::vector<uint32_t> families = { static_cast<uint32_t>(graphics_family),
            static_cast<uint32_t>(presentation_family) };
        std::sort(families.begin(), families.end());
        families.erase(std::unique(families.begin(), families.end()), families.end());
        return families;
    }
};
}// namespace Kataglyphis::VulkanRendererInternals
