// OxidANT's shared Slang vector kernels as SPIR-V on a headless Vulkan device, against the files its WGSL runner reads.

#include "GpuTestGuard.hpp"
#include "RepoFiles.hpp"

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::uint32_t kUnwritten = 0xFFFFFFFFU;
constexpr std::uint32_t kSpirvMagic = 0x07230203U;
constexpr std::uint64_t kFenceTimeoutNs = 120ULL * 1000ULL * 1000ULL * 1000ULL;

/// `width` words of `kind` (f32, u32 or i32); an f32 output passes within `abs + rel * |ref|`.
struct Port
{
    std::string name;
    std::string kind;
    std::size_t width = 0;
    double abs = 0.0;
    double rel = 0.0;
};

/// One vector file: the kernel and entry it names, its ports and its cases.
struct Vectors
{
    std::string kernel;
    std::string entry;
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    json cases;
};

std::size_t words(const std::vector<Port> &ports)
{
    std::size_t total = 0;
    for (const Port &port : ports) { total += port.width; }
    return total;
}

bool is_text(const json &object, const char *key) { return object.contains(key) && object[key].is_string(); }

// nlohmann aborts on a type mismatch without exceptions, so every field is checked before it is read.
std::optional<std::vector<Port>> parse_ports(const json &doc, const char *key, std::string &error)
{
    if (!doc.contains(key) || !doc[key].is_array() || doc[key].empty()) {
        error = std::string("no `") + key + "` list";
        return std::nullopt;
    }
    std::vector<Port> ports;
    for (const json &row : doc[key]) {
        if (!row.is_object() || !is_text(row, "name") || !is_text(row, "type") || !row.contains("width")
            || !row["width"].is_number_integer() || row["width"].get<std::int64_t>() < 1) {
            error = std::string("a `") + key + "` row lacks its name, type or width: " + row.dump();
            return std::nullopt;
        }
        Port port{ row["name"].get<std::string>(),
            row["type"].get<std::string>(),
            static_cast<std::size_t>(row["width"].get<std::int64_t>()) };
        if (port.kind != "f32" && port.kind != "u32" && port.kind != "i32") {
            error = "port " + port.name + " has the unknown type " + port.kind;
            return std::nullopt;
        }
        for (const char *bound : { "abs", "rel" }) {
            if (row.contains(bound) && !row[bound].is_number()) {
                error = "port " + port.name + "'s " + bound + " is not a number";
                return std::nullopt;
            }
        }
        port.abs = row.contains("abs") ? row["abs"].get<double>() : 0.0;
        port.rel = row.contains("rel") ? row["rel"].get<double>() : 0.0;
        ports.push_back(std::move(port));
    }
    return ports;
}

std::optional<Vectors> parse_vectors(const json &doc, std::string &error)
{
    if (doc.is_discarded() || !doc.is_object()) {
        error = "not a JSON object";
        return std::nullopt;
    }
    if (!is_text(doc, "kernel") || !is_text(doc, "entry")) {
        error = "no `kernel` or `entry`";
        return std::nullopt;
    }
    auto inputs = parse_ports(doc, "inputs", error);
    auto outputs = inputs ? parse_ports(doc, "outputs", error) : std::nullopt;
    if (!outputs) { return std::nullopt; }
    if (!doc.contains("cases") || !doc["cases"].is_array() || doc["cases"].empty()) {
        error = "no `cases` list";
        return std::nullopt;
    }
    return Vectors{
        doc["kernel"].get<std::string>(), doc["entry"].get<std::string>(), *inputs, *outputs, doc["cases"]
    };
}

