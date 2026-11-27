// Build integrity: host/device structs match the compiled SPIR-V's offsets, and a shader reads every field.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"
#include "ObjectDescription.hpp"
#include "renderer/FxaaUniforms.hpp"
#include "renderer/GlobalUBO.hpp"
#include "renderer/SceneUBO.hpp"
#include "renderer/SsaoUniforms.hpp"
#include "renderer/pushConstants/PushConstantPathTracing.hpp"
#include "renderer/pushConstants/PushConstantPost.hpp"
#include "renderer/pushConstants/PushConstantRasterizer.hpp"
#include "renderer/pushConstants/PushConstantRayTracing.hpp"
#include "scene/sky_box/SkyUniforms.hpp"
#include "shared/scene/ObjMaterial.hpp"
#include "shared/scene/Vertex.hpp"

import kataglyphis.vulkan.cascaded_shadow_map;

using namespace Kataglyphis::TestSupport::BuildIntegrity;

namespace {

using Kataglyphis::ShadowPushConstants;
using Kataglyphis::VulkanRendererInternals::DirectionalLightData;
using Kataglyphis::VulkanRendererInternals::GlobalUBO;
using Kataglyphis::VulkanRendererInternals::PushConstantPathTracing;
using Kataglyphis::VulkanRendererInternals::PushConstantPost;
using Kataglyphis::VulkanRendererInternals::PushConstantRasterizer;
using Kataglyphis::VulkanRendererInternals::PushConstantRaytracing;
using Kataglyphis::VulkanRendererInternals::SceneUBO;

constexpr uint32_t kOpName = 5;
constexpr uint32_t kOpMemberName = 6;
constexpr uint32_t kOpMemberDecorate = 72;
constexpr uint32_t kDecorationOffset = 35;

// SPIR-V literal strings pack four bytes per word, low byte first, NUL-terminated and word-padded.
std::string spirv_literal_string(const std::vector<uint32_t> &words, std::size_t word_start, std::size_t word_count)
{
    std::string text;
    text.reserve(word_count * 4);
    for (std::size_t i = 0; i < word_count; ++i) {
        const uint32_t word = words[word_start + i];
        for (uint32_t shift = 0; shift < 32U; shift += 8U) {
            const auto ch = static_cast<char>((word >> shift) & 0xFFU);
            if (ch == '\0') { return text; }
            text.push_back(ch);
        }
    }
    return text;
}

// One emitted struct as the shader sees it: member offsets, its size, its arrays' strides and its strides as an element.
struct SpirvStructLayout
{
    std::map<std::string, uint32_t> member_offsets;
    std::set<uint32_t> sizes;// one per distinct emission; two mean two shaders disagree
    std::map<std::string, std::set<uint32_t>> member_array_strides;
    std::set<uint32_t> element_strides;// ArrayStride of every array of it and every buffer-address pointer to it
};

// Every named struct of every .spv under spirv_root, merged by its emitted name.
std::map<std::string, SpirvStructLayout> parse_spirv_struct_layouts(const fs::path &spirv_root)
{
    constexpr uint32_t kOpTypeBool = 20;
    constexpr uint32_t kOpTypeInt = 21;
    constexpr uint32_t kOpTypeFloat = 22;
    constexpr uint32_t kOpTypeVector = 23;
    constexpr uint32_t kOpTypeMatrix = 24;
    constexpr uint32_t kOpTypeArray = 28;
    constexpr uint32_t kOpTypeRuntimeArray = 29;
    constexpr uint32_t kOpTypeStruct = 30;
    constexpr uint32_t kOpTypePointer = 32;
    constexpr uint32_t kOpConstant = 43;
    constexpr uint32_t kOpDecorate = 71;
    constexpr uint32_t kDecorationArrayStride = 6;
    constexpr uint32_t kDecorationMatrixStride = 7;
    constexpr uint32_t kStorageClassPhysicalStorageBuffer = 5349;

    std::map<std::string, SpirvStructLayout> layouts;
    std::error_code error;
    for (fs::recursive_directory_iterator it(spirv_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".spv") { continue; }
        const auto text = readFileText(it->path());
        if (!text || text->size() < kSpirvHeaderWordCount * sizeof(uint32_t) || text->size() % sizeof(uint32_t) != 0) {
            continue;
        }
        std::vector<uint32_t> words(text->size() / sizeof(uint32_t));
        std::memcpy(words.data(), text->data(), text->size());
        if (words[0] != kSpirvMagicNumber) { continue; }

        std::map<uint32_t, std::string> names;
        std::map<std::pair<uint32_t, uint32_t>, std::string> member_names;
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> offsets;
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> matrix_strides;
        std::map<uint32_t, uint32_t> array_strides;
        std::map<uint32_t, std::pair<uint32_t, std::vector<uint32_t>>> types;// id -> (opcode, operands)
        std::map<uint32_t, uint32_t> constants;
        for (std::size_t pos = kSpirvHeaderWordCount; pos < words.size();) {
            const uint32_t word_count = words[pos] >> 16U;
            const uint32_t opcode = words[pos] & 0xFFFFU;
            if (word_count == 0 || pos + word_count > words.size()) { break; }
            const std::vector<uint32_t> args(words.begin() + static_cast<std::ptrdiff_t>(pos) + 1,
              words.begin() + static_cast<std::ptrdiff_t>(pos + word_count));
            if (opcode == kOpName && !args.empty()) {
                names[args[0]] = spirv_literal_string(words, pos + 2, word_count - 2);
            } else if (opcode == kOpMemberName && args.size() >= 2) {
                member_names[{ args[0], args[1] }] = spirv_literal_string(words, pos + 3, word_count - 3);
            } else if (opcode == kOpMemberDecorate && args.size() >= 4 && args[2] == kDecorationOffset) {
                offsets[{ args[0], args[1] }] = args[3];
            } else if (opcode == kOpMemberDecorate && args.size() >= 4 && args[2] == kDecorationMatrixStride) {
                matrix_strides[{ args[0], args[1] }] = args[3];
            } else if (opcode == kOpDecorate && args.size() >= 3 && args[1] == kDecorationArrayStride) {
                array_strides[args[0]] = args[2];
            } else if (opcode == kOpConstant && args.size() >= 3) {
                constants[args[1]] = args[2];
            } else if ((opcode >= kOpTypeBool && opcode <= kOpTypeMatrix) || opcode == kOpTypeArray
                       || opcode == kOpTypeRuntimeArray || opcode == kOpTypeStruct || opcode == kOpTypePointer) {
                if (!args.empty()) { types[args[0]] = { opcode, std::vector<uint32_t>(args.begin() + 1, args.end()) }; }
            }
            pos += word_count;
        }

        // Bytes a value of `type` spans; a matrix member takes its parent's MatrixStride, a runtime array spans none.
        std::function<uint32_t(uint32_t, uint32_t)> size_of = [&](uint32_t type, uint32_t matrix_stride) -> uint32_t {
            const auto found = types.find(type);
            if (found == types.end()) { return 0; }
            const auto &[opcode, operands] = found->second;
            switch (opcode) {
            case kOpTypeBool:
                return 4;
            case kOpTypeInt:
            case kOpTypeFloat:
                return operands.empty() ? 0 : operands[0] / 8U;
            case kOpTypeVector:
                return operands.size() < 2 ? 0 : operands[1] * size_of(operands[0], 0);
            case kOpTypeMatrix:
                if (operands.size() < 2) { return 0; }
                return operands[1] * (matrix_stride != 0 ? matrix_stride : size_of(operands[0], 0));
            case kOpTypeArray: {
                const auto stride = array_strides.find(type);
                const auto length = operands.size() < 2 ? constants.end() : constants.find(operands[1]);
                if (stride == array_strides.end() || length == constants.end()) { return 0; }
                return stride->second * length->second;
            }
            case kOpTypePointer:
                return 8;
            case kOpTypeStruct: {
                uint32_t extent = 0;
                for (uint32_t member = 0; member < operands.size(); ++member) {
                    const auto offset = offsets.find({ type, member });
                    if (offset == offsets.end()) { continue; }
                    const auto stride = matrix_strides.find({ type, member });
                    const uint32_t member_size =
                      size_of(operands[member], stride == matrix_strides.end() ? 0 : stride->second);
                    extent = std::max(extent, offset->second + member_size);
                }
                return extent;
            }
            default:
                return 0;
            }
        };
        // An array member's stride, also through the one-member struct Slang wraps a std140 array in.
        const auto array_stride_of = [&](uint32_t type) -> std::optional<uint32_t> {
            for (int depth = 0; depth < 2; ++depth) {
                const auto found = types.find(type);
                if (found == types.end()) { return std::nullopt; }
                const auto &[opcode, operands] = found->second;
                if (opcode == kOpTypeArray || opcode == kOpTypeRuntimeArray) {
                    const auto stride = array_strides.find(type);
                    if (stride == array_strides.end()) { return std::nullopt; }
                    return stride->second;
                }
                const auto wrapper = names.find(type);
                if (opcode != kOpTypeStruct || operands.size() != 1 || wrapper == names.end()
                    || !wrapper->second.starts_with("_Array_")) {
                    return std::nullopt;
                }
                type = operands[0];
            }
            return std::nullopt;
        };

        for (const auto &[type, entry] : types) {
            const auto &[opcode, operands] = entry;
            const auto name = names.find(type);
            if (opcode == kOpTypeStruct && name != names.end() && !name->second.starts_with("_")) {
                auto &layout = layouts[name->second];
                for (uint32_t member = 0; member < operands.size(); ++member) {
                    const auto member_name = member_names.find({ type, member });
                    const auto offset = offsets.find({ type, member });
                    if (member_name == member_names.end() || offset == offsets.end()) { continue; }
                    layout.member_offsets[member_name->second] = offset->second;
                    if (const auto stride = array_stride_of(operands[member])) {
                        layout.member_array_strides[member_name->second].insert(*stride);
                    }
                }
                layout.sizes.insert(size_of(type, 0));
            }
            const bool is_array = opcode == kOpTypeArray || opcode == kOpTypeRuntimeArray;
            const bool is_address =
              opcode == kOpTypePointer && operands.size() >= 2 && operands[0] == kStorageClassPhysicalStorageBuffer;
            const uint32_t element = is_address ? operands[1] : (operands.empty() ? 0 : operands[0]);
            const auto element_name = names.find(element);
            const auto stride = array_strides.find(type);
            if ((is_array || is_address) && element_name != names.end() && stride != array_strides.end()
                && types.contains(element) && types.at(element).first == kOpTypeStruct) {
                layouts[element_name->second].element_strides.insert(stride->second);
            }
        }
    }
    return layouts;
}

// Emitted struct name, every member's host offset, the host size and alignment, and each array member's element size.
struct SpirvStructContract
{
    std::string spirv_name;
    std::map<std::string, std::size_t> member_offsets;
    std::size_t host_size;
    std::size_t host_alignment;
    std::map<std::string, std::size_t> member_array_strides{};
};

std::vector<SpirvStructContract> build_shared_struct_offset_contracts()
{
    return {
        { "SceneUBO_std140",
          { { "dirLight", offsetof(SceneUBO, dirLight) },
            { "pcfRadius", offsetof(SceneUBO, pcfRadius) },
            { "cascadedShadowIntensity", offsetof(SceneUBO, cascadedShadowIntensity) },
            { "numCascades", offsetof(SceneUBO, numCascades) },
            { "punctualShadowCount", offsetof(SceneUBO, punctualShadowCount) },
            { "cascadeSplits", offsetof(SceneUBO, cascadeSplits) },
            { "cascadeLightSpaceMatrices", offsetof(SceneUBO, cascadeLightSpaceMatrices) },
            { "view_dir", offsetof(SceneUBO, view_dir) },
            { "cam_pos", offsetof(SceneUBO, cam_pos) },
            { "cloudLightMarch", offsetof(SceneUBO, cloudLightMarch) },
            { "cloudMeshScale", offsetof(SceneUBO, cloudMeshScale) },
            { "cloudMeshOffset", offsetof(SceneUBO, cloudMeshOffset) },
            { "cloudParameters", offsetof(SceneUBO, cloudParameters) },
            { "cloudAmbientTop", offsetof(SceneUBO, cloudAmbientTop) },
            { "cloudAmbientBottom", offsetof(SceneUBO, cloudAmbientBottom) },
            { "iblParams", offsetof(SceneUBO, iblParams) },
            { "screenOcclusion", offsetof(SceneUBO, screenOcclusion) } },
          sizeof(SceneUBO),
          alignof(SceneUBO),
          { { "cascadeLightSpaceMatrices", sizeof(SceneUBO::cascadeLightSpaceMatrices[0]) } } },
        { "GlobalUBO_std140",
          { { "projection", offsetof(GlobalUBO, projection) },
            { "view", offsetof(GlobalUBO, view) },
            { "inv_projection", offsetof(GlobalUBO, inv_projection) },
            { "inv_view", offsetof(GlobalUBO, inv_view) } },
          sizeof(GlobalUBO),
          alignof(GlobalUBO) },
        // OxidANT's sky.slang, which the C++ renderer fills from SkyUniforms.hpp.
        { "SkyUniforms_std140",
          { { "inv_view_proj", offsetof(Kataglyphis::SkyUniforms, inv_view_proj) },
            { "light_dir_intensity", offsetof(Kataglyphis::SkyUniforms, light_dir_intensity) },
            { "mode", offsetof(Kataglyphis::SkyUniforms, mode) } },
          sizeof(Kataglyphis::SkyUniforms),
          alignof(Kataglyphis::SkyUniforms) },
        { "DirectionalLightData_std140",
          { { "direction", offsetof(DirectionalLightData, direction) },
            { "color", offsetof(DirectionalLightData, color) } },
          sizeof(DirectionalLightData),
          alignof(DirectionalLightData) },
        { "ObjectDescription_std430",
          { { "vertex_address", offsetof(ObjectDescription, vertex_address) },
            { "index_address", offsetof(ObjectDescription, index_address) },
            { "material_index_address", offsetof(ObjectDescription, material_index_address) },
            { "material_address", offsetof(ObjectDescription, material_address) },
            { "texture_offset", offsetof(ObjectDescription, texture_offset) } },
          sizeof(ObjectDescription),
          alignof(ObjectDescription) },
        { "ObjMaterial_natural",
          { { "diffuse", offsetof(ObjMaterial, diffuse) },
            { "emission", offsetof(ObjMaterial, emission) },
            { "shininess", offsetof(ObjMaterial, shininess) },
            { "dissolve", offsetof(ObjMaterial, dissolve) },
            { "textureID", offsetof(ObjMaterial, textureID) },
            { "alphaCutoff", offsetof(ObjMaterial, alphaCutoff) },
            { "uv_transform_row0", offsetof(ObjMaterial, uv_transform_row0) },
            { "uv_transform_row1", offsetof(ObjMaterial, uv_transform_row1) },
            { "metallic", offsetof(ObjMaterial, metallic) },
            { "roughness", offsetof(ObjMaterial, roughness) },
            { "emissiveTextureID", offsetof(ObjMaterial, emissiveTextureID) },
            { "normalTextureID", offsetof(ObjMaterial, normalTextureID) },
            { "normalScale", offsetof(ObjMaterial, normalScale) },
            { "metallicRoughnessTextureID", offsetof(ObjMaterial, metallicRoughnessTextureID) },
            { "normal_uv_transform_row0", offsetof(ObjMaterial, normal_uv_transform_row0) },
            { "normal_uv_transform_row1", offsetof(ObjMaterial, normal_uv_transform_row1) },
            { "metallic_roughness_uv_transform_row0", offsetof(ObjMaterial, metallic_roughness_uv_transform_row0) },
            { "metallic_roughness_uv_transform_row1", offsetof(ObjMaterial, metallic_roughness_uv_transform_row1) },
            { "emissive_uv_transform_row0", offsetof(ObjMaterial, emissive_uv_transform_row0) },
            { "emissive_uv_transform_row1", offsetof(ObjMaterial, emissive_uv_transform_row1) },
            { "unlit", offsetof(ObjMaterial, unlit) },
            { "alphaTextureID", offsetof(ObjMaterial, alphaTextureID) },
            { "uv1SlotMask", offsetof(ObjMaterial, uv1SlotMask) },
            { "alphaMode", offsetof(ObjMaterial, alphaMode) },
            { "occlusionTextureID", offsetof(ObjMaterial, occlusionTextureID) },
            { "occlusionStrength", offsetof(ObjMaterial, occlusionStrength) },
            { "occlusion_uv_transform_row0", offsetof(ObjMaterial, occlusion_uv_transform_row0) },
            { "occlusion_uv_transform_row1", offsetof(ObjMaterial, occlusion_uv_transform_row1) } },
          sizeof(ObjMaterial),
          alignof(ObjMaterial) },
        { "Vertex_natural",
          { { "position", offsetof(Vertex, position) },
            { "normal", offsetof(Vertex, normal) },
            { "color", offsetof(Vertex, color) },
            { "texture_coords", offsetof(Vertex, texture_coords) },
            { "tangent", offsetof(Vertex, tangent) },
            { "texture_coords1", offsetof(Vertex, texture_coords1) } },
          sizeof(Vertex),
          alignof(Vertex) },
        { "PushConstantRasterizer_std430",
          { { "model", offsetof(PushConstantRasterizer, model) },
            { "objectIndex", offsetof(PushConstantRasterizer, objectIndex) } },
          sizeof(PushConstantRasterizer),
          alignof(PushConstantRasterizer) },
        { "PushConstantPathTracing_std430",
          { { "clearColor", offsetof(PushConstantPathTracing, clearColor) },
            { "width", offsetof(PushConstantPathTracing, width) },
            { "height", offsetof(PushConstantPathTracing, height) },
            { "frame_index", offsetof(PushConstantPathTracing, frame_index) },
            { "samples_per_pixel", offsetof(PushConstantPathTracing, samples_per_pixel) },
            { "max_bounces", offsetof(PushConstantPathTracing, max_bounces) },
            { "furnace_metallic", offsetof(PushConstantPathTracing, furnace_metallic) },
            { "furnace_roughness", offsetof(PushConstantPathTracing, furnace_roughness) },
            { "trace_cloud_depth", offsetof(PushConstantPathTracing, trace_cloud_depth) },
            { "skybox_enabled", offsetof(PushConstantPathTracing, skybox_enabled) } },
          sizeof(PushConstantPathTracing),
          alignof(PushConstantPathTracing) },
        { "PushConstantPost_std430",
          { { "clouds_enabled", offsetof(PushConstantPost, clouds_enabled) },
            { "bloom_strength", offsetof(PushConstantPost, bloom_strength) } },
          sizeof(PushConstantPost),
          alignof(PushConstantPost) },
        // OxidANT's ssao.slang, which SsaoStage fills from SsaoUniforms.hpp.
        { "SsaoUniforms_std140",
          { { "proj", offsetof(Kataglyphis::VulkanRendererInternals::SsaoUniforms, proj) },
            { "inv_proj", offsetof(Kataglyphis::VulkanRendererInternals::SsaoUniforms, inv_proj) },
            { "params", offsetof(Kataglyphis::VulkanRendererInternals::SsaoUniforms, params) } },
          sizeof(Kataglyphis::VulkanRendererInternals::SsaoUniforms),
          alignof(Kataglyphis::VulkanRendererInternals::SsaoUniforms) },
        // OxidANT's fxaa.slang, which FxaaStage fills from FxaaUniforms.hpp.
        { "FxaaUniforms_std140",
          { { "params", offsetof(Kataglyphis::VulkanRendererInternals::FxaaUniforms, params) } },
          sizeof(Kataglyphis::VulkanRendererInternals::FxaaUniforms),
          alignof(Kataglyphis::VulkanRendererInternals::FxaaUniforms) },
        { "PushConstantRaytracing_std430",
          { { "clear_color", offsetof(PushConstantRaytracing, clear_color) } },
          sizeof(PushConstantRaytracing),
          alignof(PushConstantRaytracing) },
        { "ShadowPushConstants_std430",
          // The host's cascadeIndex carries the shader's objectIndex (makeShadowPush); what must agree is the offset.
          { { "model", offsetof(ShadowPushConstants, model) },
            { "objectIndex", offsetof(ShadowPushConstants, cascadeIndex) } },
          sizeof(ShadowPushConstants),
          alignof(ShadowPushConstants) },
    };
}

}// namespace

