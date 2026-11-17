module;
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.punctual_shadow_map;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.descriptor_set_group;
// Re-exported: setSlots takes the slots assignPunctualShadowSlots returns.
export import kataglyphis.vulkan.punctual_light;

export namespace Kataglyphis {

/// Texels per side of each cube face; fixed, as a light's cube costs six of the sun's cascade passes.
inline constexpr uint32_t PUNCTUAL_SHADOW_RESOLUTION = 512;

/// Bytes between two lights' face matrices in the per-image buffer: six mat4 padded to any dynamic-offset alignment.
inline constexpr vk::DeviceSize PUNCTUAL_SHADOW_SLOT_STRIDE = 512;

/// Whether a caster's world box reaches the sphere a light's cube covers; boxes of unknown extent always do.
bool castsIntoPunctualShadow(const AABB &casterBounds, const glm::vec3 &lightPosition, float farPlane);

/// The world bounds of every mesh of `scene`, placed by its model; inverted when the scene has none.
AABB sceneWorldBounds(Scene &scene);

/// Depth cube maps for the first point and spot lights: one multiview pass per light renders all six faces.
class PunctualShadowMaps
{
  public:
    PunctualShadowMaps() = default;
    PunctualShadowMaps(const PunctualShadowMaps &) = delete;
    PunctualShadowMaps &operator=(const PunctualShadowMaps &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &device,
      vk::DescriptorSetLayout sharedRenderDescriptorSetLayout,
      uint32_t swapChainImageCount);
    void createGraphicsPipeline();
    void shaderHotReload();

    /// Takes the scene's shadowed lights; reallocates the cube array, so the caller drains the GPU first and
    /// rewrites the descriptor after. A device without cube arrays or six views keeps none.
    void setSlots(std::span<const PunctualShadowSlot> slots);
    [[nodiscard]] uint32_t slotCount() const { return static_cast<uint32_t>(slots.size()); }
    [[nodiscard]] bool isSupported() const;

    /// Writes the face matrices into this swapchain image's buffer, before its pass reads them.
    void uploadFaceMatrices(uint32_t image_index);
    void recordCommands(vk::CommandBuffer commandBuffer,
      uint32_t image_index,
      Scene *scene,
      vk::DescriptorSet sharedRenderSet,
      bool cullingEnabled);
    /// Without a pass the lighting shaders still bind the array, so it must leave UNDEFINED.
    void recordSkippedPass(vk::CommandBuffer commandBuffer);

    Texture *getCubeArray() const { return cubeArray.get(); }
    unsigned int getCastersDrawn() const { return castersDrawn; }
    unsigned int getCastersConsidered() const { return castersConsidered; }

    void cleanUp();
    ~PunctualShadowMaps() { cleanUp(); }

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };
    uint32_t swapChainImageCount{ 0 };
    vk::Format depthFormat{ vk::Format::eD32Sfloat };
    uint32_t resolution{ 1 };
    unsigned int castersDrawn{ 0 };
    unsigned int castersConsidered{ 0 };

    std::vector<PunctualShadowSlot> slots;
    std::array<glm::mat4, MAX_SHADOWED_PUNCTUAL_LIGHTS * 6> faceMatrices{};

    // Sampled through a cube-array view; each light also has a 2D-array view of its six layers to render into.
    std::unique_ptr<Texture> cubeArray;
    std::vector<vk::ImageView> slotViews;
    uint32_t cubeCount{ 0 };

    vk::Pipeline graphicsPipeline{};
    vk::PipelineLayout pipelineLayout{};
    DescriptorSetGroup faceMatrixDescriptors;
    // Owned by VulkanRenderer's sharedRenderDescriptors; bound at set 0 for the alpha test, never destroyed here.
    vk::DescriptorSetLayout sharedRenderDescriptorSetLayout{};
    std::vector<VulkanBuffer> faceMatrixBuffers;

    void createCubeArray(uint32_t cubes, uint32_t size);
    void releaseCubeArray();
    void buildGraphicsPipeline();
    void recordLight(vk::CommandBuffer commandBuffer,
      uint32_t slot,
      Scene *scene,
      std::span<const vk::DescriptorSet, 2> sets,
      bool cullingEnabled);
};

}// namespace Kataglyphis