/// One value as the kernel's word: an f32's IEEE bits, a u32 or an i32 as the integer; empty when it does not fit.
std::optional<std::uint32_t> encode(const std::string &kind, const json &value)
{
    if (kind == "f32") {
        if (!value.is_number()) { return std::nullopt; }
        return std::bit_cast<std::uint32_t>(static_cast<float>(value.get<double>()));
    }
    if (!value.is_number_integer()) { return std::nullopt; }
    if (value.is_number_unsigned()) {
        const std::uint64_t word = value.get<std::uint64_t>();
        const std::uint64_t limit = kind == "u32"
                                      ? std::numeric_limits<std::uint32_t>::max()
                                      : static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
        if (word > limit) { return std::nullopt; }
        return static_cast<std::uint32_t>(word);
    }
    const std::int64_t signed_word = value.get<std::int64_t>();
    if (kind == "u32" || signed_word < std::numeric_limits<std::int32_t>::min()) { return std::nullopt; }
    return std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(signed_word));
}

/// The two header words, input and output words per case, then every case's inputs in port order.
std::optional<std::vector<std::uint32_t>> pack(const Vectors &vectors, std::string &error)
{
    std::vector<std::uint32_t> packed{ static_cast<std::uint32_t>(words(vectors.inputs)),
        static_cast<std::uint32_t>(words(vectors.outputs)) };
    for (const json &entry : vectors.cases) {
        const std::string name = is_text(entry, "name") ? entry["name"].get<std::string>() : entry.dump();
        if (!entry.contains("inputs") || !entry["inputs"].is_object()) {
            error = name + ": no inputs";
            return std::nullopt;
        }
        for (const Port &port : vectors.inputs) {
            const json &inputs = entry["inputs"];
            if (!inputs.contains(port.name) || !inputs[port.name].is_array()
                || inputs[port.name].size() != port.width) {
                error = name + ": input " + port.name + " is not " + std::to_string(port.width) + " values";
                return std::nullopt;
            }
            for (const json &value : inputs[port.name]) {
                const auto word = encode(port.kind, value);
                if (!word) {
                    error = name + ": input " + port.name + " holds " + value.dump() + ", not a " + port.kind;
                    return std::nullopt;
                }
                packed.push_back(*word);
            }
        }
    }
    return packed;
}

/// `word`'s distance from `reference` in tolerances, above 1 a miss; NaN, an unwritten word, is infinitely far.
double distance(const Port &port, const json &reference, std::uint32_t word)
{
    constexpr double kFar = std::numeric_limits<double>::infinity();
    if (port.kind != "f32") {
        const auto expected = encode(port.kind, reference);
        return expected.has_value() && *expected == word ? 0.0 : kFar;
    }
    if (!reference.is_number()) { return kFar; }
    const double gpu = static_cast<double>(std::bit_cast<float>(word));
    const double expected = reference.get<double>();
    if (gpu == expected) { return 0.0; }
    const double far = std::abs(gpu - expected) / (port.abs + port.rel * std::abs(expected));
    return std::isnan(far) ? kFar : far;
}

std::string describe(const Port &port, const json &reference, std::uint32_t word)
{
    std::ostringstream text;
    if (port.kind != "f32") {
        text << "gpu " << word << " against " << reference.dump();
        return text.str();
    }
    const double expected = reference.is_number() ? reference.get<double>() : std::nan("");
    text.precision(9);
    text << "gpu " << std::bit_cast<float>(word) << " against " << expected << ", tolerance "
         << port.abs + port.rel * std::abs(expected);
    return text.str();
}

