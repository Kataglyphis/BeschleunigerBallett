module;

#include "renderer/SwapChainDetails.hpp"
#include <cstdint>

#include "common/ExtensionSupport.hpp"
#include "common/Utilities.hpp"
#include "spdlog/spdlog.h"
#include "vulkan_base/PhysicalDeviceChoices.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.device;

namespace {

auto readGpuSelectionFromEnvironment() -> std::string
{
#if defined(_WIN32)
    std::size_t required_size = 0;
    if (getenv_s(&required_size, nullptr, 0, "KATAGLYPHIS_VK_GPU") != 0 || required_size == 0) { return ""; }

    std::string value(required_size, '\0');
    if (getenv_s(&required_size, value.data(), value.size(), "KATAGLYPHIS_VK_GPU") != 0 || required_size == 0) {
        return "";
    }

    value.resize(required_size - 1);
    return value;
#else
    const char *value = std::getenv("KATAGLYPHIS_VK_GPU");
    if (value == nullptr) { return ""; }
    return std::string(value);
#endif
}

// Relative to the working directory, like Resources/ and logs/.
auto pipelineCacheFilePath() -> std::filesystem::path
{ return std::filesystem::path("pipeline_cache") / "kataglyphis_pipeline.cache"; }

auto deviceTypeToString(vk::PhysicalDeviceType type) -> const char *
{
    switch (type) {
    case vk::PhysicalDeviceType::eDiscreteGpu:
        return "Discrete GPU";
    case vk::PhysicalDeviceType::eIntegratedGpu:
        return "Integrated GPU";
    case vk::PhysicalDeviceType::eVirtualGpu:
        return "Virtual GPU";
    case vk::PhysicalDeviceType::eCpu:
        return "CPU";
    default:
        return "Other";
    }
}
}// namespace

const std::vector<const char *> device_extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

const std::vector<const char *> device_extensions_for_raytracing = { VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    VK_KHR_SPIRV_1_4_EXTENSION_NAME,
    VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
    VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME };

Kataglyphis::VulkanDevice::VulkanDevice(VulkanInstance *instance, vk::SurfaceKHR *surface)
  : instance(instance), surface(surface)
{


    get_physical_device();
    create_logical_device();
    create_pipeline_cache();
}

auto Kataglyphis::VulkanDevice::getSwapchainDetails() -> Kataglyphis::VulkanRendererInternals::SwapChainDetails
{ return getSwapchainDetails(physical_device); }

void Kataglyphis::VulkanDevice::cleanUp()
{
    if (!logical_device) { return; }
    // The pipeline cache must be saved while the device that created it still exists.
    save_and_destroy_pipeline_cache();
    // The allocator outlives every allocation but must go before the logical device.
    allocator.cleanUp();
    logical_device.destroy();
    logical_device = nullptr;
}

void Kataglyphis::VulkanDevice::create_pipeline_cache()
{
    std::vector<char> initial_data;
    const std::filesystem::path cache_file = pipelineCacheFilePath();

    std::error_code file_error;
    if (std::filesystem::exists(cache_file, file_error) && !file_error) {
        std::ifstream cache_stream(cache_file, std::ios::binary | std::ios::ate);
        if (cache_stream) {
            const std::streamsize cache_size = cache_stream.tellg();
            if (cache_size > 0) {
                initial_data.resize(static_cast<std::size_t>(cache_size));
                cache_stream.seekg(0, std::ios::beg);
                if (!cache_stream.read(initial_data.data(), cache_size)) {
                    spdlog::warn("Failed to read pipeline cache file '{}'; starting with an empty pipeline cache.",
                      cache_file.string());
                    initial_data.clear();
                }
            }
        } else {
            spdlog::warn(
              "Could not open pipeline cache file '{}'; starting with an empty pipeline cache.", cache_file.string());
        }
    }

    vk::PipelineCacheCreateInfo pipeline_cache_create_info{};
    pipeline_cache_create_info.initialDataSize = initial_data.size();
    pipeline_cache_create_info.pInitialData = initial_data.empty() ? nullptr : initial_data.data();

    auto cache_result = logical_device.createPipelineCache(pipeline_cache_create_info);
    if (cache_result.result != vk::Result::eSuccess && !initial_data.empty()) {
        // Stale on-disk data (say, after a driver update) must never be fatal: retry once empty.
        spdlog::warn("Creating the pipeline cache from '{}' failed (result {}); retrying with an empty cache.",
          cache_file.string(),
          static_cast<int>(cache_result.result));
        pipeline_cache_create_info.initialDataSize = 0;
        pipeline_cache_create_info.pInitialData = nullptr;
        cache_result = logical_device.createPipelineCache(pipeline_cache_create_info);
    }

    if (cache_result.result != vk::Result::eSuccess) {
        spdlog::warn("Failed to create a Vulkan pipeline cache (result {}); continuing without one.",
          static_cast<int>(cache_result.result));
        pipeline_cache = nullptr;
        return;
    }

    pipeline_cache = cache_result.value;
    if (initial_data.empty()) {
        spdlog::info("Created an empty Vulkan pipeline cache (no cache file at '{}').", cache_file.string());
    } else {
        spdlog::info(
          "Created Vulkan pipeline cache seeded with {} bytes from '{}'.", initial_data.size(), cache_file.string());
    }
}

