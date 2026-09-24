module;

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.sampler_builder;

auto Kataglyphis::buildSamplerCreateInfo(vk::Filter filter,
  vk::SamplerAddressMode addressMode,
  float maxLod,
  vk::Bool32 anisotropyEnable,
  float maxAnisotropy,
  vk::BorderColor borderColor,
  vk::Bool32 compareEnable,
  vk::CompareOp compareOp) -> vk::SamplerCreateInfo
{
    // addressModeU == addressModeV == addressMode here, so the desc overload's
    // addressModeW = desc.addressModeU is byte-identical to this overload's
    // former addressModeW = addressMode.
    return buildSamplerCreateInfo(GltfSamplerDesc{ .addressModeU = addressMode,
                                     .addressModeV = addressMode,
                                     .magFilter = filter,
                                     .minFilter = filter,
                                     .mipmapMode = vk::SamplerMipmapMode::eLinear },
      maxLod,
      anisotropyEnable,
      maxAnisotropy,
      borderColor,
      compareEnable,
      compareOp);
}

auto Kataglyphis::buildSamplerCreateInfo(const GltfSamplerDesc &desc,
  float maxLod,
  vk::Bool32 anisotropyEnable,
  float maxAnisotropy,
  vk::BorderColor borderColor,
  vk::Bool32 compareEnable,
  vk::CompareOp compareOp) -> vk::SamplerCreateInfo
{
    vk::SamplerCreateInfo sampler_create_info{};
    sampler_create_info.magFilter = desc.magFilter;
    sampler_create_info.minFilter = desc.minFilter;
    sampler_create_info.addressModeU = desc.addressModeU;
    sampler_create_info.addressModeV = desc.addressModeV;
    sampler_create_info.addressModeW = desc.addressModeU;
    sampler_create_info.borderColor = borderColor;
    sampler_create_info.unnormalizedCoordinates = VK_FALSE;
    sampler_create_info.mipmapMode = desc.mipmapMode;
    sampler_create_info.mipLodBias = 0.0F;
    sampler_create_info.minLod = 0.0F;
    sampler_create_info.maxLod = maxLod;
    sampler_create_info.anisotropyEnable = anisotropyEnable;
    sampler_create_info.maxAnisotropy = maxAnisotropy;
    sampler_create_info.compareEnable = compareEnable;
    sampler_create_info.compareOp = compareOp;

    return sampler_create_info;
}

auto Kataglyphis::usesNearestFiltering(const GltfSamplerDesc &desc) -> bool
{
    return desc.magFilter == vk::Filter::eNearest || desc.minFilter == vk::Filter::eNearest
        || desc.mipmapMode == vk::SamplerMipmapMode::eNearest;
}

auto Kataglyphis::findSampler(std::span<const SamplerKey> createdSamplers, SamplerKey key)
  -> std::optional<std::size_t>
{
    // find_if, not find: MSVC STL 14.51 under clang-cl static_asserts on find over a struct with a defaulted ==
    // whose size is not 1/2/4/8 bytes (SamplerKey is 24) - microsoft/STL#6294.
    const auto it =
      std::ranges::find_if(createdSamplers, [&key](const SamplerKey &candidate) { return candidate == key; });
    if (it == createdSamplers.end()) { return std::nullopt; }
    return static_cast<std::size_t>(std::ranges::distance(createdSamplers.begin(), it));
}