/// A compute-only device without a surface; the destructor releases it, also after an early ASSERT return.
struct ComputeDevice
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkPhysicalDeviceMemoryProperties memory{};

    ComputeDevice() = default;
    ComputeDevice(const ComputeDevice &) = delete;
    ComputeDevice &operator=(const ComputeDevice &) = delete;
    ComputeDevice(ComputeDevice &&) = delete;
    ComputeDevice &operator=(ComputeDevice &&) = delete;
    ~ComputeDevice()
    {
        if (device != VK_NULL_HANDLE) { vkDestroyDevice(device, nullptr); }
        if (instance != VK_NULL_HANDLE) { vkDestroyInstance(instance, nullptr); }
    }

    /// Empty once the first device with a compute queue is open, else why none is.
    std::string open()
    {
        const VkApplicationInfo app{ .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "sharedShaderVectors",
            .apiVersion = VK_API_VERSION_1_3 };
        const VkInstanceCreateInfo instance_info{ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app };
        if (const VkResult result = vkCreateInstance(&instance_info, nullptr, &instance); result != VK_SUCCESS) {
            instance = VK_NULL_HANDLE;
            return "vkCreateInstance returned " + std::to_string(result);
        }
        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        for (VkPhysicalDevice candidate : devices) {
            std::uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
            for (std::uint32_t index = 0; index < family_count && physical == VK_NULL_HANDLE; ++index) {
                if ((families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U) {
                    physical = candidate;
                    queue_family = index;
                }
            }
            if (physical != VK_NULL_HANDLE) { break; }
        }
        if (physical == VK_NULL_HANDLE) {
            return std::to_string(count) + " physical device(s), none with a compute queue";
        }
        const float priority = 1.0F;
        const VkDeviceQueueCreateInfo queue_info{ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = queue_family,
            .queueCount = 1,
            .pQueuePriorities = &priority };
        const VkDeviceCreateInfo device_info{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info
        };
        if (const VkResult result = vkCreateDevice(physical, &device_info, nullptr, &device); result != VK_SUCCESS) {
            device = VK_NULL_HANDLE;
            return "vkCreateDevice returned " + std::to_string(result);
        }
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        return {};
    }
};

/// One host-visible, coherent storage buffer.
struct HostBuffer
{
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;

    explicit HostBuffer(VkDevice owner) : device(owner) {}
    HostBuffer(const HostBuffer &) = delete;
    HostBuffer &operator=(const HostBuffer &) = delete;
    HostBuffer(HostBuffer &&) = delete;
    HostBuffer &operator=(HostBuffer &&) = delete;
    ~HostBuffer()
    {
        if (buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, buffer, nullptr); }
        if (memory != VK_NULL_HANDLE) { vkFreeMemory(device, memory, nullptr); }
    }

    /// Creates it holding `contents`; empty on success, else the failing call.
    std::string create(const ComputeDevice &gpu, const std::vector<std::uint32_t> &contents)
    {
        size = static_cast<VkDeviceSize>(contents.size() * sizeof(std::uint32_t));
        const VkBufferCreateInfo info{ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) { return "vkCreateBuffer failed"; }
        VkMemoryRequirements needs{};
        vkGetBufferMemoryRequirements(device, buffer, &needs);
        constexpr VkMemoryPropertyFlags kHost =
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::optional<std::uint32_t> type;
        for (std::uint32_t index = 0; index < gpu.memory.memoryTypeCount && !type; ++index) {
            if ((needs.memoryTypeBits & (1U << index)) != 0U
                && (gpu.memory.memoryTypes[index].propertyFlags & kHost) == kHost) {
                type = index;
            }
        }
        if (!type) { return "no host-visible, coherent memory type"; }
        const VkMemoryAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = needs.size, .memoryTypeIndex = *type
        };
        if (vkAllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS) { return "vkAllocateMemory failed"; }
        if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) { return "vkBindBufferMemory failed"; }
        void *mapped = nullptr;
        if (vkMapMemory(device, memory, 0, size, 0, &mapped) != VK_SUCCESS) { return "vkMapMemory failed"; }
        std::memcpy(mapped, contents.data(), static_cast<std::size_t>(size));
        vkUnmapMemory(device, memory);
        return {};
    }

    /// Its words, or empty when it cannot be mapped.
    std::optional<std::vector<std::uint32_t>> read() const
    {
        std::vector<std::uint32_t> contents(static_cast<std::size_t>(size) / sizeof(std::uint32_t));
        void *mapped = nullptr;
        if (vkMapMemory(device, memory, 0, size, 0, &mapped) != VK_SUCCESS) { return std::nullopt; }
        std::memcpy(contents.data(), mapped, static_cast<std::size_t>(size));
        vkUnmapMemory(device, memory);
        return contents;
    }
};

