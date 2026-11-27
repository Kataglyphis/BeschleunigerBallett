module;
#include "renderer/PathTracingHistory.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
import kataglyphis.vulkan.instance;
import kataglyphis.vulkan.path_tracing;
import kataglyphis.vulkan.post_stage;
import kataglyphis.vulkan.exposure_stage;
import kataglyphis.vulkan.bloom_stage;
import kataglyphis.vulkan.ssao_stage;
import kataglyphis.vulkan.fxaa_stage;
import kataglyphis.vulkan.rasterizer;
import kataglyphis.vulkan.render_settings;
import kataglyphis.vulkan.render_stage_registry;
import kataglyphis.vulkan.deferred_rasterizer;
import kataglyphis.vulkan.raytracing;
import kataglyphis.vulkan.cascaded_shadow_map;
import kataglyphis.vulkan.punctual_shadow_map;
import kataglyphis.vulkan.sky_box;
import kataglyphis.vulkan.ambient_maps;
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
    // The settings and feedback are the app's; the renderer keeps neither reference.
    VulkanRenderer(Kataglyphis::Frontend::Window *window,
      Scene *scene,
      Camera *camera,
      const RenderSettings &settings,
      RendererFeedback &feedback);

    void drawFrame(const RenderSettings &settings, RendererFeedback &feedback);

    void updateUniforms(Scene *scene_data, Camera *camera_data, const RenderSettings &settings);

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

    // Acts on the settings' one-shot requests; the caller spends them with applyRendererFeedback afterwards.
    void updateStateDueToUserInput(const RenderSettings &settings, RendererFeedback &feedback);
    bool hasDeviceLost() const { return device_lost_detected; }
    // No skybox faces loaded, so the sky is OxidANT's procedural gradient baked into the cubemap.
    bool skyIsAnalytic() const { return skyBox.usesAnalyticSky(); }
    // True once any frame has hit a fatal (non-recoverable) error - a failed
    // wait/submit or an invalid sync handle - that forced the window closed.
    // Distinct from hasDeviceLost(): a failed submit can leave a live device
    // that App::run() still tears down normally, but the run itself must
    // still be reported as a failure.
    [[nodiscard]] bool hasFatalFrameError() const { return fatal_frame_error; }
    // Test seam, called by no production code: every later fence wait reports eErrorDeviceLost, as after a real loss.
    void simulateDeviceLossForTesting() { device_loss_simulated = true; }
    // Test seam, called by no production code: fresh ambient maps whose bake bails, as after a failed skybox upload.
    void simulateFailedAmbientBakeForTesting();
    // Observes the device without extending its life, so a test can see when its last owner lets go.
    [[nodiscard]] std::weak_ptr<VulkanDevice> deviceLifetime() const { return device; }
    bool supportsHardwareRaytracing() const;
    // What the editor's ImGui Vulkan backend is created against.
    [[nodiscard]] std::shared_ptr<VulkanDevice> sharedDevice() const { return device; }
    [[nodiscard]] vk::Instance vulkanInstance() const { return instance->getVulkanInstance(); }
    [[nodiscard]] vk::Format swapChainFormat() const { return vulkanSwapChain.getSwapChainFormat(); }
    [[nodiscard]] uint32_t swapChainImageCount() const { return vulkanSwapChain.getNumberSwapChainImages(); }
    [[nodiscard]] vk::PresentModeKHR presentMode() const { return vulkanSwapChain.getPresentMode(); }
    [[nodiscard]] std::vector<vk::PresentModeKHR> offeredPresentModes() const;
    // Test instrumentation: an addModel must build one BLAS and keep the others' handles.
    [[nodiscard]] std::uint64_t bottomLevelBuildCount() const { return asManager.getBlasBuildCount(); }
    [[nodiscard]] std::vector<vk::AccelerationStructureKHR> bottomLevelHandles() const
    { return asManager.getBlasHandles(); }

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
    // The same arm-then-take pair for the HDR target post reads: raw RGBA16F texels, 8 bytes each.
    void requestHdrCapture();
    std::vector<uint8_t> takeCapturedHdrFrame(uint32_t &outWidth, uint32_t &outHeight);
    // The same pair for the exposure state post read, [adapted EV, target EV]; empty when nothing was captured.
    void requestExposureCapture() { exposureStage.requestCapture(); }
    [[nodiscard]] std::optional<std::array<float, 2>> takeCapturedExposure() { return exposureStage.takeCapture(); }
    // The same pair for the half-resolution bloom post adds: raw RGBA16F texels, 8 bytes each.
    void requestBloomCapture() { bloomCapture.request(); }
    std::vector<uint8_t> takeCapturedBloomFrame(uint32_t &outWidth, uint32_t &outHeight);

    void recreateSwapChain();
    void reprovisionPerImageResources();

    // A pass added here records, resizes and hot-reloads with the built-in ones, without editing this class.
    RenderStageRegistry::AddResult addRenderStage(RenderStage stage) { return renderStages.add(std::move(stage)); }
    // Drains the GPU first, since the stage's closures may own resources a frame in flight still reads.
    bool removeRenderStage(std::string_view name);
    [[nodiscard]] const RenderStageRegistry &renderStageRegistry() const { return renderStages; }
    // Benchmark seam, called by no production code: records image 0's commands and never submits them.
    bool recordCommandsUnsubmittedForBenchmark();

    VulkanRenderer(const VulkanRenderer &) = delete;
    VulkanRenderer &operator=(const VulkanRenderer &) = delete;
    VulkanRenderer(VulkanRenderer &&) = delete;
    VulkanRenderer &operator=(VulkanRenderer &&) = delete;
    // Drains the GPU; the members then release in reverse declaration order.
    ~VulkanRenderer();

  private:
    void shaderHotReload();

    // -- decomposed updateStateDueToUserInput handlers
    void handleShaderHotReloadRequest(
      const Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &rendererSettings);
    void handleRasterizationModeChange(
      const Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &rendererSettings);
    void handleVsyncChange(
      const Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &rendererSettings);
    void handleShadowResolutionChange(const GUISceneSharedVars &sceneSettings);
    // Tears down and re-creates dirShadowMap against the resolution/cascade
    // count frameSettings holds and the swapchain's current image count.
    // Shared by handleShadowResolutionChange (GUI-driven resolution change)
    // and reprovisionPerImageResources (swapchain image count change) - both
    // need the exact same cleanUp()+init()+createGraphicsPipeline() sequence,
    // just triggered by a different condition.
    void reinitShadowMapForCurrentSettings();
    // Re-creates the punctual cubes against the swapchain's image count and the shared layout, keeping the slots.
    void reinitPunctualShadows();
    // Gives the packed lights their cube slots and the cubes their lights; drains the GPU, as it may reallocate.
    void applyPunctualShadowSlots(std::vector<glm::vec4> &packedLights);
    void handleModelTransformChange(const GUISceneSharedVars &sceneSettings);
    void handleModelReloadRequest(const GUISceneSharedVars &sceneSettings);
    // Reports what the scene now holds; applyRendererFeedback moves the editor's selection and inspector to it.
    void publishLoadedModel();

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

    // Shared with the device, which owns the surface, so both live while the scene or GUI holds the device.
    std::shared_ptr<VulkanInstance> instance;
    [[nodiscard]] vk::SurfaceKHR create_surface();

    // Every Vulkan-owning member below releases before this reference, so it never outlives the device.
    std::shared_ptr<VulkanDevice> device;

    VulkanSwapChain vulkanSwapChain;

    Kataglyphis::Frontend::Window *window;
    Scene *scene;
    Camera *camera;
    // Whether the last load moved the camera, so a scene that keeps the fixed pose after it gets that pose back.
    bool lastLoadFramed = false;

    // A copy of the settings the last per-frame call passed, for the paths that run outside one (addModel, resizes).
    RenderSettings frameSettings;
    // Written as the frame goes and copied out to the caller's feedback at the end of each per-frame call.
    RendererFeedback output;
    void drawFrameImpl();

    // -- pools
    bool record_commands(uint32_t image_index);

    // The built-in passes, in RenderStageOrder; each hook below is one stage's record step.
    void registerBuiltInStages();
    void recordShadowStage(const RenderStageFrame &frame);
    void recordPunctualShadowStage(const RenderStageFrame &frame);
    void recordMainStage(const RenderStageFrame &frame);
    void recordCloudsStage(const RenderStageFrame &frame);
    void recordCloudUpsampleStage(const RenderStageFrame &frame);
    void recordPostStage(const RenderStageFrame &frame);
    // The one rule post and the fxaa stage decide this frame's target by.
    [[nodiscard]] bool fxaaFiltersFrame(const RenderStageFrame &frame) const;

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
    // the raster and shadow passes) and recordRaytracingOrPathTracing (to gate
    // the RT/PT dispatch), so the two can never disagree. The TLAS-null term is
    // load-bearing: during the async model-load window RT/PT is requested but
    // cannot dispatch yet, so the raster pass must still run.
    [[nodiscard]] bool raytracingOwnsFrame(uint32_t image_index);
    // raytracingOwnsFrame's mode, per drawnRenderMode(): 0 raster, 1 RT, 2 PT.
    [[nodiscard]] int drawnRenderMode() const;

    // Records the ray-tracing or path-tracing pass (whichever the GUI selected)
    // when hardware RT is available and the TLAS is built, including the
    // path-tracing accumulation-reset bookkeeping. Extracted verbatim from
    // record_commands.
    void recordRaytracingOrPathTracing(vk::CommandBuffer &commandBuffer, uint32_t image_index, bool clouds_enabled);
    void create_command_pool();
    // Destroyed after every member below it, all of which allocate or record from it.
    vk::UniqueCommandPool graphics_command_pool;

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
    // RT/PT's primary-hit NDC depth, which the clouds read in place of a raster depth buffer.
    Texture tracedDepth;
    void createTracedDepthResources();
    Kataglyphis::VulkanRendererInternals::PostStage postStage;
    Kataglyphis::VulkanRendererInternals::FxaaStage fxaaStage;
    // The histogram and the exposure buffer post reads; declared before postDescriptors, which bind that buffer.
    Kataglyphis::VulkanRendererInternals::ExposureStage exposureStage;
    // The settings' clock at the last reduce, whose difference is the adaptation's frame time.
    double lastExposureSeconds{ 0.0 };
    [[nodiscard]] Kataglyphis::VulkanRendererInternals::ExposureFrameParams exposureFrameParams();
    // bloom.slang's passes; declared before postDescriptors, which bind its output.
    Kataglyphis::VulkanRendererInternals::BloomStage bloomStage;
    // ssao.slang's passes over the raster paths' depth; declared before the shared sets, which bind its occlusion.
    Kataglyphis::VulkanRendererInternals::SsaoStage ssaoStage;
    // Points SSAO at the forward prepass's and the G-buffer's depth, after either was (re)created.
    void bindSsaoDepths();

    // Drawn inside the main stage, into the HDR target post reads.
    Kataglyphis::SkyBox skyBox;
    // Baked from skyBox's cube after its init, and again on a shader reload.
    Kataglyphis::AmbientMaps ambientMaps;
    Kataglyphis::Clouds clouds;
    // The depth image this frame's main pass writes; the clouds' descriptor is rewritten only when it changes.
    enum class CloudDepthSource : std::uint8_t { None, ForwardRaster, DeferredRaster, Traced };
    CloudDepthSource boundCloudDepthSource{ CloudDepthSource::None };
    [[nodiscard]] CloudDepthSource cloudDepthSourceForThisFrame() const;
    void bindCloudSceneDepth();
    void handleCloudDepthSourceChange();
    Kataglyphis::CascadedShadowMap dirShadowMap;
    // Set at the end of reinitShadowMapForCurrentSettings(): a re-init leaves
    // every swapchain image but the next one drawFrame() acquires holding
    // stale (or default-constructed) light matrices, because dirShadowMap's
    // own seed loop runs before updateUniforms() has recomputed cascadeData
    // for the new settings. Consumed (and cleared) by update_uniform_buffers(),
    // the first point in the frame where cascadeData is guaranteed fresh.
    bool lightMatricesNeedFullReseed{ false };
    // Cube shadow maps for the first point and spot lights; the slots survive a re-init.
    Kataglyphis::PunctualShadowMaps punctualShadows;
    std::vector<Kataglyphis::PunctualShadowSlot> punctualShadowSlots;

    // -- synchronization
    // All frame-sync primitives (current_frame, frame_sync_count, the per-frame
    // image_available/in_flight_fences and the per-swapchain-image
    // render_finished_by_image/images_in_flight_fences) live in FrameSync; see
    // kataglyphis.vulkan.frame_sync for why the sizing split is load-bearing.
    FrameSync frameSync;
    void createSynchronization();

    // -- per-pass GPU timing (timestamp query pool); see
    // kataglyphis.vulkan.gpu_timing for the layout invariants (the per-image
    // slice sizing and the "never read an unrecorded slice" guard are
    // load-bearing).
    GpuTimingSubsystem gpuTiming;

    // -- headless frame capture (see requestFrameCapture above); see
    // kataglyphis.vulkan.frame_capture for the arm/record/bind/take lifecycle.
    FrameCapture frameCapture;
    FrameCapture hdrCapture;
    FrameCapture bloomCapture;

    // Released before the device, after the destructor drained the GPU, so a stage's captured resources go first.
    RenderStageRegistry renderStages;

    Kataglyphis::VulkanRendererInternals::ASManager asManager;
    VulkanBuffer objectDescriptionBuffer;
    // The scene's packed punctual lights; rebuilt with the object descriptions on every scene change.
    VulkanBuffer punctualLightBuffer;
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
    // Every change those sets depend on (TLAS, raster mode, extent, image count) ends in updateAllDescriptorSets.
    void updateRaytracingDescriptorSets();
    void writeRaytracingDescriptorsForImage(uint32_t image_index);

    void finishModelLoad();

    // The single entry point every scene-changing path (finishModelLoad,
    // addModel, handleModelTransformChange, handleModelReloadRequest) must go
    // through: rebuilds the object-description buffer, rebuilds the
    // acceleration structure (the new models' BLAS plus the TLAS via
    // createASForScene when geometry changed, TLAS-only via createTLAS for a
    // transform-only change), then
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
    bool device_loss_simulated{ false };
};
}// namespace Kataglyphis
