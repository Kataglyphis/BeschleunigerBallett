module;
#include <vector>
#include <memory>
#include <span>
#include <vulkan/vulkan.hpp>
#include <glm/glm.hpp>

export module kataglyphis.vulkan.cascaded_shadow_map;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.descriptor_set_group;

export namespace Kataglyphis {

struct CascadeData {
    float splitDepth;
    glm::mat4 viewProjMatrix;
};

// Push constants consumed by rasterizer/shadows/shadow_map.slang.
struct ShadowPushConstants
{
    glm::mat4 model;
    uint32_t cascadeIndex;
};

// Free functions so the cascade maths and the caster transform can be tested
// WITHOUT a Vulkan device - CascadedShadowMap itself cannot be constructed
// without one, which is why neither had any coverage while a hard-coded
// identity model matrix silently disabled shadows entirely.

// Aggregate for the parameters shared by computeCascadeData and
// computeCascadeDataInto - six adjacent floats where a transposed argument
// still compiles, so callers build this by name instead.
struct CascadeFitParams {
    glm::mat4 cameraView{ 1.0F };
    float cameraFov{ 45.0F };
    float aspect{ 1.0F };
    float nearPlane{ 0.1F };
    float farPlane{ 100.0F };
    glm::vec3 lightDir{ 0.0F, -1.0F, 0.0F };
    // shadowDistance clamps how far shadows are fitted, independently of the
    // camera far plane - geometry beyond it is simply unshadowed, which is far
    // cheaper than spreading the map over space nothing occupies. Pass <= 0 to
    // fall back to farPlane (the old behaviour).
    float shadowDistance{ 0.0F };
    // splitLambda blends logarithmic (1.0) against uniform (0.0) splits. See
    // the measurements in the implementation before raising it: high lambda
    // starves subjects that are framed from a distance. Defaults to 0.0F to
    // track GUISceneSharedVars::cascade_split_lambda, whose 0.0F is the
    // measured choice pinned in guiSceneVarsRoundTripSuite.cpp.
    float splitLambda{ 0.0F };
    // shadowMapResolution (optional): when > 0, cascades are STABILIZED - the
    // light basis is world-fixed (pure rotation), the ortho box is sized from
    // the slice's bounding radius (camera-motion invariant), and its center
    // snaps to whole shadow-map texels. Without it the box is refitted to the
    // exact frustum corners every frame, so it translates AND resizes
    // continuously and every shadow edge shimmers as the camera moves. 0
    // keeps the legacy tight-fit behaviour. The stabilized box is a padded
    // square (radius + one texel), which trades a little texel density for
    // edges that hold still.
    uint32_t shadowMapResolution{ 0U };
};

// Splits and light-space matrices for `numCascades` cascades. Pure maths.
std::vector<CascadeData> computeCascadeData(uint32_t numCascades, const CascadeFitParams &params);

// The same maths, written into storage the CALLER owns, so the per-frame
// shadow update path allocates nothing at all. computeCascadeData above is a
// thin wrapper over this - the two are required to agree bit for bit.
//
// Writes exactly the first `numCascades` entries of `out`. If `out` is too
// short it writes NOTHING and returns: silently clamping would hand the
// caller a partly-stale cascade set that still looks well-formed, which is
// exactly the class of bug the mapped-UBO comment in updateCascades records.
void computeCascadeDataInto(std::span<CascadeData> out, uint32_t numCascades, const CascadeFitParams &params);

// The caster transform. This exists as a named function purely so a test can
// pin the invariant that was once broken: the shadow pass must transform
// casters by the SAME model matrix as the forward pass, not by identity.
ShadowPushConstants makeShadowPush(const glm::mat4 &modelMatrix, uint32_t cascadeIndex);

// vkCmdBindDescriptorSets arguments for the shadow pass's set 0/1 split. Named
// and pulled out for the same reason as makeShadowPush: the no-shared-set
// fallback used to bind the light-matrices set at set 0, against a pipeline
// layout (CascadedShadowMap.cpp's setLayouts) that says set 0 is the shared
// render set and set 1 is light matrices - so the vertex shader's set 1 read
// was never bound. The light matrices set must land at set index 1 in BOTH
// cases; only firstSet/setCount change.
struct ShadowSetBinding
{
    uint32_t firstSet;
    uint32_t setCount;

