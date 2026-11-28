#include <gtest/gtest.h>

#include "vulkan_base/PhysicalDeviceChoices.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

import kataglyphis.vulkan.queue_family_indices;

namespace {

// By text: two copies of one literal need not share an address.
std::vector<std::string> extensionNames(const std::vector<const char *> &extensions)
{ return std::vector<std::string>(extensions.begin(), extensions.end()); }

vk::PhysicalDeviceProperties makeProperties(vk::PhysicalDeviceType type, uint32_t maxImageDimension2D)
{
    vk::PhysicalDeviceProperties properties{};
    properties.deviceType = type;
    properties.limits.maxImageDimension2D = maxImageDimension2D;
    return properties;
}

TEST(PhysicalDeviceChoicesUnit, DeviceTypeOutranksTheCapabilityTieBreak)
{
    const vk::PhysicalDeviceProperties discrete = makeProperties(vk::PhysicalDeviceType::eDiscreteGpu, 8192);
    const vk::PhysicalDeviceProperties integrated = makeProperties(vk::PhysicalDeviceType::eIntegratedGpu, 32768);

    EXPECT_GT(Kataglyphis::scorePhysicalDevice(discrete), Kataglyphis::scorePhysicalDevice(integrated));
}

TEST(PhysicalDeviceChoicesUnit, CapabilityBreaksTiesWithinOneType)
{
    const vk::PhysicalDeviceProperties higher = makeProperties(vk::PhysicalDeviceType::eDiscreteGpu, 16384);
    const vk::PhysicalDeviceProperties lower = makeProperties(vk::PhysicalDeviceType::eDiscreteGpu, 8192);

    EXPECT_GT(Kataglyphis::scorePhysicalDevice(higher), Kataglyphis::scorePhysicalDevice(lower));
}

TEST(PhysicalDeviceChoicesUnit, ACpuDeviceNeverOutranksAnyGpu)
{
    const vk::PhysicalDeviceProperties cpu = makeProperties(vk::PhysicalDeviceType::eCpu, 32768);
    const vk::PhysicalDeviceProperties virtual_gpu = makeProperties(vk::PhysicalDeviceType::eVirtualGpu, 4096);

    EXPECT_LT(Kataglyphis::scorePhysicalDevice(cpu), Kataglyphis::scorePhysicalDevice(virtual_gpu));
}

TEST(PhysicalDeviceChoicesUnit, SelectionModeParsingIsCaseInsensitiveAndDefaultsToAuto)
{
    EXPECT_EQ(Kataglyphis::parseGpuSelectionMode("Dedicated"), Kataglyphis::GpuSelectionMode::Dedicated);
    EXPECT_EQ(Kataglyphis::parseGpuSelectionMode("INTEGRATED"), Kataglyphis::GpuSelectionMode::Integrated);
    EXPECT_EQ(Kataglyphis::parseGpuSelectionMode(""), Kataglyphis::GpuSelectionMode::Auto);
    EXPECT_EQ(Kataglyphis::parseGpuSelectionMode("nonsense"), Kataglyphis::GpuSelectionMode::Auto);
}

TEST(PhysicalDeviceChoicesUnit, ComputeDerivativeQuadsNeedBothTheExtensionAndTheFeature)
{
    EXPECT_TRUE(Kataglyphis::shouldEnableComputeDerivativeGroupQuads(true, VK_TRUE));
    EXPECT_FALSE(Kataglyphis::shouldEnableComputeDerivativeGroupQuads(true, VK_FALSE));
    EXPECT_FALSE(Kataglyphis::shouldEnableComputeDerivativeGroupQuads(false, VK_TRUE));
    EXPECT_FALSE(Kataglyphis::shouldEnableComputeDerivativeGroupQuads(false, VK_FALSE));
}

TEST(PhysicalDeviceChoicesUnit, MaxAnisotropyIsClampedToTheDeviceLimit)
{
    EXPECT_FLOAT_EQ(Kataglyphis::resolveMaxAnisotropy(false, 16.0F), 1.0F);
    EXPECT_FLOAT_EQ(Kataglyphis::resolveMaxAnisotropy(true, 16.0F), 16.0F);
    EXPECT_FLOAT_EQ(Kataglyphis::resolveMaxAnisotropy(true, 2.0F), 2.0F);
    EXPECT_FLOAT_EQ(Kataglyphis::resolveMaxAnisotropy(true, 1.0F), 1.0F);
}

TEST(PhysicalDeviceChoicesUnit, RenderingNeedsVulkan13WithDynamicRenderingAndSynchronization2)
{
    EXPECT_EQ(Kataglyphis::firstMissingRenderingRequirement(VK_API_VERSION_1_3, VK_TRUE, VK_TRUE), nullptr);
    EXPECT_EQ(Kataglyphis::firstMissingRenderingRequirement(VK_API_VERSION_1_4, VK_TRUE, VK_TRUE), nullptr);
    // A 1.2 device is named by its version first, even when it exposes the features through extensions.
    EXPECT_STREQ(Kataglyphis::firstMissingRenderingRequirement(VK_API_VERSION_1_2, VK_TRUE, VK_TRUE), "Vulkan 1.3");
    EXPECT_STREQ(Kataglyphis::firstMissingRenderingRequirement(VK_API_VERSION_1_3, VK_FALSE, VK_TRUE),
      "the dynamicRendering feature");
    EXPECT_STREQ(Kataglyphis::firstMissingRenderingRequirement(VK_API_VERSION_1_3, VK_TRUE, VK_FALSE),
      "the synchronization2 feature");
}

TEST(PhysicalDeviceChoicesUnit, DeviceLossDiagnosticsPreferTheBufferMarker)
{
    using Kataglyphis::BreadcrumbBackend;
    using Kataglyphis::DeviceDiagnosticsOffer;
    const auto both_markers = Kataglyphis::chooseDeviceDiagnostics(DeviceDiagnosticsOffer{ .bufferMarker = true,
      .checkpoints = true,
      .deviceCoherentMemoryListed = true,
      .deviceCoherentMemoryFeature = VK_TRUE,
      .deviceFaultListed = false,
      .deviceFaultFeature = VK_FALSE });
    EXPECT_EQ(both_markers.backend, BreadcrumbBackend::BufferMarker);
    EXPECT_TRUE(both_markers.deviceCoherentMemory);
    EXPECT_EQ(extensionNames(Kataglyphis::deviceDiagnosticExtensions(both_markers)),
      (std::vector<std::string>{ VK_AMD_BUFFER_MARKER_EXTENSION_NAME, VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME }));

    // Coherent memory serves only the buffer marker, so a checkpoint device leaves it off.
    const auto checkpoints = Kataglyphis::chooseDeviceDiagnostics(DeviceDiagnosticsOffer{ .bufferMarker = false,
      .checkpoints = true,
      .deviceCoherentMemoryListed = true,
      .deviceCoherentMemoryFeature = VK_TRUE,
      .deviceFaultListed = true,
      .deviceFaultFeature = VK_TRUE });
    EXPECT_EQ(checkpoints.backend, BreadcrumbBackend::Checkpoints);
    EXPECT_FALSE(checkpoints.deviceCoherentMemory);
    EXPECT_TRUE(checkpoints.deviceFault);
    EXPECT_EQ(extensionNames(Kataglyphis::deviceDiagnosticExtensions(checkpoints)),
      (std::vector<std::string>{
        VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, VK_EXT_DEVICE_FAULT_EXTENSION_NAME }));
}

TEST(PhysicalDeviceChoicesUnit, ADiagnosticExtensionListedWithoutItsFeatureStaysOff)
{
    using Kataglyphis::DeviceDiagnosticsOffer;
    const auto without_features = Kataglyphis::chooseDeviceDiagnostics(DeviceDiagnosticsOffer{ .bufferMarker = true,
      .checkpoints = false,
      .deviceCoherentMemoryListed = true,
      .deviceCoherentMemoryFeature = VK_FALSE,
      .deviceFaultListed = true,
      .deviceFaultFeature = VK_FALSE });
    EXPECT_FALSE(without_features.deviceCoherentMemory) << "VMA's coherent-memory flag needs the feature";
    EXPECT_FALSE(without_features.deviceFault) << "vkGetDeviceFaultInfoEXT needs the deviceFault feature";
    EXPECT_EQ(extensionNames(Kataglyphis::deviceDiagnosticExtensions(without_features)),
      (std::vector<std::string>{ VK_AMD_BUFFER_MARKER_EXTENSION_NAME }));

    // A feature reported for an extension the device does not list cannot be enabled either.
    const auto unlisted = Kataglyphis::chooseDeviceDiagnostics(DeviceDiagnosticsOffer{ .bufferMarker = true,
      .checkpoints = false,
      .deviceCoherentMemoryListed = false,
      .deviceCoherentMemoryFeature = VK_TRUE,
      .deviceFaultListed = false,
      .deviceFaultFeature = VK_TRUE });
    EXPECT_FALSE(unlisted.deviceCoherentMemory);
    EXPECT_FALSE(unlisted.deviceFault);
}

TEST(PhysicalDeviceChoicesUnit, ADeviceOfferingNoDiagnosticsRequestsNoExtensionForThem)
{
    const auto none = Kataglyphis::chooseDeviceDiagnostics(Kataglyphis::DeviceDiagnosticsOffer{});
    EXPECT_EQ(none.backend, Kataglyphis::BreadcrumbBackend::None);
    EXPECT_FALSE(none.deviceCoherentMemory);
    EXPECT_FALSE(none.deviceFault);
    EXPECT_TRUE(Kataglyphis::deviceDiagnosticExtensions(none).empty());
    EXPECT_STREQ(Kataglyphis::breadcrumbBackendName(none.backend), "none");
}

TEST(PhysicalDeviceChoicesUnit, MaterialShadersTakeTheUniformTwinExactlyWhenNonUniformSampledImageIndexingIsMissing)
{
    using Kataglyphis::MaterialTextureIndexing;
    using Kataglyphis::materialTextureIndexingFor;
    vk::PhysicalDeviceFeatures dynamic{};
    dynamic.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    vk::PhysicalDeviceVulkan12Features offered{};
    offered.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    EXPECT_EQ(materialTextureIndexingFor(dynamic, offered), MaterialTextureIndexing::NonUniform);
    EXPECT_EQ(materialTextureIndexingFor(dynamic, offered, true), MaterialTextureIndexing::Uniform)
      << "the goldens' knob must load the twins on a device that has both features";

    // Other descriptor-indexing bits do not stand in for the sampled-image one; descriptorIndexing would imply it.
    vk::PhysicalDeviceVulkan12Features others{};
    others.runtimeDescriptorArray = VK_TRUE;
    others.shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
    others.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    others.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    others.descriptorBindingPartiallyBound = VK_TRUE;
    EXPECT_EQ(materialTextureIndexingFor(dynamic, others), MaterialTextureIndexing::Uniform);
    EXPECT_EQ(
      materialTextureIndexingFor(dynamic, vk::PhysicalDeviceVulkan12Features{}), MaterialTextureIndexing::Uniform);
}

TEST(PhysicalDeviceChoicesUnit, ADeviceWithoutDynamicSampledImageIndexingHasNoValidMaterialVariant)
{
    using Kataglyphis::MaterialTextureIndexing;
    using Kataglyphis::materialTextureIndexingFor;
    // v3dv's shape (V3D 7.1, Mesa 26.2): every descriptor-indexing bit and dynamic sampled-image indexing are false.
    const vk::PhysicalDeviceFeatures v3dv{};
    const vk::PhysicalDeviceVulkan12Features v3dv12{};
    EXPECT_EQ(materialTextureIndexingFor(v3dv, v3dv12), MaterialTextureIndexing::None);
    EXPECT_EQ(materialTextureIndexingFor(v3dv, v3dv12, true), MaterialTextureIndexing::None)
      << "the knob must not hide a device that cannot take either variant";

    // Non-uniform indexing alone does not help: a non-uniform index is a variable index first.
    vk::PhysicalDeviceVulkan12Features nonUniformOnly{};
    nonUniformOnly.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    EXPECT_EQ(materialTextureIndexingFor(v3dv, nonUniformOnly), MaterialTextureIndexing::None);

    // The nearest variant still loads, so the device runs what its driver tolerates.
    EXPECT_EQ(Kataglyphis::materialShaderPath("rasterizer/rasterizer.fs_main.spv", MaterialTextureIndexing::None),
      "rasterizer/rasterizer_uniform_index.fs_main.spv");
}

TEST(PhysicalDeviceChoicesUnit, TheUniformTwinKeepsTheDirectoryAndEntryAndSuffixesTheStem)
{
    using Kataglyphis::materialShaderPath;
    constexpr auto kUniform = Kataglyphis::MaterialTextureIndexing::Uniform;
    const std::string dir = "Resources/ShadersSlang/build/spirv/";
    EXPECT_EQ(materialShaderPath(dir + "rasterizer/rasterizer.fs_main.spv", kUniform),
      dir + "rasterizer/rasterizer_uniform_index.fs_main.spv");
    EXPECT_EQ(materialShaderPath(dir + "rasterizer/shadows/shadow_map.shadow_fs_main.spv", kUniform),
      dir + "rasterizer/shadows/shadow_map_uniform_index.shadow_fs_main.spv");
    // Only the entry follows the last dot, so a dotted stem keeps its dots.
    EXPECT_EQ(
      materialShaderPath("raytrace.rchit.rchit_main.spv", kUniform), "raytrace.rchit_uniform_index.rchit_main.spv");
    EXPECT_EQ(
      materialShaderPath(dir + "rasterizer/rasterizer.fs_main.spv", Kataglyphis::MaterialTextureIndexing::NonUniform),
      dir + "rasterizer/rasterizer.fs_main.spv");
    // Not "<stem>.<entry>.spv": left alone, so the load names the file asked for instead of a third one.
    EXPECT_EQ(materialShaderPath("build.v2/rasterizer.spv", kUniform), "build.v2/rasterizer.spv");
    EXPECT_EQ(materialShaderPath("rasterizer/.fs_main.spv", kUniform), "rasterizer/.fs_main.spv");
    EXPECT_EQ(materialShaderPath("rasterizer/rasterizer.fs_main.wgsl", kUniform), "rasterizer/rasterizer.fs_main.wgsl");
    EXPECT_EQ(materialShaderPath(".spv", kUniform), ".spv");
}

TEST(PhysicalDeviceChoicesUnit, EachMaterialChoiceLogsTheFeatureItLacks)
{
    using Kataglyphis::MaterialTextureIndexing;
    using Kataglyphis::materialTextureIndexingLogLine;
    const std::string_view non_uniform = materialTextureIndexingLogLine(MaterialTextureIndexing::NonUniform);
    const std::string_view uniform = materialTextureIndexingLogLine(MaterialTextureIndexing::Uniform);
    const std::string_view none = materialTextureIndexingLogLine(MaterialTextureIndexing::None);
    EXPECT_NE(uniform.find("shaderSampledImageArrayNonUniformIndexing"), std::string_view::npos);
    EXPECT_EQ(non_uniform.find("shaderSampledImageArrayNonUniformIndexing"), std::string_view::npos);
    EXPECT_NE(none.find("shaderSampledImageArrayDynamicIndexing"), std::string_view::npos);
    EXPECT_NE(none.find("no valid variant"), std::string_view::npos);
    EXPECT_EQ(uniform.find("no valid variant"), std::string_view::npos);
    EXPECT_NE(uniform, non_uniform);
}

TEST(QueueFamilyIndicesUnit, CreatesNoQueueOnTheComputeFamily)
{
    using Kataglyphis::VulkanRendererInternals::QueueFamilyIndices;
    const QueueFamilyIndices separate{ .graphics_family = 0, .presentation_family = 1, .compute_family = 2 };
    EXPECT_EQ(separate.queueFamiliesToCreate(), (std::vector<uint32_t>{ 0U, 1U }));

    const QueueFamilyIndices shared{ .graphics_family = 0, .presentation_family = 0, .compute_family = 0 };
    EXPECT_EQ(shared.queueFamiliesToCreate(), (std::vector<uint32_t>{ 0U }));

    const QueueFamilyIndices present_first{ .graphics_family = 2, .presentation_family = 1, .compute_family = 3 };
    EXPECT_EQ(present_first.queueFamiliesToCreate(), (std::vector<uint32_t>{ 1U, 2U }));
}

}// namespace