void Kataglyphis::VulkanDevice::save_and_destroy_pipeline_cache()
{
    if (!pipeline_cache) { return; }

    auto cache_data = logical_device.getPipelineCacheData(pipeline_cache);
    const bool cache_data_ok = cache_data.result == vk::Result::eSuccess;
    if (cache_data_ok && !cache_data.value.empty()) {
        const std::filesystem::path cache_file = pipelineCacheFilePath();
        std::error_code dir_error;
        std::filesystem::create_directories(cache_file.parent_path(), dir_error);
        if (dir_error) {
            spdlog::warn("Could not create pipeline cache directory '{}': {}. Pipeline cache not persisted.",
              cache_file.parent_path().string(),
              dir_error.message());
        } else {
            std::ofstream cache_stream(cache_file, std::ios::binary | std::ios::trunc);
            if (cache_stream
                && cache_stream.write(reinterpret_cast<const char *>(cache_data.value.data()),
                  static_cast<std::streamsize>(cache_data.value.size()))) {
                spdlog::info(
                  "Persisted Vulkan pipeline cache ({} bytes) to '{}'.", cache_data.value.size(), cache_file.string());
            } else {
                spdlog::warn(
                  "Failed to write pipeline cache file '{}'. Pipeline cache not persisted.", cache_file.string());
            }
        }
    } else if (cache_data.result != vk::Result::eSuccess) {
        spdlog::warn("vkGetPipelineCacheData failed (result {}). Pipeline cache not persisted.",
          static_cast<int>(cache_data.result));
    }

    logical_device.destroyPipelineCache(pipeline_cache);
    pipeline_cache = nullptr;
}

// No cleanUp() here: VulkanRenderer::cleanUp() owns the device-versus-instance teardown order.
Kataglyphis::VulkanDevice::~VulkanDevice() = default;

auto Kataglyphis::VulkanDevice::getQueueFamilies() -> Kataglyphis::VulkanRendererInternals::QueueFamilyIndices
{ return getQueueFamilies(physical_device); }

