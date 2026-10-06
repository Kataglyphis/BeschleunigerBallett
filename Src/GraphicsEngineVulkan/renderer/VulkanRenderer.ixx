module;
#include "renderer/PathTracingHistory.hpp"
#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.renderer;

import kataglyphis.vulkan.as_manager;
import kataglyphis.vulkan.buffer_manager;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.descriptor_set_group;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.frame_capture;
import kataglyphis.vulkan.frame_sync;
import kataglyphis.vulkan.gpu_timing;
import kataglyphis.vulkan.global_ubo;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.gui_renderer_shared_vars;
import kataglyphis.vulkan.gui_scene_shared_vars;
import kataglyphis.vulkan.instance;
import kataglyphis.vulkan.path_tracing;
import kataglyphis.vulkan.post_stage;
import kataglyphis.vulkan.rasterizer;
import kataglyphis.vulkan.deferred_rasterizer;
import kataglyphis.vulkan.raytracing;
import kataglyphis.vulkan.cascaded_shadow_map;
import kataglyphis.vulkan.sky_box;
import kataglyphis.vulkan.clouds;
import kataglyphis.vulkan.scene_ubo;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.window;

export namespace Kataglyphis {
class VulkanRenderer
{
  public:
    VulkanRenderer(Kataglyphis::Frontend::Window *window,
      Scene *scene,
      Kataglyphis::Frontend::GUI *gui,
      Camera *camera);

    void drawFrame(const GUISceneSharedVars &guiSceneSharedVars);

    void updateUniforms(Scene *scene_data, Camera *camera_data, const GUISceneSharedVars &guiSceneSharedVars);

    /// Adds a model to the current scene without replacing what is already
    /// there, and refreshes the descriptor sets that reference scene
    /// resources. Returns the new model's index - the value the raster paths
    /// push as objectIndex - or std::nullopt if loading failed.
    ///
    /// Lives here rather than on Scene alone because the caller would
    /// otherwise need the renderer's device and command pool, and because
    /// forgetting the descriptor refresh leaves the new model sampling
    /// whatever the previous scene had bound.
    std::optional<uint32_t> addModel(const std::string &modelPath, const glm::mat4 &modelMatrix);

    /// True while the startup model is still parsing on its worker. Tests and
    /// tools that need the scene populated must render frames until this
    /// clears - the renderer draws the sky meanwhile rather than blocking.
    [[nodiscard]] bool isModelLoadPending() const { return scene != nullptr && scene->isModelLoadPending(); }

    // Reads/mutates the caller-owned scene settings directly (no GUI pointer)
    // so this can be driven by anything that produces a GUISceneSharedVars,
    // not only a live GUI object. GUIRendererSharedVars (renderer-only state:
    // rasterization mode, hot-reload trigger, ...) is unrelated to scene
    // settings and still comes from the `gui` member.
    void updateStateDueToUserInput(GUISceneSharedVars &guiSceneSharedVars);
    void finishAllRenderCommands();
    bool hasDeviceLost() const { return device_lost_detected; }
    // True once any frame has hit a fatal (non-recoverable) error - a failed
    // wait/submit or an invalid sync handle - that forced the window closed.
    // Distinct from hasDeviceLost(): a failed submit can leave a live device
    // that App::run() still tears down normally, but the run itself must
    // still be reported as a failure.
    [[nodiscard]] bool hasFatalFrameError() const { return fatal_frame_error; }
    bool supportsHardwareRaytracing() const;

    // -- headless frame capture (test / tooling instrumentation)
    //
    // The capture is recorded *inside* the frame's own command buffer, right
    // after the post stage has transitioned the swapchain image to
    // ePresentSrcKHR and before the present. Copying the image after
    // vkQueuePresentKHR would mean touching an image owned by the presentation
    // engine, so instead the copy is armed one frame ahead:
    //
    //     renderer->requestFrameCapture();
    //     ... one normal frame (updateUniforms + drawFrame) ...
    //     auto pixels = renderer->takeCapturedFrame(width, height);
    //
    // takeCapturedFrame() waits on the fence of the frame that recorded the
    // copy, so it never calls waitIdle and never blocks the frame path.
    // Returns tightly packed RGBA8 (swizzled from BGRA when the swapchain uses
    // a B8G8R8A8 format), or an empty vector when nothing was captured.
    bool supportsFrameCapture() const;
    void requestFrameCapture();
    std::vector<uint8_t> takeCapturedFrame(uint32_t &outWidth, uint32_t &outHeight);

    void cleanUp();

    void recreateSwapChain();
    void reprovisionPerImageResources();

    ~VulkanRenderer();

  private:
    void shaderHotReload();

