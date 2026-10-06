module;
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "renderer/accelerationStructures/BottomLevelAccelerationStructure.hpp"
#include "renderer/accelerationStructures/TopLevelAccelerationStructure.hpp"

export module kataglyphis.vulkan.as_manager;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.buffer_manager;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.mesh;
import kataglyphis.vulkan.buffer;

export namespace Kataglyphis::VulkanRendererInternals {
struct BuildAccelerationStructure
{
    vk::AccelerationStructureBuildGeometryInfoKHR build_info;
    vk::AccelerationStructureBuildSizesInfoKHR size_info;
    const vk::AccelerationStructureBuildRangeInfoKHR *range_info;
    BottomLevelAccelerationStructure single_blas;
};

struct BlasInput
{
    std::vector<vk::AccelerationStructureGeometryKHR> as_geometry;
    std::vector<vk::AccelerationStructureBuildRangeInfoKHR> as_build_offset_info;
};

class ASManager
{
  public:
    ASManager();

    vk::AccelerationStructureKHR &getTLAS() { return tlas.vulkanAS; };

    void createASForScene(const std::shared_ptr<VulkanDevice> &device,
      vk::CommandPool commandPool,
      Kataglyphis::Scene *scene);

    bool
      createBLAS(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool commandPool, Kataglyphis::Scene *scene);

    void
      createTLAS(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool commandPool, Kataglyphis::Scene *scene);

    void cleanUp();

    ~ASManager();

  private:
    std::shared_ptr<VulkanDevice> vulkanDevice{ nullptr };
    Kataglyphis::VulkanBufferManager vulkanBufferManager;

    std::vector<BottomLevelAccelerationStructure> blas;
    TopLevelAccelerationStructure tlas;

    // Replaces every freshly built BLAS with a compacted copy. Compaction
    // typically reclaims a large fraction of BLAS memory at no trace-time
    // cost; it is legal because the builds carry eAllowCompaction and the
    // build submission is fully synchronous before this runs. TLAS is built
    // AFTER, so it picks up the compacted device addresses naturally.
    void compactBLAS(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool commandPool);

    static void createSingleBlas(const std::shared_ptr<VulkanDevice> &device,
      vk::CommandBuffer command_buffer,
      BuildAccelerationStructure &build_as_structure,
      vk::DeviceAddress scratch_device_or_host_address);

    static void createAccelerationStructureInfosBLAS(const std::shared_ptr<VulkanDevice> &device,
      BuildAccelerationStructure &build_as_structure,
      BlasInput &blas_input,
      vk::DeviceSize &current_scratch_size,
      vk::DeviceSize &current_size);

    static void objectToVkGeometryKHR(const std::shared_ptr<VulkanDevice> &device,
      Kataglyphis::Mesh *mesh,
      vk::AccelerationStructureGeometryKHR &acceleration_structure_geometry,
      vk::AccelerationStructureBuildRangeInfoKHR &acceleration_structure_build_range_info);
};
}// namespace Kataglyphis::VulkanRendererInternals
