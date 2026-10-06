module;

#include "GLFW/glfw3.h"
#include "spdlog/spdlog.h"

#include "common/ExtensionSupport.hpp"
#include "common/Utilities.hpp"
#include <cstdint>
#include <span>
#include <vector>
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.instance;

import kataglyphis.vulkan.config;
import kataglyphis.vulkan.debug;

Kataglyphis::VulkanInstance::VulkanInstance()
{
    static vk::detail::DynamicLoader dl;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));

    if (Kataglyphis::validationLayersEnabled() && !check_validation_layer_support()) {
        spdlog::error("Validation layers requested, but not available!");
    }

    vk::ApplicationInfo app_info{};
    app_info.pApplicationName = "\\__/ Epic Graphics from hell \\__/";// custom name of app
    app_info.applicationVersion = VK_MAKE_VERSION(Kataglyphis::RendererConfig::projectVersionMajor,
      Kataglyphis::RendererConfig::projectVersionMinor,
      Kataglyphis::RendererConfig::projectVersionPatch);// custom version of app
    app_info.pEngineName = "Cataglyphis Renderer";// custom engine name
    app_info.engineVersion = VK_MAKE_VERSION(Kataglyphis::RendererConfig::projectVersionMajor,
      Kataglyphis::RendererConfig::projectVersionMinor,
      Kataglyphis::RendererConfig::projectVersionPatch);// custom engine version
    app_info.apiVersion = Kataglyphis::RendererConfig::vulkanApiVersion;// the vulkan version

    // creation info for a vk::Instance
    vk::InstanceCreateInfo create_info{};
    create_info.pApplicationInfo = &app_info;

    // add validation layers IF enabled to the creeate info struct
    if (Kataglyphis::validationLayersEnabled()) {
        create_info.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        create_info.ppEnabledLayerNames = validationLayers.data();

    } else {
        create_info.enabledLayerCount = 0;
        create_info.pNext = nullptr;
    }

    // create list to hold instance extensions
    std::vector<const char *> instance_extensions = std::vector<const char *>();

    // Setup extensions the instance will use
    uint32_t glfw_extensions_count = 0;// GLFW may require multiple extensions
    const char **glfw_extensions = nullptr;// Extensions passed as array of cstrings, so
                                           // need pointer(array) to pointer

    // set GLFW extensions
    glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extensions_count);

    // Add GLFW extensions to list of extensions
    for (size_t i = 0; i < glfw_extensions_count; i++) { instance_extensions.push_back(glfw_extensions[i]); }

    if (Kataglyphis::validationLayersEnabled()) { instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); }

    // check instance extensions supported
    if (!check_instance_extension_support(instance_extensions)) {
        spdlog::critical("VkInstance does not support required extensions!");
        std::abort();
    }

    create_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size());
    create_info.ppEnabledExtensionNames = instance_extensions.data();

    // create instance
    vk::ResultValue<vk::Instance> instance_result = vk::createInstance(create_info);
    ASSERT_VULKAN(instance_result.result, "Failed to create vulkan instance!");
    instance = instance_result.value;

    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);
}

auto Kataglyphis::VulkanInstance::check_validation_layer_support() -> bool
{
    auto available_layers_result = vk::enumerateInstanceLayerProperties();
    if (available_layers_result.result != vk::Result::eSuccess) {
        spdlog::warn("vkEnumerateInstanceLayerProperties failed (result {}); treating as no supported "
                     "validation layers.",
          static_cast<int>(available_layers_result.result));
    }
    std::vector<vk::LayerProperties> availableLayers = available_layers_result.value;

    for (const char *layerName : validationLayers) {
        if (!supportsLayer(availableLayers, layerName)) { return false; }
    }

    return true;
}

auto Kataglyphis::VulkanInstance::check_instance_extension_support(std::span<const char *const> check_extensions)
  -> bool
{
    auto extensions_result = vk::enumerateInstanceExtensionProperties();
    if (extensions_result.result != vk::Result::eSuccess) {
        spdlog::warn("vkEnumerateInstanceExtensionProperties failed (result {}); treating as no supported "
                     "instance extensions.",
          static_cast<int>(extensions_result.result));
    }
    std::vector<vk::ExtensionProperties> extensions = extensions_result.value;

    const char *missing = firstMissingExtension(extensions, check_extensions);
    if (missing != nullptr) {
        spdlog::critical("Required instance extension not supported: {}", missing);
        return false;
    }

    return true;
}

void Kataglyphis::VulkanInstance::cleanUp()
{
    if (!instance) { return; }
    instance.destroy();
    instance = nullptr;
}

// No cleanUp() here: VulkanRenderer::cleanUp() owns the device-versus-instance teardown order.
Kataglyphis::VulkanInstance::~VulkanInstance() = default;