// The compiled SPIR-V is the layout truth: offsets, size, array strides and every emitted member must match the host.
TEST(BuildIntegrity, SharedStructOffsetsMatchTheCompiledSpirv)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path spirv_root = spirvRoot();
    ASSERT_TRUE(sourceExists(spirv_root)) << "missing " << spirv_root.string();

    const auto compiled = parse_spirv_struct_layouts(spirv_root);
    std::vector<std::string> not_found_in_any_spv;
    std::vector<std::string> mismatches;
    const auto round_up = [](std::size_t value, std::size_t alignment) {
        return (value + alignment - 1) / alignment * alignment;
    };

    for (const auto &contract : build_shared_struct_offset_contracts()) {
        const auto struct_it = compiled.find(contract.spirv_name);
        if (struct_it == compiled.end()) {
            not_found_in_any_spv.push_back(contract.spirv_name);
            continue;
        }
        const auto &layout = struct_it->second;
        const std::string &name = contract.spirv_name;

        for (const auto &[member_name, host_offset] : contract.member_offsets) {
            const auto member_it = layout.member_offsets.find(member_name);
            if (member_it == layout.member_offsets.end()) {
                mismatches.push_back(name + "." + member_name + ": not emitted as a member by any compiled .spv");
            } else if (member_it->second != host_offset) {
                mismatches.push_back(name + "." + member_name + ": compiled SPIR-V offset "
                                     + std::to_string(member_it->second) + " != host offsetof "
                                     + std::to_string(host_offset));
            }
        }
        for (const auto &[member_name, offset] : layout.member_offsets) {
            if (!contract.member_offsets.contains(member_name)) {
                mismatches.push_back(name + "." + member_name + " (offset " + std::to_string(offset)
                                     + "): emitted by the shader but missing from the contract");
            }
        }
        for (const auto size : layout.sizes) {
            if (round_up(size, contract.host_alignment) != contract.host_size) {
                mismatches.push_back(name + ": compiled size " + std::to_string(size) + " != host sizeof "
                                     + std::to_string(contract.host_size) + " (alignment "
                                     + std::to_string(contract.host_alignment) + ")");
            }
        }
        for (const auto stride : layout.element_strides) {
            if (stride != contract.host_size) {
                mismatches.push_back(name + ": ArrayStride " + std::to_string(stride)
                                     + " of an array of it != host sizeof " + std::to_string(contract.host_size));
            }
        }
        for (const auto &[member_name, strides] : layout.member_array_strides) {
            const auto host = contract.member_array_strides.find(member_name);
            if (host == contract.member_array_strides.end()) {
                mismatches.push_back(name + "." + member_name + ": an array without a contracted element size");
                continue;
            }
            for (const auto stride : strides) {
                if (stride != host->second) {
                    mismatches.push_back(name + "." + member_name + ": ArrayStride " + std::to_string(stride)
                                         + " != host element size " + std::to_string(host->second));
                }
            }
        }
    }

    EXPECT_TRUE(not_found_in_any_spv.empty())
      << not_found_in_any_spv.size() << " struct(s) expected in the compiled SPIR-V were not emitted by ANY .spv under "
      << spirv_root.string() << " - renamed or deleted shader struct: " << joinViolations(not_found_in_any_spv);

    EXPECT_TRUE(mismatches.empty()) << mismatches.size()
                                    << " host/SPIR-V layout mismatch(es):" << joinViolations(mismatches);
}

