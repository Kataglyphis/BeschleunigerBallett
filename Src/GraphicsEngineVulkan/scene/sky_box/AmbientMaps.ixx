module;

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.ambient_maps;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.descriptor_set_group;

export namespace Kataglyphis {

// The Rust renderer's sizes (OxidANT render/ibl.rs), so both renderers light a scene from the same bake.
inline constexpr uint32_t kAmbientEnvironmentSize = 128;
inline constexpr uint32_t kAmbientEnvironmentMips = 5;
inline constexpr uint32_t kAmbientIrradianceSize = 32;
inline constexpr uint32_t kAmbientPrefilterSize = 128;
// Roughness levels in the prefiltered cube: roughness = mip / (mips - 1).
inline constexpr uint32_t kAmbientPrefilterMips = 5;
inline constexpr uint32_t kAmbientPrefilterSamples = 256;
inline constexpr uint32_t kAmbientBrdfLutSize = 256;

// Every draw one bake records: the table, the environment's faces and mips, the irradiance and the prefilter.
inline constexpr uint32_t kAmbientBakeDraws = 1 + 6 * kAmbientEnvironmentMips + 6 + 6 * kAmbientPrefilterMips;

// OxidANT's ibl.slang IblParams.
struct AmbientBakeParams
{
    std::array<float, 4> faceRoughnessSamplesMip{};
    std::array<float, 4> sourceResolution{};
};

// The split-sum maps baked from the drawn skybox's cube, which the raster and ray-traced modes light the scene with.
class AmbientMaps
{
  public:
    AmbientMaps();

    AmbientMaps(const AmbientMaps &) = delete;
    AmbientMaps &operator=(const AmbientMaps &) = delete;

    // Creates the maps, zeroed and sampleable, and the bake pipelines; only bake() fills them.
    void init(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool commandPool);

    // Bakes the table and every cube from skybox, whose faces are faceSize texels; false leaves baked() false.
    bool bake(Texture &skybox, uint32_t faceSize);

    // F5: new pipelines from the recompiled ibl.slang, then a bake with them.
    void shaderHotReload(Texture &skybox, uint32_t faceSize);

    [[nodiscard]] bool baked() const { return bakedOnce; }
    // The maps exist and are sampleable, baked or not, so set 0 can name them before the first bake.
    [[nodiscard]] bool ready() const { return mapsCreated; }
    [[nodiscard]] float prefilteredTopMip() const { return static_cast<float>(kAmbientPrefilterMips - 1); }

    Texture &getIrradiance() { return irradiance; }
    Texture &getPrefiltered() { return prefiltered; }
    Texture &getBrdfLut() { return brdfLut; }

    void cleanUp();

    ~AmbientMaps();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };
    vk::CommandPool commandPool{};

    Texture environment;
    Texture irradiance;
    Texture prefiltered;
    Texture brdfLut;
    vk::Sampler bakeSampler{};

    VulkanBuffer params;
    vk::DeviceSize paramsStride{ 0 };
    DescriptorSetGroup descriptors;
    vk::PipelineLayout pipelineLayout{};
    vk::Pipeline skyboxToCube{};
    vk::Pipeline downsample{};
    vk::Pipeline irradianceFilter{};
    vk::Pipeline prefilter{};
    vk::Pipeline brdfTable{};

    bool mapsCreated{ false };
    bool bakedOnce{ false };

    void createMaps();
    // Moves every map out of UNDEFINED, so a bake that never runs or bails leaves zeros the shaders may bind.
    bool clearMaps();
    void createPipelines();
    // Every pipeline but the table's, whose teardown takes the shared layout with it.
    void destroyFilterPipelines();
};

}// namespace Kataglyphis
