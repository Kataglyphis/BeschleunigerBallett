module;

#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.instance;

export namespace Kataglyphis {
class VulkanInstance
{
  public:
    VulkanInstance();
    // false is what a Profile or Release build creates; the loader's debug_utils still comes on.
    explicit VulkanInstance(bool validation);
    // Shared by the device, so the instance and its debug messenger outlive every child object.
    VulkanInstance(const VulkanInstance &) = delete;
    VulkanInstance &operator=(const VulkanInstance &) = delete;
    VulkanInstance(VulkanInstance &&) = delete;
    VulkanInstance &operator=(VulkanInstance &&) = delete;

    vk::Instance &getVulkanInstance() { return instance; };
    // On whenever the loader lists debug_utils, so the labels and object names reach Profile and Release captures.
    bool debugUtilsEnabled() const { return debug_utils_enabled; };

    ~VulkanInstance();

  private:
    vk::Instance instance{};
    bool validation_enabled{ false };
    bool debug_utils_enabled{ false };

    std::vector<const char *> validationLayers = { "VK_LAYER_KHRONOS_validation" };

    bool check_validation_layer_support();
    static bool check_instance_extension_support(std::span<const char *const> check_extensions);
};
}// namespace Kataglyphis