// Only C++ compiles the shared headers now, so the GLSL dual-compile shim (`#ifdef __cplusplus`, KTG_VEC*) is dead.
TEST(BuildIntegrity, NoHostDeviceHeaderCarriesTheRetiredGlslDualCompileShim)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (path.generic_string().find("/third_party/") != std::string::npos) { continue; }
        if (!it->is_regular_file(error)) { continue; }
        const auto extension = path.extension();
        if (extension != ".hpp" && extension != ".ixx") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        int line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find("__cplusplus") != std::string::npos || line.find("KTG_VEC") != std::string::npos) {
                violations.push_back(
                  fs::relative(path, repo_root).generic_string() + ':' + std::to_string(line_number) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size() << " line(s) under " << src_root.string()
      << " still carry the retired GLSL dual-compile shim (__cplusplus guard or KTG_VEC macro) - the GLSL tree "
         "these guarded against is gone and these headers are compiled only by C++: "
      << joinViolations(violations);
}

namespace {

// Member names from SceneUBO's C++ text, as an unread member never reaches SPIR-V; _pad filler is skipped.
std::vector<std::string> parse_scene_ubo_member_names(const fs::path &header_path)
{
    const auto struct_body = cpp_struct_body(readFileText(header_path).value_or(std::string{}), "struct SceneUBO");
    if (!struct_body) { return {}; }
    const std::string &body = *struct_body;
    const std::regex array_suffix(R"(\[[^\]]*\])");
    // A declaration ends in a bare identifier; anything else, like a wrapped static_assert message, is skipped.
    const std::regex identifier(R"(^[A-Za-z_]\w*$)");

    std::vector<std::string> names;
    std::istringstream body_stream(body);
    std::string line;
    while (std::getline(body_stream, line)) {
        const auto comment_pos = line.find("//");
        if (comment_pos != std::string::npos) { line = line.substr(0, comment_pos); }
        line = std::regex_replace(line, array_suffix, "");

        const auto semi_pos = line.find(';');
        if (semi_pos == std::string::npos) { continue; }

        std::istringstream decl_stream(line.substr(0, semi_pos));
        std::string token;
        std::string last_token;
        while (decl_stream >> token) { last_token = token; }

        if (!std::regex_match(last_token, identifier) || last_token.rfind("_pad", 0) == 0) { continue; }
        names.push_back(last_token);
    }
    return names;
}

}// namespace

// Every SceneUBO member, uploaded per image every frame, must be read by some shader.
TEST(BuildIntegrity, EverySceneUboFieldIsReadByAShader)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path header_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "SceneUBO.hpp";
    ASSERT_TRUE(sourceExists(header_path)) << "missing " << header_path.string();

    const std::vector<std::string> members = parse_scene_ubo_member_names(header_path);
    ASSERT_GT(members.size(), 5U) << "found only " << members.size() << " SceneUBO member(s) in "
                                  << header_path.string() << " - the parser itself is broken";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    std::string all_slang_text;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        if (!isReadableRegularFile(shader_path) || shader_path.extension() != ".slang") { continue; }
        all_slang_text += readFileText(shader_path).value_or(std::string{});
        all_slang_text += '\n';
    }

    std::vector<std::string> unread;
    for (const auto &member : members) {
        const std::regex word_boundary(R"(\b)" + member + R"(\b)");
        if (!std::regex_search(all_slang_text, word_boundary)) { unread.push_back(member); }
    }

    EXPECT_TRUE(unread.empty())
      << unread.size()
      << " SceneUBO member(s) are written by the host every frame but read by no Slang shader source under "
      << slang_root.string()
      << " - either wire the field into a shader or delete it (see the cloudMovementDirection removal for the "
         "precedent):"
      << joinViolations(unread);
}