/// The objects of one dispatch, released in reverse whatever step failed.
struct Dispatch
{
    VkDevice device = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    explicit Dispatch(VkDevice owner) : device(owner) {}
    Dispatch(const Dispatch &) = delete;
    Dispatch &operator=(const Dispatch &) = delete;
    Dispatch(Dispatch &&) = delete;
    Dispatch &operator=(Dispatch &&) = delete;
    ~Dispatch()
    {
        if (fence != VK_NULL_HANDLE) { vkDestroyFence(device, fence, nullptr); }
        if (commands != VK_NULL_HANDLE) { vkDestroyCommandPool(device, commands, nullptr); }
        if (pool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, pool, nullptr); }
        if (pipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, pipeline, nullptr); }
        if (pipeline_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, pipeline_layout, nullptr); }
        if (set_layout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, set_layout, nullptr); }
        if (module != VK_NULL_HANDLE) { vkDestroyShaderModule(device, module, nullptr); }
    }
};

/// Runs the kernel's `main` once per case over `input` into `output`; empty on success, else the failing call.
std::string run_kernel(const ComputeDevice &gpu,
  const std::vector<std::uint32_t> &spirv,
  const HostBuffer &input,
  const HostBuffer &output,
  std::uint32_t cases)
{
    Dispatch d(gpu.device);
    const VkShaderModuleCreateInfo module_info{ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spirv.size() * sizeof(std::uint32_t),
        .pCode = spirv.data() };
    if (vkCreateShaderModule(d.device, &module_info, nullptr, &d.module) != VK_SUCCESS) {
        return "vkCreateShaderModule failed";
    }
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{ {
      { .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
      { .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    } };
    const VkDescriptorSetLayoutCreateInfo set_info{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data() };
    if (vkCreateDescriptorSetLayout(d.device, &set_info, nullptr, &d.set_layout) != VK_SUCCESS) {
        return "vkCreateDescriptorSetLayout failed";
    }
    const VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &d.set_layout
    };
    if (vkCreatePipelineLayout(d.device, &layout_info, nullptr, &d.pipeline_layout) != VK_SUCCESS) {
        return "vkCreatePipelineLayout failed";
    }
    // Slang names every SPIR-V entry point "main".
    const VkComputePipelineCreateInfo pipeline_info{ .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_COMPUTE_BIT,
          .module = d.module,
          .pName = "main" },
        .layout = d.pipeline_layout };
    if (vkCreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &d.pipeline) != VK_SUCCESS) {
        return "vkCreateComputePipelines failed";
    }
    const VkDescriptorPoolSize pool_size{ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
    const VkDescriptorPoolCreateInfo pool_info{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size };
    if (vkCreateDescriptorPool(d.device, &pool_info, nullptr, &d.pool) != VK_SUCCESS) {
        return "vkCreateDescriptorPool failed";
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    const VkDescriptorSetAllocateInfo set_allocate{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = d.pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &d.set_layout };
    if (vkAllocateDescriptorSets(d.device, &set_allocate, &set) != VK_SUCCESS) {
        return "vkAllocateDescriptorSets failed";
    }
    const std::array<VkDescriptorBufferInfo, 2> buffers{ { { input.buffer, 0, VK_WHOLE_SIZE },
      { output.buffer, 0, VK_WHOLE_SIZE } } };
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t binding = 0; binding < 2; ++binding) {
        writes[binding] = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffers[binding] };
    }
    vkUpdateDescriptorSets(d.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    const VkCommandPoolCreateInfo command_info{ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = gpu.queue_family };
    if (vkCreateCommandPool(d.device, &command_info, nullptr, &d.commands) != VK_SUCCESS) {
        return "vkCreateCommandPool failed";
    }
    VkCommandBuffer command = VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo command_allocate{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = d.commands,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1 };
    if (vkAllocateCommandBuffers(d.device, &command_allocate, &command) != VK_SUCCESS) {
        return "vkAllocateCommandBuffers failed";
    }
    const VkCommandBufferBeginInfo begin{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) { return "vkBeginCommandBuffer failed"; }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeline_layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(command, cases, 1, 1);
    const VkMemoryBarrier to_host{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(command,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT,
      0,
      1,
      &to_host,
      0,
      nullptr,
      0,
      nullptr);
    if (vkEndCommandBuffer(command) != VK_SUCCESS) { return "vkEndCommandBuffer failed"; }
    const VkFenceCreateInfo fence_info{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(d.device, &fence_info, nullptr, &d.fence) != VK_SUCCESS) { return "vkCreateFence failed"; }
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command
    };
    if (vkQueueSubmit(gpu.queue, 1, &submit, d.fence) != VK_SUCCESS) { return "vkQueueSubmit failed"; }
    if (const VkResult result = vkWaitForFences(d.device, 1, &d.fence, VK_TRUE, kFenceTimeoutNs);
      result != VK_SUCCESS) {
        // The kernel may still run: nothing it uses may be destroyed before the device is idle.
        vkDeviceWaitIdle(d.device);
        return "vkWaitForFences returned " + std::to_string(result);
    }
    return {};
}

