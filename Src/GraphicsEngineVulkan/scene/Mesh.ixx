module;
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.mesh;

import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.object_description;
import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.buffer_manager;
import kataglyphis.vulkan.frustum;

export namespace Kataglyphis {
// this a simple Mesh without mesh generation
class Mesh
{
  public:
    Mesh(const std::shared_ptr<VulkanDevice> &device,
      vk::CommandPool transfer_command_pool,
      const std::vector<Vertex> &vertices,
      const std::vector<uint32_t> &indices,
      const std::vector<unsigned int> &materialIndex,
      const std::vector<ObjMaterial> &materials);

    Mesh();
    Mesh(const Mesh &) = delete;
    Mesh &operator=(const Mesh &) = delete;
    Mesh(Mesh &&other) noexcept = default;
    Mesh &operator=(Mesh &&other) noexcept = default;

    void cleanUp();

    ObjectDescription &getObjectDescription() { return object_description; };
    uint32_t getVertexCount() const { return vertex_count; };
    uint32_t getIndexCount() const { return index_count; };
    vk::Buffer &getVertexBuffer() { return vertexBuffer.getBuffer(); };
    vk::Buffer &getIndexBuffer() { return indexBuffer.getBuffer(); };

    /// Object-space bounds, computed from the vertex positions at
    /// construction. Left invalid (min > max) for an empty mesh, which
    /// Kataglyphis::isVisible deliberately treats as "always visible" rather
    /// than culling something whose size is unknown.
    const AABB &getBounds() const { return bounds; };

    /// glTF `material.doubleSided`: when true this mesh must render both faces
    /// (the raster pass sets a dynamic cull mode of NONE per draw). Default
    /// false = back-face culled, matching every OBJ mesh and single-sided glTF.
    /// Set after construction by the loader, so the Mesh constructor is unchanged.
    void setDoubleSided(bool value) { double_sided = value; };
    bool isDoubleSided() const { return double_sided; };

    /// Whether any material this mesh references is a glTF MASK material
    /// (ObjMaterial::alphaCutoff >= 0). Derived once in the constructor from
    /// the material vector the constructor already receives - unlike
    /// double_sided this needs no setter, since every loader already hands
    /// the constructor its full materials, so deriving it here can never go
    /// stale the way a loader-set flag could.
    bool hasMaskedMaterial() const { return has_masked_material; };

    ~Mesh();

  private:
    AABB bounds{ glm::vec3(1.0F), glm::vec3(-1.0F) };
    VulkanBufferManager vulkanBufferManager;

    static constexpr uint64_t INVALID_ADDR = ~uint64_t(0);
    ObjectDescription object_description{ INVALID_ADDR, INVALID_ADDR, INVALID_ADDR, INVALID_ADDR };

    VulkanBuffer vertexBuffer;
    VulkanBuffer indexBuffer;
    VulkanBuffer materialIdsBuffer;
    VulkanBuffer materialsBuffer;

    static constexpr uint32_t INVALID_COUNT = ~uint32_t(0);
    uint32_t vertex_count{ INVALID_COUNT };
    uint32_t index_count{ INVALID_COUNT };

    // glTF material.doubleSided; see setDoubleSided.
    bool double_sided{ false };

    // See hasMaskedMaterial().
    bool has_masked_material{ false };

    std::shared_ptr<VulkanDevice> device{ nullptr };

    void createVertexBuffer(vk::CommandPool transfer_command_pool, const std::vector<Vertex> &vertices);

    void createIndexBuffer(vk::CommandPool transfer_command_pool, const std::vector<uint32_t> &indices);

    void createMaterialIDBuffer(vk::CommandPool transfer_command_pool, const std::vector<unsigned int> &materialIndex);

    void createMaterialBuffer(vk::CommandPool transfer_command_pool, const std::vector<ObjMaterial> &materials);

    // Shared body of the four create*Buffer uploads: a device-local buffer,
    // eTransferDst plus the caller's `baseUsage` bit(s), and - when the device
    // supports it - the buffer-device-address / ray-tracing-input usage and the
    // matching allocate flag, so the buffer's address can be fetched afterwards.
    // Each create*Buffer differs only in its target buffer and base usage bit,
    // so they all funnel through here.
    template<typename T>
    bool uploadDeviceLocalBuffer(vk::CommandPool transfer_command_pool,
      VulkanBuffer &targetBuffer,
      vk::BufferUsageFlags baseUsage,
      const std::vector<T> &data);
};

template<typename T>
inline bool Mesh::uploadDeviceLocalBuffer(vk::CommandPool transfer_command_pool,
  VulkanBuffer &targetBuffer,
  vk::BufferUsageFlags baseUsage,
  const std::vector<T> &data)
{
    vk::BufferUsageFlags usage_flags = {};
    usage_flags |= vk::BufferUsageFlagBits::eTransferDst;
    usage_flags |= baseUsage;
    vk::MemoryPropertyFlags const memory_property_flags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    vk::MemoryAllocateFlags memory_allocate_flags = {};

    if (device->supportsBufferDeviceAddress()) {
        usage_flags |= vk::BufferUsageFlagBits::eShaderDeviceAddress;
        if (device->supportsHardwareAcceleratedRRT()) {
            usage_flags |= vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
        }
        memory_allocate_flags |= vk::MemoryAllocateFlagBits::eDeviceAddress;
    }

    return vulkanBufferManager.createBufferAndUploadVectorOnDevice(
      device, transfer_command_pool, targetBuffer, usage_flags, memory_property_flags, data, memory_allocate_flags);
}
}// namespace Kataglyphis
