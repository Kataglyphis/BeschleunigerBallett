module;

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.exposure_stage;

import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.descriptor_set_group;
import kataglyphis.vulkan.device;

export namespace Kataglyphis::VulkanRendererInternals {

// One frame's inputs to the reduction: histogram.slang's ExposureParams.
struct ExposureFrameParams
{
    float deltaSeconds = 0.0F;
    float speed = 0.0F;
    bool autoExposure = false;
    float manualEv = 0.0F;
};

// OxidANT's histogram.slang into the [adapted EV, target EV] buffer post reads; manual exposure writes it too.
class ExposureStage
{
  public:
    ExposureStage();

    ExposureStage(const ExposureStage &) = delete;
    ExposureStage &operator=(const ExposureStage &) = delete;

    // The buffer starts at [initialEv, initialEv], written by the first frame's reduce pass.
    void init(const std::shared_ptr<VulkanDevice> &in_device, uint32_t imageCount, float initialEv);
    // A new swapchain image count: one parameter buffer and set per image; the HDR bindings follow bindHdrTarget.
    void recreateFrameResources(uint32_t imageCount);
    void bindHdrTarget(uint32_t imageIndex, vk::ImageView hdrView);
    void shaderHotReload();

    // Clears and rebuilds the bins from the HDR target its last writer left in eShaderReadOnlyOptimal.
    void recordHistogram(vk::CommandBuffer commandBuffer, uint32_t imageIndex, vk::Extent2D extent);
    // Every frame: auto adapts towards the bins' target, manual writes its EV; leaves the buffer for fragment reads.
    void recordReduce(vk::CommandBuffer commandBuffer, uint32_t imageIndex, const ExposureFrameParams &params);

    // The next reduce starts from [ev, ev], as after a scene change, instead of the last adapted value.
    void requestReset(float ev);

    [[nodiscard]] vk::Buffer exposureBuffer() { return exposureState.getBuffer(); }
    static constexpr vk::DeviceSize kExposureStateBytes = 2 * sizeof(float);

    // Test seam: the next reduce also copies the state out; takeCapture waits on that frame's fence.
    void requestCapture();
    void bindCaptureFence(vk::Fence fence);
    void invalidateCaptureFence();
    [[nodiscard]] std::optional<std::array<float, 2>> takeCapture();

    void cleanUp();

    ~ExposureStage();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };

    VulkanBuffer histogram;
    VulkanBuffer exposureState;
    std::vector<VulkanBuffer> parameters{};
    DescriptorSetGroup descriptors;

    vk::PipelineLayout clearLayout{};
    vk::Pipeline clearPipeline{};
    vk::PipelineLayout buildLayout{};
    vk::Pipeline buildPipeline{};
    vk::PipelineLayout reduceLayout{};
    vk::Pipeline reducePipeline{};

    std::optional<float> pendingReset{};

    VulkanBuffer captureReadback;
    bool captureArmed{ false };
    bool capturePending{ false };
    vk::Fence captureFence{};

    void createPipelines();
    void destroyPipelines();
    [[nodiscard]] bool ready(uint32_t imageIndex) const;
};

}// namespace Kataglyphis::VulkanRendererInternals