void Kataglyphis::VulkanDevice::get_physical_device()
{
    const GpuSelectionMode selection_mode = parseGpuSelectionMode(readGpuSelectionFromEnvironment());

    // Enumerate physical devices the vkInstance can access
    auto physical_devices_result = instance->getVulkanInstance().enumeratePhysicalDevices();
    ASSERT_VULKAN(physical_devices_result.result, "Failed to enumerate Vulkan physical devices!");
    std::vector<vk::PhysicalDevice> device_list = physical_devices_result.value;

    // if no devices available, then none support of Vulkan
    if (device_list.empty()) { spdlog::error("Can not find GPU's that support Vulkan Instance!"); }

    std::optional<PhysicalDeviceScore> best_device_score;
    std::optional<PhysicalDeviceScore> best_device_score_fallback;
    vk::PhysicalDevice fallback_device{};
    vk::PhysicalDeviceProperties fallback_properties{};

    for (const auto &device : device_list) {
        if (!check_device_suitable(device)) { continue; }

        vk::PhysicalDeviceProperties candidate_properties = device.getProperties();

        const PhysicalDeviceScore candidate_score = scorePhysicalDevice(candidate_properties);
        if (!best_device_score_fallback || candidate_score > *best_device_score_fallback) {
            best_device_score_fallback = candidate_score;
            fallback_device = device;
            fallback_properties = candidate_properties;
        }

        if (!matchesSelectionMode(candidate_properties, selection_mode)) { continue; }

        if (!best_device_score || candidate_score > *best_device_score) {
            best_device_score = candidate_score;
            physical_device = device;
            device_properties = candidate_properties;
        }
    }

    if (!physical_device && fallback_device) {
        physical_device = fallback_device;
        device_properties = fallback_properties;
        spdlog::default_logger_raw()->log(spdlog::level::warn,
          std::string("No suitable Vulkan GPU matching selection mode '") + gpuSelectionModeToString(selection_mode)
            + "' found. Falling back to auto device selection.");
    }

    if (!physical_device) {
        spdlog::critical("Failed to find a suitable Vulkan physical device.");
        std::abort();
    }

    spdlog::default_logger_raw()->log(spdlog::level::info,
      std::string("Selected Vulkan physical device: ") + device_properties.deviceName.data() + " ("
        + deviceTypeToString(device_properties.deviceType) + ")");
    spdlog::default_logger_raw()->log(
      spdlog::level::info, std::string("Vulkan GPU selection mode: ") + gpuSelectionModeToString(selection_mode));

    vk::PhysicalDeviceVulkan11Properties vulkan11_properties{};
    vk::PhysicalDeviceProperties2 properties2{};
    properties2.pNext = &vulkan11_properties;
    physical_device.getProperties2(&properties2);
    maxMultiviewViewCount = vulkan11_properties.maxMultiviewViewCount;
    spdlog::default_logger_raw()->log(
      spdlog::level::info, std::string("Device maxMultiviewViewCount: ") + std::to_string(maxMultiviewViewCount));
}