    // A defaulted MEMBER operator==, deliberately not the hidden-friend form
    //     friend bool operator==(const ShadowSetBinding &, const ShadowSetBinding &) = default;
    // which this was until 2026-08-06. That form makes GCC 16.1.0 die with
    // "internal compiler error: Segmentation fault" pointing at this line -
    // not while compiling the module, but while IMPORTING it (the failing TU
    // is cascadedShadowMapSuite.cpp, which does `import
    // kataglyphis.vulkan.cascaded_shadow_map`). It broke the whole gcc lane;
    // the lane was green on 2026-07-23 and this struct arrived on 2026-08-03.
    // clang-cl and clang compile either form happily, which is why only the
    // GNU preset noticed.
    //
    // A GCC bug, not a rule about how to write this - and worth reporting
    // upstream. It reduces no further: the same pattern in a standalone module
    // compiles fine under the same compiler, so it needs this module's full
    // context. Do NOT "tidy" it back to a hidden friend without rebuilding
    // preset linux-debug-GNU first.
    //
    // Semantically equivalent here: both give member-wise == on the two
    // uint32_t fields, and C++20's rewritten candidates cover `b == a` too.
    bool operator==(const ShadowSetBinding &) const = default;
};
ShadowSetBinding shadowSetBinding(bool hasSharedSet);

// Cascade count actually usable: never above maxCascades (the SceneUBO array
// size the shader samples), never above deviceViewLimit (the multiview render
// pass broadcasts one view per cascade, so a viewMask bit past the device's
// maxMultiviewViewCount is a validation error / non-conformant render pass),
// and never below 1 - even if that floor pushes the result above one of the
// limits, since 0 cascades is not a renderable state.
uint32_t clampCascadeCount(uint32_t requested, uint32_t maxCascades, uint32_t deviceViewLimit);

class CascadedShadowMap
{
  public:
    CascadedShadowMap() = default;

    CascadedShadowMap(const CascadedShadowMap &) = delete;
    CascadedShadowMap &operator=(const CascadedShadowMap &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &device, uint32_t width, uint32_t height, uint32_t num_cascades,
      vk::DescriptorSetLayout sharedRenderDescriptorSetLayout, uint32_t swapChainImageCount,
      vk::CommandPool commandPool);

    void createGraphicsPipeline();
    void shaderHotReload();
    void recordCommands(vk::CommandBuffer &commandBuffer, uint32_t image_index, Scene *scene, std::span<const vk::DescriptorSet> descriptorSets, bool cullingEnabled);
    // With shadows off: the raster pipeline still binds the array, so it must leave UNDEFINED without a pass.
    void recordSkippedPass(vk::CommandBuffer &commandBuffer);

    Kataglyphis::Texture* getShadowMapArray() const { return shadowMapArray.get(); }
    vk::RenderPass getRenderPass() const { return renderPass; }

    // Passes the map resolution through, so live cascades are stabilized.
    void updateCascades(const glm::mat4 &cameraView,
      float cameraFov,
      float aspect,
      float nearPlane,
      float farPlane,
      const glm::vec3 &lightDir,
      float shadowDistance = 0.0F,
      float splitLambda = 0.0F);
    const std::vector<CascadeData>& getCascadeData() const { return cascadeData; }

    // Uploads the current cascadeData into this swapchain image's own light
    // matrices buffer. Call once per image_index, before recordCommands binds
    // that image's descriptor set - see the caller in
    // VulkanRenderer::update_uniform_buffers for why this must happen per
    // image rather than once per frame.
    void uploadLightMatrices(uint32_t image_index);

    /// Caster draws submitted / considered across all cascades in the last
    /// recordCommands call. Summed over cascades, so a 3-cascade scene with
    /// one mesh considers 3.
    unsigned int getCastersDrawn() const { return castersDrawn; }
    unsigned int getCastersConsidered() const { return castersConsidered; }

    void cleanUp();
    ~CascadedShadowMap() { cleanUp(); }

  private:
    unsigned int castersDrawn{ 0 };
    unsigned int castersConsidered{ 0 };
    std::shared_ptr<VulkanDevice>device{ nullptr };
    uint32_t shadowWidth{ 0 };
    uint32_t shadowHeight{ 0 };
    uint32_t numCascades{ 0 };
    uint32_t swapChainImageCount{ 0 };
    // Owned by VulkanRenderer, not this class - never created/destroyed here.
    vk::CommandPool commandPool{};

    std::unique_ptr<Kataglyphis::Texture> shadowMapArray;
    // Depth format chosen in init() via chooseDepthFormat(). The render pass
    // and framebuffers must use exactly this format; defaulted to eD32Sfloat so
    // nothing changes on current hardware (it is first in the preference list).
    vk::Format depth_format{ vk::Format::eD32Sfloat };
    vk::RenderPass renderPass;
    vk::Framebuffer framebuffer{};

    vk::Pipeline graphicsPipeline{};
    vk::PipelineLayout pipelineLayout{};
    DescriptorSetGroup lightMatricesDescriptors;
    // Owned by VulkanRenderer's sharedRenderDescriptors, NOT this class - bound as
    // set 0 so the alpha-test fragment stage reaches materials + textures. Never
    // destroyed here.
    vk::DescriptorSetLayout sharedRenderDescriptorSetLayout{};
    // One buffer per swapchain image, like globalUBOBuffer/sceneUBOBuffer -
    // otherwise the CPU rewrites the single buffer while an earlier frame's
    // shadow pass may still be reading it in flight.
    std::vector<VulkanBuffer> lightMatricesBuffers;

    std::vector<CascadeData> cascadeData;

    void createRenderPass();
    void createFramebuffers();
    void createDescriptorSetAndPipeline();
    void buildGraphicsPipeline();
};
}