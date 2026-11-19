module;

#include <cstdint>
#include <optional>
#include <span>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.sampler_builder;

export namespace Kataglyphis {

// Convenience wrapper over the GltfSamplerDesc overload below for callers
// that only need one filter/address mode shared across mag, min and U/V/W.
// Returns the struct only - it does not create the sampler, since the three
// call sites use three different createSampler overloads and error
// conventions.
auto buildSamplerCreateInfo(vk::Filter filter,
  vk::SamplerAddressMode addressMode,
  float maxLod,
  vk::Bool32 anisotropyEnable,
  float maxAnisotropy,
  vk::BorderColor borderColor,
  vk::Bool32 compareEnable = VK_FALSE,
  vk::CompareOp compareOp = vk::CompareOp::eNever) -> vk::SamplerCreateInfo;

/// A glTF sampler's wrap/filter settings, translated into Vulkan enums. One
/// instance per glTF `sampler` object referenced by a texture (or the default
/// below when a texture names none / the document is not glTF at all).
/// Defaults match this engine's pre-sampler-support behaviour exactly -
/// repeat + linear + linear - so an untextured or OBJ-sourced texture keeps
/// producing a byte-identical vk::SamplerCreateInfo.
struct GltfSamplerDesc
{
    vk::SamplerAddressMode addressModeU{ vk::SamplerAddressMode::eRepeat };
    vk::SamplerAddressMode addressModeV{ vk::SamplerAddressMode::eRepeat };
    vk::Filter magFilter{ vk::Filter::eLinear };
    vk::Filter minFilter{ vk::Filter::eLinear };
    vk::SamplerMipmapMode mipmapMode{ vk::SamplerMipmapMode::eLinear };
    /// False for glTF's bare NEAREST and LINEAR minification, which read level 0 only: the sampler then clamps its
    /// LOD to [0, 0.25] with a nearest mip, whatever maxLod it is given.
    bool mipmaps{ true };

    bool operator==(const GltfSamplerDesc &) const = default;
};

// The primitive form, sourced from a glTF sampler description: mag/min
// filter and mipmap mode vary independently instead of sharing one
// `filter`, and U/V wrap modes can differ (W is not a glTF concept for a 2D
// texture, so it follows U). The overload above delegates here.
auto buildSamplerCreateInfo(const GltfSamplerDesc &desc,
  float maxLod,
  vk::Bool32 anisotropyEnable,
  float maxAnisotropy,
  vk::BorderColor borderColor,
  vk::Bool32 compareEnable = VK_FALSE,
  vk::CompareOp compareOp = vk::CompareOp::eNever) -> vk::SamplerCreateInfo;

// True when `desc` asked for nearest filtering anywhere (mag, min or mip).
// Anisotropic filtering is meaningless combined with nearest sampling, and
// nearest-filtered assets are exactly the ones whose blocky look the author
// chose deliberately - so anisotropy must not silently apply anyway.
auto usesNearestFiltering(const GltfSamplerDesc &desc) -> bool;

// Anisotropy only for a linear, mipmapped sampler, as Rust's anisotropy_for: a mipmap-less one is plain level 0.
auto anisotropyAllowed(const GltfSamplerDesc &desc) -> bool;

// Key a created sampler is reusable under: mip level AND glTF sampler
// description must both match, since two textures with the same mip count
// but different wrap/filter settings need genuinely different samplers.
struct SamplerKey
{
    uint32_t mipLevel;
    GltfSamplerDesc desc;

    bool operator==(const SamplerKey &) const = default;
};

// Looks up an already-created sampler by SamplerKey so callers with several
// textures that only differ by neither mip level nor sampler description can
// reuse one vk::Sampler instead of allocating a duplicate per texture. Pure
// and device-free, so it is unit-testable without a Vulkan instance.
auto findSampler(std::span<const SamplerKey> createdSamplers, SamplerKey key) -> std::optional<std::size_t>;

}// namespace Kataglyphis