/// The workgroup size of the module's first OpExecutionMode LocalSize; empty when it states none.
std::optional<std::array<std::uint32_t, 3>> local_size(const std::vector<std::uint32_t> &spirv)
{
    constexpr std::size_t kHeaderWords = 5;
    constexpr std::uint32_t kOpExecutionMode = 16U;
    constexpr std::uint32_t kExecutionModeLocalSize = 17U;
    for (std::size_t at = kHeaderWords; at < spirv.size();) {
        const std::uint32_t count = spirv[at] >> 16U;
        if (count == 0U || at + count > spirv.size()) { return std::nullopt; }
        if ((spirv[at] & 0xFFFFU) == kOpExecutionMode && count >= 6U && spirv[at + 2U] == kExecutionModeLocalSize) {
            return std::array<std::uint32_t, 3>{ spirv[at + 3U], spirv[at + 4U], spirv[at + 5U] };
        }
        at += count;
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint32_t>> read_spirv(const fs::path &path)
{
    const auto bytes = Kataglyphis::TestSupport::readFileText(path);
    if (!bytes || bytes->empty() || bytes->size() % sizeof(std::uint32_t) != 0) { return std::nullopt; }
    std::vector<std::uint32_t> code(bytes->size() / sizeof(std::uint32_t));
    std::memcpy(code.data(), bytes->data(), bytes->size());
    if (code.front() != kSpirvMagic) { return std::nullopt; }
    return code;
}

/// Every component of `got` that misses its reference, "case: output[k] ...", plus the closest call of the rest.
std::vector<std::string> misses(const Vectors &vectors, const std::vector<std::uint32_t> &got, std::string &closest)
{
    std::vector<std::string> found;
    double nearest = 0.0;
    closest = "none";
    std::size_t at = 0;
    for (const json &entry : vectors.cases) {
        const std::string name = is_text(entry, "name") ? entry["name"].get<std::string>() : entry.dump();
        for (const Port &port : vectors.outputs) {
            if (at + port.width > got.size()) {
                found.push_back("the read-back ends at word " + std::to_string(got.size()) + ", before " + name);
                return found;
            }
            const bool listed = entry.contains("outputs") && entry["outputs"].is_object()
                                && entry["outputs"].contains(port.name) && entry["outputs"][port.name].is_array()
                                && entry["outputs"][port.name].size() == port.width;
            if (!listed) {
                found.push_back(
                  name + ": output " + port.name + " is not " + std::to_string(port.width) + " references");
                at += port.width;
                continue;
            }
            for (std::size_t k = 0; k < port.width; ++k, ++at) {
                const json &reference = entry["outputs"][port.name][k];
                const double far = distance(port, reference, got[at]);
                const std::string label = name + ": " + port.name + "[" + std::to_string(k) + "]";
                if (far > 1.0) {
                    found.push_back(label + " " + describe(port, reference, got[at]));
                } else if (far > nearest) {
                    nearest = far;
                    closest = label + " at " + std::to_string(far) + " of its tolerance";
                }
            }
        }
    }
    if (std::any_of(got.begin() + static_cast<std::ptrdiff_t>(at), got.end(), [](std::uint32_t word) {
            return word != kUnwritten;
        })) {
        found.emplace_back("the kernel wrote past its last case");
    }
    return found;
}

void check_module(const ComputeDevice &gpu, const fs::path &file)
{
    const std::string module = file.stem().string();
    const auto text = Kataglyphis::TestSupport::readFileText(file);
    if (!text) {
        ADD_FAILURE() << module << ": cannot read " << file.string();
        return;
    }
    std::string error;
    const auto vectors = parse_vectors(json::parse(*text, nullptr, /*allow_exceptions=*/false), error);
    const auto packed = vectors ? pack(*vectors, error) : std::nullopt;
    if (!packed) {
        ADD_FAILURE() << module << ": " << file.string() << ": " << error;
        return;
    }
    // The hub's driver writes tests/<name>.slang's entry to build/spirv/tests/<name>.<entry>.spv.
    const fs::path kernel(vectors->kernel);
    const fs::path spv_path = Kataglyphis::TestSupport::spirvRoot() / kernel.parent_path()
                              / (kernel.stem().string() + "." + vectors->entry + ".spv");
    const auto spirv = read_spirv(spv_path);
    if (!spirv) {
        ADD_FAILURE() << module << ": no SPIR-V at " << spv_path.string() << " - compile the Slang shaders first";
        return;
    }
    // One invocation per case: a wider group would read past the inputs and write past the one spare case.
    const auto group = local_size(*spirv);
    if (!group || *group != std::array<std::uint32_t, 3>{ 1U, 1U, 1U }) {
        ADD_FAILURE() << module << ": " << spv_path.string() << " declares "
                      << (group ? std::to_string((*group)[0]) + "x" + std::to_string((*group)[1]) + "x"
                                    + std::to_string((*group)[2])
                                : std::string("no LocalSize"))
                      << ", but the runner dispatches [numthreads(1, 1, 1)] once per case";
        return;
    }

    const auto cases = static_cast<std::uint32_t>(vectors->cases.size());
    const std::size_t output_words = (static_cast<std::size_t>(cases) + 1U) * words(vectors->outputs);
    HostBuffer input(gpu.device);
    HostBuffer output(gpu.device);
    error = input.create(gpu, *packed);
    if (error.empty()) { error = output.create(gpu, std::vector<std::uint32_t>(output_words, kUnwritten)); }
    if (error.empty()) { error = run_kernel(gpu, *spirv, input, output, cases); }
    if (!error.empty()) {
        ADD_FAILURE() << module << ": " << error;
        return;
    }
    const auto got = output.read();
    if (!got || got->size() != output_words) {
        ADD_FAILURE() << module << ": the output buffer did not read back"
                      << (got ? " in full: " + std::to_string(got->size()) + " of " + std::to_string(output_words)
                                  + " words"
                              : std::string(": vkMapMemory failed"));
        return;
    }
    std::string closest;
    const std::vector<std::string> found = misses(*vectors, *got, closest);
    GTEST_LOG_(INFO) << module << ": " << cases << " cases, closest passing component " << closest;
    EXPECT_TRUE(found.empty()) << module << ": " << found.size() << " component(s) miss their reference:"
                               << Kataglyphis::TestSupport::joinViolations(found);
}

}// namespace

// Every vector file OxidANT ships, so its next shared module needs no edit here; a miss names module, case, component.
TEST(Integration, SharedSlangKernelsMeetTheirVectors)
{
    const fs::path vectors_dir = Kataglyphis::TestSupport::sharedSlangRoot() / "tests" / "vectors";
    std::vector<fs::path> files;
    std::error_code error;
    for (fs::directory_iterator it(vectors_dir, error), end; !error && it != end; it.increment(error)) {
        if (it->path().extension() == ".json") { files.push_back(it->path()); }
    }
    std::sort(files.begin(), files.end());
    ASSERT_GE(files.size(), 3U) << vectors_dir.string() << " holds " << files.size()
                                << " vector files - is OxidANT checked out and staged?";

    ComputeDevice gpu;
    SKIP_WITHOUT_HEADLESS_DEVICE(gpu.open());
    for (const fs::path &file : files) { check_module(gpu, file); }
}
