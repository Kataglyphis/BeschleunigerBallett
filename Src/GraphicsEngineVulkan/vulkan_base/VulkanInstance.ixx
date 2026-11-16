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
    // Shared by the device, so the instance and its debug messenger outlive every child object.
    VulkanInstance(const VulkanInstance &) = delete;
    VulkanInstance &operator=(const VulkanInstance &) = delete;
    VulkanInstance(VulkanInstance &&) = delete;
    VulkanInstance &operator=(VulkanInstance &&) = delete;

    vk::Instance &getVulkanInstance() { return instance; };

    ~VulkanInstance();

  private:
    vk::Instance instance{};

    std::vector<const char *> validationLayers = { "VK_LAYER_KHRONOS_validation" };

    bool check_validation_layer_support();
    static bool check_instance_extension_support(std::span<const char *const> check_extensions);
};
}// namespace Kataglyphis