// Every ObjMaterial member must be read as `material.<name>` somewhere, or it only costs bytes.
TEST(BuildIntegrity, EveryObjMaterialFieldIsReadByAShader)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path scene_types_path = repo_root / "Resources/ShadersSlang/common/scene_types.slang";
    ASSERT_TRUE(sourceExists(scene_types_path)) << "missing " << scene_types_path.string();

    const std::vector<std::string> members = parse_obj_material_member_names(scene_types_path);
    ASSERT_GT(members.size(), 3U) << "found only " << members.size() << " ObjMaterial member(s) in "
                                  << scene_types_path.string() << " - the parser itself is broken";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    std::string all_slang_text;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        if (!isReadableRegularFile(shader_path) || shader_path.extension() != ".slang") { continue; }
        all_slang_text += readFileText(shader_path).value_or(std::string{});
        all_slang_text += '\n';
    }

    std::vector<std::string> unread;
    for (const auto &member : members) {
        const std::regex material_dot_field(R"(material\.)" + member + R"(\b)");
        if (!std::regex_search(all_slang_text, material_dot_field)) { unread.push_back(member); }
    }

    EXPECT_TRUE(unread.empty())
      << unread.size()
      << " ObjMaterial member(s) are uploaded per material but read by no Slang shader source (as "
         "material.<name>) under "
      << slang_root.string()
      << " - either wire the field into a shader or delete it (see the ambient/specular/transmittance/ior/illum "
         "removal for the precedent):"
      << joinViolations(unread);
}