    // -- decomposed updateStateDueToUserInput handlers
    void handleShaderHotReloadRequest(
      Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &guiRendererSharedVars);
    void handleRasterizationModeChange(
      Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &guiRendererSharedVars);
    void handleShadowResolutionChange(GUISceneSharedVars &guiSceneSharedVars);
    // Tears down and re-creates dirShadowMap against the resolution/cascade
    // count the GUI currently holds and the swapchain's current image count.
    // Shared by handleShadowResolutionChange (GUI-driven resolution change)
    // and reprovisionPerImageResources (swapchain image count change) - both
    // need the exact same cleanUp()+init()+createGraphicsPipeline() sequence,
    // just triggered by a different condition.
    void reinitShadowMapForCurrentSettings();
    void handleModelTransformChange(GUISceneSharedVars &guiSceneSharedVars);
    void handleModelReloadRequest(GUISceneSharedVars &guiSceneSharedVars);

    // The rasterization mode whose offscreen texture the post (and RT) input
    // descriptors currently point at. The mode branch in record_commands is
    // consulted every frame, but the DESCRIPTORS were written once at init -
    // so switching to Deferred recorded a deferred frame into a texture the
    // post pass never sampled, and the presented image silently stayed
    // forward. Tracked so updateStateDueToUserInput can rebind on change.
    Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode lastBoundRasterizationMode{
        Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode::Forward
    };

    // helper class for managing our buffers
    VulkanBufferManager vulkanBufferManager;

    // Vulkan instance, stores all per-application states
    VulkanInstance instance;

    // surface defined on windows as WIN32 window system, Linux f.e. X11, MacOS
    // also their own
    vk::SurfaceKHR surface{};
    void create_surface();

    std::shared_ptr<VulkanDevice> device;

    VulkanSwapChain vulkanSwapChain;

    Kataglyphis::Frontend::Window *window;
    Scene *scene;
    Kataglyphis::Frontend::GUI *gui;
    Camera *camera;

    // -- pools
    bool record_commands(uint32_t image_index, const GUISceneSharedVars &guiSceneSharedVars);

    // Returns the offscreen colour texture of whichever raster path is active
    // this frame (forward vs deferred), reading the mode once. The RT/PT and
    // post input descriptors, and the RT/PT passes, all target this texture.
    Texture &activeOffscreenTexture(uint32_t index);

    // Records the active raster path (forward Rasterizer or deferred
    // DeferredRasterizer) for this frame and publishes its visibility stats.
    // Extracted verbatim from record_commands; the frustum is passed in so its
    // deliberate post-shadow-pass ordering stays visible at the call site.
    void recordRasterPass(vk::CommandBuffer &commandBuffer,
      uint32_t image_index,
      std::span<const vk::DescriptorSet> rasterizer_descriptor_sets,
      const std::optional<FrustumPlanes> &camera_frustum);

    // The single predicate deciding whether recordRaytracingOrPathTracing will
    // actually dispatch this frame. Called from both record_commands (to skip
    // the raster pass) and recordRaytracingOrPathTracing (to gate the RT/PT
    // dispatch), so the two can never disagree. The TLAS-null term is
    // load-bearing: during the async model-load window RT/PT is requested but
    // cannot dispatch yet, so the raster pass must still run.
    [[nodiscard]] bool raytracingOwnsFrame(uint32_t image_index);

    // Records the ray-tracing or path-tracing pass (whichever the GUI selected)
    // when hardware RT is available and the TLAS is built, including the
    // path-tracing accumulation-reset bookkeeping. Extracted verbatim from
    // record_commands.
    void recordRaytracingOrPathTracing(vk::CommandBuffer &commandBuffer, uint32_t image_index);
    void create_command_pool();
    void cleanUpCommandPools();
    vk::CommandPool graphics_command_pool{};

    // uniform buffers
    VulkanRendererInternals::GlobalUBO globalUBO{};
    std::vector<VulkanBuffer> globalUBOBuffer;
    std::vector<void *> globalUBOMapped;
    VulkanRendererInternals::SceneUBO sceneUBO{};
    std::vector<VulkanBuffer> sceneUBOBuffer;
    std::vector<void *> sceneUBOMapped;
    void create_uniform_buffers();
    // Returns false (and leaves the UBOs untouched) if image_index is out of
    // range for the currently sized UBO vectors.
    bool update_uniform_buffers(uint32_t image_index);
    void cleanUpUBOs();

    std::vector<vk::CommandBuffer> command_buffers;
    void create_command_buffers();

    Kataglyphis::VulkanRendererInternals::Raytracing raytracingStage;
    Kataglyphis::VulkanRendererInternals::Rasterizer rasterizer;
    Kataglyphis::VulkanRendererInternals::DeferredRasterizer deferredRasterizer;
    Kataglyphis::VulkanRendererInternals::PathTracing pathTracing;
    // Path-tracing temporal accumulation: ONE persistent full-float history
    // image (deliberately not per swapchain image - it must survive across
    // frames), the number of frames accumulated since the last reset, and the
    // key (camera view, light, quality) that history was accumulated against
    // - any change to it invalidates the history.
    Texture pathTracingAccumulation;
    uint32_t pathTracingAccumulatedFrames{ 0 };
    Kataglyphis::VulkanRendererInternals::PathTracingHistoryKey pathTracingLastHistory{};
    void createPathTracingAccumulationResources();
    Kataglyphis::VulkanRendererInternals::PostStage postStage;