void Kataglyphis::VulkanDevice::create_logical_device()
{
    // get the queue family indices for the chosen physical device
    Kataglyphis::VulkanRendererInternals::QueueFamilyIndices const indices = getQueueFamilies();

    // Zero timestamp bits means no vkCmdWriteTimestamp on this queue; GPU timing keys off it.
    {
        std::vector<vk::QueueFamilyProperties> const queue_family_props = physical_device.getQueueFamilyProperties();
        if (indices.graphics_family >= 0 && static_cast<size_t>(indices.graphics_family) < queue_family_props.size()) {
            graphics_queue_timestamp_valid_bits =
              queue_family_props[static_cast<size_t>(indices.graphics_family)].timestampValidBits;
            graphics_family_supports_compute = static_cast<bool>(
              queue_family_props[static_cast<size_t>(indices.graphics_family)].queueFlags & vk::QueueFlagBits::eCompute);
        }
    }

    // vector for queue creation information and set for family indices
    std::vector<vk::DeviceQueueCreateInfo> queue_create_infos;
    std::set<int> const queue_family_indices = {
        indices.graphics_family, indices.presentation_family, indices.compute_family
    };
    std::vector<float> queue_priorities(queue_family_indices.size(), 1.0F);
    queue_create_infos.reserve(queue_family_indices.size());

    std::size_t priority_index = 0;
    for (int const queue_family_index : queue_family_indices) {
        vk::DeviceQueueCreateInfo queue_create_info{};
        queue_create_info.queueFamilyIndex =
          static_cast<uint32_t>(queue_family_index);// the index of the family to create a queue from
        queue_create_info.queueCount = 1;// number of queues to create
        queue_create_info.pQueuePriorities =
          &queue_priorities[priority_index];// Vulkan needs to know how to handle multiple queues, so
                                            // decide priority (1 = highest)

        queue_create_infos.push_back(queue_create_info);
        ++priority_index;
    }

    vk::PhysicalDeviceVulkan13Features available_features13{};
    available_features13.pNext = nullptr;

    vk::PhysicalDeviceVulkan12Features available_features12{};
    available_features12.pNext = &available_features13;

    vk::PhysicalDeviceVulkan11Features available_features11{};
    available_features11.pNext = &available_features12;

    vk::PhysicalDeviceFeatures2 available_features2{};
    available_features2.pNext = &available_features11;
    physical_device.getFeatures2(&available_features2);

    // --ENABLE RAY TRACING PIPELINE
    vk::PhysicalDeviceRayTracingPipelineFeaturesKHR ray_tracing_pipeline_features{};
    ray_tracing_pipeline_features.pNext = nullptr;

    // -- ENABLE ACCELERATION STRUCTURES
    vk::PhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure_features{};
    acceleration_structure_features.pNext = &ray_tracing_pipeline_features;

    vk::PhysicalDeviceVulkan13Features features13{};
    features13.shaderDemoteToHelperInvocation = available_features13.shaderDemoteToHelperInvocation;
    // Chains the ray-tracing structs only in the hardware-RT block: they must not ride along for unenabled extensions.
    features13.pNext = nullptr;

    vk::PhysicalDeviceRayQueryFeaturesKHR rayQueryFeature{};
    rayQueryFeature.pNext = &features13;

    vk::PhysicalDeviceRayQueryFeaturesKHR availableRayQueryFeature{};

    vk::PhysicalDeviceRayTracingPipelineFeaturesKHR availableRayTracingPipelineFeatures{};
    availableRayTracingPipelineFeatures.pNext = &availableRayQueryFeature;

    vk::PhysicalDeviceAccelerationStructureFeaturesKHR availableAccelerationStructureFeatures{};
    availableAccelerationStructureFeatures.pNext = &availableRayTracingPipelineFeatures;

    vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR availableComputeDerivativeFeatures{};
    availableComputeDerivativeFeatures.pNext = &availableAccelerationStructureFeatures;

    vk::PhysicalDeviceFeatures2 availableRayTracingFeatures2{};
    availableRayTracingFeatures2.pNext = &availableComputeDerivativeFeatures;
    physical_device.getFeatures2(&availableRayTracingFeatures2);

    vk::PhysicalDeviceVulkan12Features features12{};
    features12.pNext = &features13;
    features12.bufferDeviceAddress = available_features12.bufferDeviceAddress;
    features12.scalarBlockLayout = available_features12.scalarBlockLayout;
    features12.descriptorIndexing = available_features12.descriptorIndexing;
    features12.runtimeDescriptorArray = available_features12.runtimeDescriptorArray;
    features12.shaderSampledImageArrayNonUniformIndexing =
      available_features12.shaderSampledImageArrayNonUniformIndexing;

    vk::PhysicalDeviceVulkan11Features features11{};
    features11.pNext = &features12;
    features11.multiview = available_features11.multiview;
    // Slang emits the DrawParameters capability for SV_PrimitiveID in fragment shaders.
    features11.shaderDrawParameters = available_features11.shaderDrawParameters;

    vk::PhysicalDeviceFeatures2 features2{};
    // This link carries every features11/12/13 and ray-tracing bit to vkCreateDevice; without it they vanish silently.
    features2.pNext = &features11;
    features2.features.samplerAnisotropy = available_features2.features.samplerAnisotropy;
    features2.features.shaderInt64 = available_features2.features.shaderInt64;
    features2.features.geometryShader = available_features2.features.geometryShader;
    features2.features.fragmentStoresAndAtomics = available_features2.features.fragmentStoresAndAtomics;
    features2.features.logicOp = available_features2.features.logicOp;
    // The shadow pass pancakes casters with depth clamp; a device without it clips them instead.
    features2.features.depthClamp = available_features2.features.depthClamp;
    features2.features.robustBufferAccess = VK_FALSE;

    std::vector<const char *> extensions(device_extensions);

    // Query available extensions for the physical device
    auto available_extensions_result = physical_device.enumerateDeviceExtensionProperties();
    if (available_extensions_result.result != vk::Result::eSuccess) {
        spdlog::warn("vkEnumerateDeviceExtensionProperties failed for device '{}' (result {}); treating as no "
                     "supported extensions.",
          device_properties.deviceName.data(),
          static_cast<int>(available_extensions_result.result));
    }
    std::vector<vk::ExtensionProperties> availableExtensions = available_extensions_result.value;

    const bool hasBufferDeviceAddressFeature = available_features12.bufferDeviceAddress == VK_TRUE;
    deviceSupportsBufferDeviceAddress = hasBufferDeviceAddressFeature;
    deviceSupportsDepthClamp = available_features2.features.depthClamp == VK_TRUE;
    deviceSupportsSamplerAnisotropy = available_features2.features.samplerAnisotropy == VK_TRUE;
    deviceMaxSamplerAnisotropy = device_properties.limits.maxSamplerAnisotropy;
    const bool hasRequiredDescriptorIndexingFeatures =
      available_features12.descriptorIndexing == VK_TRUE && available_features12.runtimeDescriptorArray == VK_TRUE
      && available_features12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;

    spdlog::default_logger_raw()->log(spdlog::level::info,
      std::string("Feature support: bufferDeviceAddress=") + (hasBufferDeviceAddressFeature ? "true" : "false")
        + ", descriptorIndexing=" + (available_features12.descriptorIndexing == VK_TRUE ? "true" : "false")
        + ", runtimeDescriptorArray=" + (available_features12.runtimeDescriptorArray == VK_TRUE ? "true" : "false")
        + ", sampledImageArrayNonUniformIndexing="
        + (available_features12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE ? "true" : "false")
        + ", robustBufferAccess=" + (available_features2.features.robustBufferAccess == VK_TRUE ? "true" : "false"));

    for (const char *extensionName : device_extensions_for_raytracing) {
        if (!supportsExtension(availableExtensions, extensionName)) {
            deviceSupportsHardwareAcceleratedRRT = false;
            spdlog::default_logger_raw()->log(
              spdlog::level::info, std::string("Required extension not supported: ") + extensionName);
        }
    }

    if (deviceSupportsHardwareAcceleratedRRT && !hasRequiredDescriptorIndexingFeatures) {
        deviceSupportsHardwareAcceleratedRRT = false;
        spdlog::info(
          "Required Vulkan 1.2 descriptor indexing features are not supported; disabling hardware ray tracing path.");
    }

    if (deviceSupportsHardwareAcceleratedRRT && !hasBufferDeviceAddressFeature) {
        deviceSupportsHardwareAcceleratedRRT = false;
        spdlog::info("bufferDeviceAddress feature is not supported; disabling hardware ray tracing path.");
    }

    const bool hasMaintenance4Feature = available_features13.maintenance4 == VK_TRUE;

    const bool hasRequiredRayTracingFeatures = availableAccelerationStructureFeatures.accelerationStructure == VK_TRUE
                                               && availableRayTracingPipelineFeatures.rayTracingPipeline == VK_TRUE
                                               && availableRayQueryFeature.rayQuery == VK_TRUE;

    if (deviceSupportsHardwareAcceleratedRRT && !hasRequiredRayTracingFeatures) {
        deviceSupportsHardwareAcceleratedRRT = false;
        spdlog::default_logger_raw()->log(spdlog::level::info,
          std::string("Required ray tracing features are not fully supported (accelerationStructure=")
            + (availableAccelerationStructureFeatures.accelerationStructure == VK_TRUE ? "true" : "false")
            + ", rayTracingPipeline="
            + (availableRayTracingPipelineFeatures.rayTracingPipeline == VK_TRUE ? "true" : "false")
            + ", rayQuery=" + (availableRayQueryFeature.rayQuery == VK_TRUE ? "true" : "false")
            + "); disabling hardware ray tracing path.");
    }

    if (deviceSupportsHardwareAcceleratedRRT && !hasMaintenance4Feature) {
        deviceSupportsHardwareAcceleratedRRT = false;
        spdlog::default_logger_raw()->log(spdlog::level::info,
          "Vulkan 1.3 maintenance4 feature is not supported; disabling hardware ray tracing/path tracing path.");
    }

    if (deviceSupportsHardwareAcceleratedRRT) {
        // VMA suballocates, so SBT and AS-scratch address alignment must be requested explicitly.
        vk::PhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure_properties{};
        vk::PhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_pipeline_properties{};
        ray_tracing_pipeline_properties.pNext = &acceleration_structure_properties;
        vk::PhysicalDeviceProperties2 properties2{};
        properties2.pNext = &ray_tracing_pipeline_properties;
        physical_device.getProperties2(&properties2);

        deviceAddressAlignment = std::max<vk::DeviceSize>({ vk::DeviceSize{ 1 },
          ray_tracing_pipeline_properties.shaderGroupBaseAlignment,
          acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment });

        // COPY ALL NECESSARY EXTENSIONS FOR RAYTRACING TO THE EXTENSION
        extensions.insert(
          extensions.begin(), device_extensions_for_raytracing.begin(), device_extensions_for_raytracing.end());

        features12.bufferDeviceAddress = true;
        features12.descriptorIndexing = true;
        features12.runtimeDescriptorArray = true;
        features12.shaderSampledImageArrayNonUniformIndexing = true;
        features13.maintenance4 = true;

        acceleration_structure_features.accelerationStructure = true;
        acceleration_structure_features.accelerationStructureCaptureReplay = false;
        ray_tracing_pipeline_features.rayTracingPipeline = true;
        rayQueryFeature.rayQuery = true;
        features12.pNext = &rayQueryFeature;
        features13.pNext = &acceleration_structure_features;
    }

    if (!deviceSupportsHardwareAcceleratedRRT && !hasBufferDeviceAddressFeature) {
        spdlog::info("bufferDeviceAddress feature is not supported; related shader capabilities may be unavailable.");
    }

    // Slang emits derivative-group quads for compute shaders; request them only once proven, as linear-only devices refuse.
    vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR computeDerivativeFeatures{};
    const bool computeDerivativeExtensionPresent =
      supportsExtension(availableExtensions, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
    if (shouldEnableComputeDerivativeGroupQuads(
          computeDerivativeExtensionPresent, availableComputeDerivativeFeatures.computeDerivativeGroupQuads)) {
        extensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
        computeDerivativeFeatures.computeDerivativeGroupQuads = VK_TRUE;
        computeDerivativeFeatures.pNext = features2.pNext;
        features2.pNext = &computeDerivativeFeatures;
    }

    // information to create logical device (sometimes called "device")
    vk::DeviceCreateInfo device_create_info{};
    device_create_info.queueCreateInfoCount =
      static_cast<uint32_t>(queue_create_infos.size());// number of queue create infos
    device_create_info.pQueueCreateInfos = queue_create_infos.data();// list of queue create infos so device can
                                                                     // create required queues
    device_create_info.enabledExtensionCount =
      static_cast<uint32_t>(extensions.size());// number of enabled logical device extensions
    device_create_info.ppEnabledExtensionNames = extensions.data();// list of enabled logical device extensions
    device_create_info.flags = {};
    device_create_info.pEnabledFeatures = nullptr;
    device_create_info.pNext = &features2;

    // create logical device for the given physical device
    VkDevice c_device;
    VkResult const result = vkCreateDevice(static_cast<VkPhysicalDevice>(physical_device),
      reinterpret_cast<const VkDeviceCreateInfo *>(&device_create_info),
      nullptr,
      &c_device);
    logical_device = c_device;
    if (result != VK_SUCCESS) {
        spdlog::critical("Failed to create logical device. Vulkan Result: {}", static_cast<int>(result));
    }
    ASSERT_VULKAN(result, "Failed to create a logical device!");
    if (result != VK_SUCCESS || !logical_device) {
        spdlog::critical("Unable to continue without a valid Vulkan logical device.");
        std::abort();
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(logical_device);

    graphics_queue = logical_device.getQueue(static_cast<uint32_t>(indices.graphics_family), 0);
    presentation_queue = logical_device.getQueue(static_cast<uint32_t>(indices.presentation_family), 0);

    // Ask VMA for buffer device addresses only when the logical device enabled the feature.
    allocator =
      Allocator(logical_device, physical_device, instance->getVulkanInstance(), deviceSupportsBufferDeviceAddress);
}

auto Kataglyphis::VulkanDevice::getQueueFamilies(vk::PhysicalDevice selectedPhysicalDevice)
  -> Kataglyphis::VulkanRendererInternals::QueueFamilyIndices
{
    Kataglyphis::VulkanRendererInternals::QueueFamilyIndices indices{};

    std::vector<vk::QueueFamilyProperties> queue_family_list = selectedPhysicalDevice.getQueueFamilyProperties();

    uint32_t index = 0;
    for (const auto &queue_family : queue_family_list) {
        if (queue_family.queueCount > 0 && (queue_family.queueFlags & vk::QueueFlagBits::eGraphics)) {
            indices.graphics_family = static_cast<int>(index);// if queue family valid, than get index
        }

        if (queue_family.queueCount > 0 && (queue_family.queueFlags & vk::QueueFlagBits::eCompute)) {
            indices.compute_family = static_cast<int>(index);
        }

        // A failed query must not read as "no presentation support", which misreports device selection.
        auto presentation_support_result = selectedPhysicalDevice.getSurfaceSupportKHR(index, *surface);
        ASSERT_VULKAN(presentation_support_result.result, "Failed to query surface presentation support!");
        vk::Bool32 presentation_support = presentation_support_result.value;
        if (queue_family.queueCount > 0 && presentation_support) {
            indices.presentation_family = static_cast<int>(index);
        }

        // check if queue family indices are in a valid state
        if (indices.is_valid()) { break; }

        index++;
    }

    return indices;
}

auto Kataglyphis::VulkanDevice::getSwapchainDetails(vk::PhysicalDevice device)
  -> Kataglyphis::VulkanRendererInternals::SwapChainDetails
{
    Kataglyphis::VulkanRendererInternals::SwapChainDetails swapchain_details{};

    // Fatal: default-constructed capabilities would silently build a zero-sized swapchain.
    auto surface_capabilities_result = device.getSurfaceCapabilitiesKHR(*surface);
    ASSERT_VULKAN(surface_capabilities_result.result, "Failed to query surface capabilities!");
    swapchain_details.surface_capabilities = surface_capabilities_result.value;

    // get list of formats
    auto formats_result = device.getSurfaceFormatsKHR(*surface);
    if (formats_result.result != vk::Result::eSuccess) {
        spdlog::warn("vkGetPhysicalDeviceSurfaceFormatsKHR failed for device '{}' (result {}); treating as no "
                     "supported formats.",
          device.getProperties().deviceName.data(),
          static_cast<int>(formats_result.result));
    }
    swapchain_details.formats = formats_result.value;

    // get list of presentation modes
    auto presentation_modes_result = device.getSurfacePresentModesKHR(*surface);
    if (presentation_modes_result.result != vk::Result::eSuccess) {
        spdlog::warn("vkGetPhysicalDeviceSurfacePresentModesKHR failed for device '{}' (result {}); treating as no "
                     "supported presentation modes.",
          device.getProperties().deviceName.data(),
          static_cast<int>(presentation_modes_result.result));
    }
    swapchain_details.presentation_mode = presentation_modes_result.value;

    return swapchain_details;
}

auto Kataglyphis::VulkanDevice::check_device_suitable(vk::PhysicalDevice device) -> bool
{
    Kataglyphis::VulkanRendererInternals::QueueFamilyIndices indices = getQueueFamilies(device);

    bool const extensions_supported = check_device_extension_support(device);

    bool swap_chain_valid = false;

    if (extensions_supported) {
        Kataglyphis::VulkanRendererInternals::SwapChainDetails const swap_chain_details = getSwapchainDetails(device);
        swap_chain_valid = !swap_chain_details.presentation_mode.empty() && !swap_chain_details.formats.empty();
    }

    // samplerAnisotropy is not required: it has a fallback, and requiring it rejects software Vulkan such as lavapipe.
    return indices.is_valid() && extensions_supported && swap_chain_valid;
}

auto Kataglyphis::VulkanDevice::check_device_extension_support(vk::PhysicalDevice device) -> bool
{
    auto extensions_result = device.enumerateDeviceExtensionProperties();
    if (extensions_result.result != vk::Result::eSuccess) {
        spdlog::warn("vkEnumerateDeviceExtensionProperties failed for device '{}' (result {}); treating as no "
                     "supported extensions.",
          device.getProperties().deviceName.data(),
          static_cast<int>(extensions_result.result));
    }
    std::vector<vk::ExtensionProperties> extensions = extensions_result.value;

    return firstMissingExtension(extensions, device_extensions) == nullptr;
}

auto Kataglyphis::VulkanDevice::getBufferDeviceAddress(const vk::BufferDeviceAddressInfo &info) const
  -> vk::DeviceAddress
{
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddress(
      static_cast<VkDevice>(logical_device), reinterpret_cast<const VkBufferDeviceAddressInfo *>(&info));
}