// A .w that packs real data rather than a literal filler must be read as `<field>.w` by some shader.
TEST(BuildIntegrity, SceneUboWComponentsCarryingDataAreReadByAShader)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path marshal_path = repo_root / "Src" / "GraphicsEngineVulkan" / "common" / "SceneUboMarshal.hpp";
    const auto marshal_source = readFileText(marshal_path);
    ASSERT_TRUE(marshal_source.has_value()) << "missing " << marshal_path.string();

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();
    std::string all_slang_text;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        if (!isReadableRegularFile(shader_path) || shader_path.extension() != ".slang") { continue; }
        all_slang_text += readFileText(shader_path).value_or(std::string{});
        all_slang_text += '\n';
    }

    static const std::regex kFloatLiteral(R"(^-?[0-9]+\.[0-9]+[fF]?$)");
    static const std::array<const char *, 4> kWBearingFields{
        "view_dir", R"(dirLight\.direction)", R"(dirLight\.color)", "cam_pos"
    };

    bool matchedRadianceField = false;
    for (const char *field : kWBearingFields) {
        const std::regex assignRegex(
          std::string("ubo\\.") + field
          + R"(\s*=\s*glm::vec4\([^;]*?,\s*([A-Za-z_][A-Za-z0-9_]*|-?[0-9]+\.[0-9]+[fF]?)\s*\)\s*;)");
        std::smatch match;
        ASSERT_TRUE(std::regex_search(*marshal_source, match, assignRegex))
          << marshal_path.string() << " no longer assigns ubo." << field
          << " as glm::vec4(xyz, w) - update this parser to match the new packing shape";

        const std::string wArg = match[1].str();
        if (std::regex_match(wArg, kFloatLiteral)) { continue; }// filler constant, nothing to check

        if (std::string(field) == R"(dirLight\.color)") { matchedRadianceField = true; }

        const std::regex wRead(field + std::string(R"(\.w\b)"));
        EXPECT_TRUE(std::regex_search(all_slang_text, wRead))
          << "SceneUboMarshal.hpp packs a non-constant value (" << wArg << ") into ubo." << field
          << ".w, but no .slang source under " << slang_root.string() << " reads " << field
          << ".w - either wire it into a shader or write a constant filler instead";
    }

    EXPECT_TRUE(matchedRadianceField)
      << "expected dirLight.color's .w assignment (radiance) to be the one non-constant SceneUBO "
         ".w slot this test verifies - if that packing moved, this sanity check needs updating too";
}
