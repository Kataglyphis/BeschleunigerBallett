module;

#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.texture;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.image;
import kataglyphis.vulkan.image_view;

export namespace Kataglyphis {
class Texture
{
  public:
    Texture();
    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;
    Texture(Texture &&other) noexcept;
    Texture &operator=(Texture &&other) noexcept;

    bool createFromFile(const std::shared_ptr<VulkanDevice> &device,
      vk::CommandPool commandPool,
      const std::string &fileName,
      bool srgb = true);
    /// Decodes an encoded image (PNG/JPG/...) already in memory and uploads it,
    /// for glTF's embedded / data-URI images which never touch the filesystem.
    /// Shares the upload path with createFromFile; returns false if the bytes
    /// do not decode. `srgb` selects the colour space the data is uploaded in:
    /// true for colour data (base colour, emissive), false for linear data
    /// (normal maps) - see the note on uploadRgba's texture_format.
    bool createFromMemory(const std::shared_ptr<VulkanDevice> &device,
      vk::CommandPool commandPool,
      const unsigned char *encodedBytes,
      size_t byteCount,
      bool srgb = true);
    /// Returns false if the white-default upload itself failed (e.g. a failed
    /// submit), in which case the texture is left empty/unwritten.
    bool createDefaultTexture(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool commandPool);

    void setImage(vk::Image image);
    void setImageView(vk::ImageView imageView);

    uint32_t getMipLevel() const { return mip_levels; };
    VulkanImage &getVulkanImage() { return vulkanImage; };
    vk::Image &getImage() { return vulkanImage.getImage(); };
    vk::ImageView &getImageView() { return vulkanImageView.getImageView(); };
    vk::Sampler &getSampler() { return textureSampler; }

    void createImage(const std::shared_ptr<VulkanDevice> &device,
      uint32_t width,
      uint32_t height,
      uint32_t in_mip_levels,
      vk::Format format,
      vk::ImageTiling tiling,
      vk::ImageUsageFlags use_flags,
      vk::MemoryPropertyFlags prop_flags,
      uint32_t array_layers = 1,
      vk::ImageCreateFlags create_flags = {},
      vk::ImageType image_type = vk::ImageType::e2D,
      uint32_t depth = 1);

    void createImageView(const std::shared_ptr<VulkanDevice> &device,
      vk::Format format,
      vk::ImageAspectFlags aspect_flags,
      uint32_t in_mip_levels,
      vk::ImageViewType view_type = vk::ImageViewType::e2D,
      uint32_t array_layers = 1);

    void createTextureSampler(const std::shared_ptr<VulkanDevice> &device,
      vk::Filter filter = vk::Filter::eLinear,
      vk::SamplerAddressMode addressMode = vk::SamplerAddressMode::eRepeat,
      vk::Bool32 compareEnable = VK_FALSE,
      vk::CompareOp compareOp = vk::CompareOp::eAlways);

    /// Releases the current view only, leaving the sampler and image alone.
    /// Call before a createImage() that will replace the image the view looks
    /// at, so the view is destroyed before its image (VUID-vkDestroyImage-image-01000).
    void releaseImageView();

    /// Releases the current sampler only, leaving the view and image alone.
    /// Called first by createTextureSampler() so a second call on an
    /// already-created instance releases the previous sampler instead of
    /// leaking it.
    void releaseSampler();

    void cleanUp();

    // Decodes an image file to tightly packed RGBA8. Returns nullptr on
    // failure; on success `*image_size` is exactly width * height * 4 and the
    // caller owns the buffer (free with stbi_image_free).
    //
    // Public because it is a stateless utility that needs no Vulkan device,
    // and because it is the surface texture_loading_fuzz_test exercises: the
    // byte count it reports sizes a staging buffer that is then memcpy'd into,
    // so a mismatch between the count and the returned allocation is a heap
    // overflow rather than a bad picture.
    static unsigned char *
      loadTextureData(const std::string &file_name, int *width, int *height, vk::DeviceSize *image_size);

    ~Texture();

  private:
    // Shared tail of createFromFile / createFromMemory: takes tightly packed
    // RGBA8 and does the staging-buffer copy, image creation, layout
    // transitions, mip generation and view creation. Returns false on an empty
    // image.
    bool uploadRgba(const std::shared_ptr<VulkanDevice> &device,
      vk::CommandPool commandPool,
      int width,
      int height,
      vk::DeviceSize size,
      const unsigned char *rgba,
      bool srgb = true);

    uint32_t mip_levels = 0;

    void generateMipMaps(vk::CommandBuffer command_buffer, vk::Image image, int32_t width, int32_t height);

    VulkanImage vulkanImage;
    VulkanImageView vulkanImageView;
    vk::Sampler textureSampler = nullptr;
    std::shared_ptr<VulkanDevice> device = nullptr;
};
}// namespace Kataglyphis