    // Atmospheric & Lighting Additions
    Kataglyphis::SkyBox skyBox;
    Kataglyphis::Clouds clouds;
    Kataglyphis::CascadedShadowMap dirShadowMap;
    // Set at the end of reinitShadowMapForCurrentSettings(): a re-init leaves
    // every swapchain image but the next one drawFrame() acquires holding
    // stale (or default-constructed) light matrices, because dirShadowMap's
    // own seed loop runs before updateUniforms() has recomputed cascadeData
    // for the new settings. Consumed (and cleared) by update_uniform_buffers(),
    // the first point in the frame where cascadeData is guaranteed fresh.
    bool lightMatricesNeedFullReseed{ false };
    // One image view per swapchain image, for the skybox framebuffers - the
    // depth attachment is a single loop-invariant handle (PostStage owns just
    // one depth buffer) and is passed to SkyBox separately, not through this.
    std::vector<vk::ImageView> swapchainImageViews();

    // -- synchronization
    // All frame-sync primitives (current_frame, frame_sync_count, the per-frame
    // image_available/in_flight_fences and the per-swapchain-image
    // render_finished_by_image/images_in_flight_fences) live in FrameSync; see
    // kataglyphis.vulkan.frame_sync for why the sizing split is load-bearing.
    FrameSync frameSync;
    void createSynchronization();
    void cleanUpSync();

    // -- per-pass GPU timing (timestamp query pool); see
    // kataglyphis.vulkan.gpu_timing for the layout invariants (the per-image
    // slice sizing and the "never read an unrecorded slice" guard are
    // load-bearing).
    GpuTimingSubsystem gpuTiming;

    // -- headless frame capture (see requestFrameCapture above); see
    // kataglyphis.vulkan.frame_capture for the arm/record/bind/take lifecycle.
    FrameCapture frameCapture;

    Kataglyphis::VulkanRendererInternals::ASManager asManager;
    VulkanBuffer objectDescriptionBuffer;
    void create_object_description_buffer();
    // Discards and re-uploads the object-description buffer. The scene-changed
    // paths (finishModelLoad / addModel / updateStateDueToUserInput) all do this
    // identical pair before rebuilding acceleration structures and descriptors;
    // the surrounding AS/descriptor steps differ per path and stay inline.
    void rebuildObjectDescriptions();
    void updateObjectDescriptionDescriptorSets();

    // -- descriptor set groups (each owns one layout + pool + per-swapchain-
    // image sets; see kataglyphis.vulkan.descriptor_set_group)
    DescriptorSetGroup sharedRenderDescriptors;
    void createSharedRenderDescriptorResources();
    void updateTexturesInSharedRenderDescriptorSet();
    void updateUBODescriptorSets();

    DescriptorSetGroup postDescriptors;
    void create_post_descriptor_resources();
    void updatePostDescriptorSets();

    DescriptorSetGroup gbufferDescriptors;
    void create_gbuffer_descriptor_resources();
    void updateGBufferDescriptorSets();

    DescriptorSetGroup raytracingDescriptors;
    void createRaytracingDescriptorResources();
    void update_raytracing_descriptor_set(uint32_t image_index);
    void updateRaytracingDescriptorSets();

    // The three raytracingDescriptors.write* calls (TLAS, output image,
    // accumulation image) shared verbatim by update_raytracing_descriptor_set
    // (per-image) and updateRaytracingDescriptorSets (all images).
    void writeRaytracingDescriptorsForImage(uint32_t image_index);

    void finishModelLoad();

    // The single entry point every scene-changing path (finishModelLoad,
    // addModel, handleModelTransformChange, handleModelReloadRequest) must go
    // through: rebuilds the object-description buffer, rebuilds the
    // acceleration structure (BLAS+TLAS via createASForScene when geometry
    // changed, TLAS-only via createTLAS for a transform-only change), then
    // refreshes every descriptor set that could be bound to stale data. A
    // caller that rebuilds the AS without also going through this helper is
    // exactly the bug this consolidates away - see BuildIntegrity's
    // AccelerationStructureRebuildsGoThroughTheSceneChangeHelper gate.
    void refreshAfterSceneChange(bool rebuildBottomLevel);

    void updateAllDescriptorSets();
    void cleanUpDescriptorResources();
    void initDescriptorResources();

    bool checkChangedFramebufferSize();
    bool device_lost_detected{ false };
    bool fatal_frame_error{ false };
};
}// namespace Kataglyphis
