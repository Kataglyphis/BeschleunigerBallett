// Build integrity: texture slots: one owner per sample and UV row, non-uniform indices, fitted images, samplers.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// Raw emissive, normal and metallic-roughness samples belong to material_textures.slang, so no path re-wraps them.
TEST(BuildIntegrity, TextureSlotWrappersHaveOneOwner)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    static const std::array<const char *, 6> kRawSamplers = { "sample_emissive_lod0(",
        "sample_emissive(",
        "sample_normal_lod0(",
        "sample_normal(",
        "sample_metallic_roughness_lod0(",
        "sample_metallic_roughness(" };
    static const std::string kOwningFile = "common/material_textures.slang";

    std::vector<std::string> violations;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }

        const std::string relative_path = shaderRelativePath(path, slang_root).generic_string();
        if (relative_path.starts_with("build/")) { continue; }
        if (relative_path == kOwningFile) { continue; }

        const auto textOpt = readFileText(path);
        ASSERT_TRUE(textOpt.has_value()) << "could not open " << relative_path;
        const std::string &text = *textOpt;

        for (const char *raw_sampler : kRawSamplers) {
            if (text.find(raw_sampler) == std::string::npos) { continue; }
            violations.push_back(relative_path + " calls " + raw_sampler + " directly - only " + kOwningFile
                                  + " may call the raw sampler; every shading path must go through "
                                    "resolved_emission/resolved_metallic_roughness/resolved_normal "
                                    "(and their _lod0 twins) instead");
        }
    }

    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " raw texture-slot sampler call(s) found outside their owning module:"
                                    << joinViolations(violations);
}

// All four alpha-testing sites must fold the map_d texture into the alpha they test.
TEST(BuildIntegrity, AlphaTextureIsSampledByEveryAlphaTestingPath)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "material_rules.slang's material_masked_out() is the sole place the MASK cutoff test happens; a site that stops "
      "sampling ObjMaterial::alphaTextureID silently drops map_d cut-outs (the chain and hanging-plant geometry in "
      "the bundled crytek-sponza render as solid rectangles) instead of alpha-testing them";

    const std::vector<fs::path> alpha_testing_paths = {
        repo_root / "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        repo_root / "Resources/ShadersSlang/deferred/deferred.slang",
        repo_root / "Resources/ShadersSlang/rasterizer/shadows/shadow_map.slang",
        repo_root / "Resources/ShadersSlang/common/alpha_test.slang",
    };
    for (const fs::path &path : alpha_testing_paths) {
        const auto text_opt = readFileText(path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
        const std::string &text = *text_opt;
        EXPECT_NE(text.find("alphaTextureID"), std::string::npos)
          << path.string() << " no longer branches on ObjMaterial::alphaTextureID. " << kFailureMessage;
        EXPECT_NE(text.find("material_masked_out("), std::string::npos)
          << path.string() << " no longer calls material_masked_out(). " << kFailureMessage;
    }
}

// Transforms are per slot, and the bare transform_uv overload applies the base-colour rows to any slot it samples.
TEST(BuildIntegrity, NonBaseTextureSlotsUseTheirOwnUvTransformRows)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "KHR_texture_transform is per texture slot; a non-base sample calling the bare transform_uv(uv, material) "
      "overload (the base-colour pair) instead of its own slot's named accessor stamps the base-colour transform "
      "onto a slot that may have a different transform, or none at all";

    const fs::path path = repo_root / "Resources/ShadersSlang/common/material_textures.slang";
    const auto text_opt = readFileText(path);
    ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
    const std::string &text = *text_opt;

    static const std::pair<const char *, std::size_t> kExpectedCounts[] = {
        { "normal_uv(", 2U },
        { "metallic_roughness_uv(", 2U },
        { "emissive_uv(", 2U },
        { "alpha_uv(", 2U },
    };
    for (const auto &[accessor, expected_count] : kExpectedCounts) {
        std::size_t count = 0;
        std::size_t pos = 0;
        while ((pos = text.find(accessor, pos)) != std::string::npos) {
            ++count;
            pos += std::strlen(accessor);
        }
        EXPECT_EQ(count, expected_count)
          << path.string() << " must call " << accessor
          << ") exactly once from its explicit-LOD helper and once from its implicit-LOD helper. " << kFailureMessage;
    }

    // Even map_d, which shares the base-colour rows, names that through alpha_uv().
    std::size_t bareTransformUvCalls = 0;
    std::size_t pos = 0;
    while ((pos = text.find("transform_uv(", pos)) != std::string::npos) {
        ++bareTransformUvCalls;
        pos += std::strlen("transform_uv(");
    }
    EXPECT_EQ(bareTransformUvCalls, 0U)
      << path.string()
      << " must not call the bare transform_uv(uv, material) overload; every slot, map_d's alpha_uv() included, "
         "goes through its own named accessor. "
      << kFailureMessage;
}

// material_textures.slang alone declares texture bindings and samples the four non-base slots; base colour is inline.
TEST(BuildIntegrity, TextureSlotSamplingHasOneOwner)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    static const char *kTexturesBinding = "[vk::binding(TEXTURES_BINDING, 0)]";
    static const char *kSamplersBinding = "[vk::binding(SAMPLER_BINDING, 0)]";

    const fs::path owner_path = slang_root / "common/material_textures.slang";
    const auto owner_text_opt = readFileText(owner_path);
    ASSERT_TRUE(owner_text_opt.has_value()) << "could not open " << owner_path.string();
    EXPECT_NE(owner_text_opt->find(kTexturesBinding), std::string::npos)
      << owner_path.string() << " no longer declares the textures[] binding";
    EXPECT_NE(owner_text_opt->find(kSamplersBinding), std::string::npos)
      << owner_path.string() << " no longer declares the textureSamplers[] binding";

    // An explicit list: a directory walk would have to excuse this file's and material_textures.slang's own mentions.
    static const std::vector<fs::path> kConsumers = {
        slang_root / "rasterizer/rasterizer.slang",
        slang_root / "deferred/deferred.slang",
        slang_root / "rasterizer/shadows/shadow_map.slang",
        slang_root / "common/traced_hit.slang",
        slang_root / "common/alpha_test.slang",
    };

    // The base-colour overload of resolve_texture_slot stays inline at each path, so it is not listed.
    static const std::vector<std::string> kSlotFields = {
        "material.alphaTextureID",
        "material.normalTextureID",
        "material.metallicRoughnessTextureID",
        "material.emissiveTextureID",
    };

    std::vector<std::string> violations;
    for (const auto &path : kConsumers) {
        const auto text_opt = readFileText(path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
        const std::string &text = *text_opt;
        const std::string relative = fs::relative(path, repo_root).generic_string();

        if (text.find(kTexturesBinding) != std::string::npos) {
            violations.push_back(relative
                                  + " redeclares the textures[] binding - common/material_textures.slang is the "
                                    "sole owner");
        }
        if (text.find(kSamplersBinding) != std::string::npos) {
            violations.push_back(relative
                                  + " redeclares the textureSamplers[] binding - common/material_textures.slang is "
                                    "the sole owner");
        }
        for (const auto &field : kSlotFields) {
            if (text.find("resolve_texture_slot(obj, " + field + ")") != std::string::npos) {
                violations.push_back(relative + " still hand-rolls resolve_texture_slot(obj, " + field
                                     + ") inline - call common/material_textures.slang's helper instead");
            }
        }
    }

    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " texture-slot sampling ownership violation(s):" << joinViolations(violations);
}

// Triangles, hits and bounces pick their own slot; without NonUniform that is undefined (VUID-RuntimeSpirv-NonUniform-06274).
TEST(BuildIntegrity, EveryMaterialTextureIndexIsNonUniform)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    // The one wrapper: NonUniformResourceIndex everywhere but in the uniform-index twins.
    const auto owner = readFileText(slang_root / "common" / "material_textures.slang");
    ASSERT_TRUE(owner.has_value()) << "could not open common/material_textures.slang";
    const std::string helper = extract_function_body(*owner, "int material_texture_index(int slot)");
    EXPECT_NE(helper.find("kUniformTextureIndex ? slot : NonUniformResourceIndex(slot)"), std::string::npos)
      << "material_texture_index no longer returns NonUniformResourceIndex(slot) unless kUniformTextureIndex is set";

    static const std::regex kIndex(R"(\b(textures|textureSamplers)\[([^\]]*)\])");
    std::vector<std::string> violations;
    std::size_t wrapped = 0;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        // The Vulkan tree only: OxidANT's shared sources also emit WGSL, where slangc refuses NonUniformResourceIndex.
        const fs::path relative = shader_path.lexically_normal().lexically_relative(slang_root.lexically_normal());
        if (relative.empty() || *relative.begin() == ".." || *relative.begin() == "build") { continue; }
        const std::string relative_path = relative.generic_string();
        const auto lines = readFileLines(shader_path);
        if (!lines) { continue; }
        for (std::size_t index = 0; index < lines->size(); ++index) {
            const std::string &line = (*lines)[index];
            for (auto it = std::sregex_iterator(line.begin(), line.end(), kIndex); it != std::sregex_iterator(); ++it) {
                const std::string subscript = (*it)[2].str();
                if (subscript == "MAX_TEXTURE_COUNT") { continue; }
                if (subscript.starts_with("material_texture_index(")) {
                    ++wrapped;
                    continue;
                }
                violations.push_back(relative_path + ":" + std::to_string(index + 1) + ": " + (*it)[0].str());
            }
        }
    }

    // Control: the 16 sample sites index both arrays, so a renamed array cannot pass this by matching nothing.
    EXPECT_GE(wrapped, 32U) << "found only " << wrapped << " material_texture_index-wrapped texture indices";
    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " textures[]/textureSamplers[] index(es) without material_texture_index:"
                                    << joinViolations(violations);
}

// A device without non-uniform indexing loads the *_uniform_index twins: each its base under the define, per pipeline.
TEST(BuildIntegrity, UniformIndexTwinsWrapTheirBaseAndEveryMaterialPipelinePicksOne)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path slang_root = slangRoot();
    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    // Line pairs, not one string: a CRLF checkout must read the same.
    const auto has_line_pair = [](const fs::path &path, const std::string &first, const std::string &second) {
        const auto lines = readFileLines(path);
        if (!lines) { return false; }
        for (std::size_t index = 0; index + 1 < lines->size(); ++index) {
            if ((*lines)[index] == first && (*lines)[index + 1] == second) { return true; }
        }
        return false;
    };

    static const std::string kTwinSuffix = "_uniform_index.slang";
    std::vector<std::string> violations;
    std::set<std::string> twinned;// "<base stem>.<entry>", the file names the C++ loads
    for (const auto &[file, entry] : manifest->spirv_entries) {
        if (!file.ends_with(kTwinSuffix)) { continue; }
        const std::string base_file = file.substr(0, file.size() - kTwinSuffix.size()) + ".slang";
        const fs::path base(base_file);
        twinned.insert(base.stem().string() + "." + entry);
        if (!manifest->spirv_entries.contains({ base_file, entry })) {
            violations.push_back(file + " " + entry + ": " + base_file + " has no row with that entry");
        }
        if (!has_line_pair(slang_root / file,
              "#define KATAGLYPHIS_UNIFORM_TEXTURE_INDEX",
              "#include \"" + base.filename().string() + "\"")) {
            violations.push_back(file + ": does not define KATAGLYPHIS_UNIFORM_TEXTURE_INDEX and then include "
                                 + base.filename().string());
        }
        if (!has_line_pair(slang_root / base_file,
              "#if defined(KATAGLYPHIS_UNIFORM_TEXTURE_INDEX)",
              "export static const bool kUniformTextureIndex = true;")) {
            violations.push_back(base_file + ": does not export kUniformTextureIndex = true under the define, so "
                                 + file + " compiles the base unchanged");
        }
    }
    // Control: forward's two fragment entries, the G-buffer's and the shadow casters'.
    ASSERT_GE(twinned.size(), 4U) << "found only " << twinned.size() << " *_uniform_index rows in shader-manifest.json";

    // A bare `= true` drops NonUniform from a module every device loads, the traced ones included.
    std::size_t exports = 0;
    std::error_code source_error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, source_error)) {
        if (source_error) { break; }
        const auto lines = readFileLines(shader_path);
        if (!lines) { continue; }
        for (std::size_t index = 0; index < lines->size(); ++index) {
            if ((*lines)[index].find("kUniformTextureIndex = ") == std::string::npos) { continue; }
            ++exports;
            const bool guarded = index > 0 && (*lines)[index - 1] == "#if defined(KATAGLYPHIS_UNIFORM_TEXTURE_INDEX)";
            if ((*lines)[index].find("kUniformTextureIndex = true;") != std::string::npos && !guarded) {
                violations.push_back(fs::relative(shader_path, slang_root, source_error).generic_string() + ":"
                                     + std::to_string(index + 1)
                                     + ": exports kUniformTextureIndex = true outside the twins' #if");
            }
        }
    }
    // Control: rasterizer, deferred and shadow_map export both values; closest-hit, any-hit and the path tracer false.
    EXPECT_GE(exports, 9U) << "found only " << exports << " kUniformTextureIndex export(s) under "
                           << slang_root.string();

    // A twinned literal outside materialShaderPath( loads the non-uniform module on every device.
    static const std::regex kSpvLiteral(R"re("(?:[^"]*/)?([A-Za-z0-9_.]+)\.spv")re");
    static const std::set<std::string> kSourceExtensions = { ".cpp", ".ixx", ".hpp", ".h" };
    static const std::string kPickCall = "materialShaderPath(";
    std::size_t routed = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(repo_root / "Src", error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || !kSourceExtensions.contains(it->path().extension().string())) { continue; }
        const auto lines = readFileLines(it->path());
        if (!lines) { continue; }
        const std::string relative = fs::relative(it->path(), repo_root).generic_string();
        for (std::size_t index = 0; index < lines->size(); ++index) {
            const std::string line = strip_line_comment((*lines)[index]);
            for (auto match = std::sregex_iterator(line.begin(), line.end(), kSpvLiteral);
              match != std::sregex_iterator();
              ++match) {
                const std::string name = (*match)[1].str();
                // Picked only as the call's first argument: nothing between its paren and the literal ends it.
                const std::string before = line.substr(0, static_cast<std::size_t>(match->position()));
                const std::size_t call = before.rfind(kPickCall);
                const bool picked = call != std::string::npos
                                    && before.find_first_of(",()", call + kPickCall.size()) == std::string::npos;
                const std::string where = relative + ":" + std::to_string(index + 1) + ": " + name + ".spv";
                if (picked) {
                    ++routed;
                    if (!twinned.contains(name)) {
                        violations.push_back(where + " goes through materialShaderPath but has no twin row");
                    }
                } else if (twinned.contains(name)) {
                    violations.push_back(where + " is loaded without materialShaderPath");
                }
            }
        }
    }
    // Control: forward, transparent, prepass, G-buffer, cascades and punctual cubes.
    EXPECT_GE(routed, 6U) << "found only " << routed << " materialShaderPath( call(s) under Src/";

    const auto device = readFileText(repo_root / "Src/GraphicsEngineVulkan/vulkan_base/VulkanDevice.cpp");
    ASSERT_TRUE(device.has_value()) << "could not open VulkanDevice.cpp";
    EXPECT_NE(
      device->find(
        "materialTextureIndexingFor(available_features2.features, available_features12, forceUniformTextureIndex);"),
      std::string::npos)
      << "VulkanDevice.cpp no longer picks the material shaders from the queried sampled-image indexing features";
    EXPECT_NE(device->find("materialTextureIndexingLogLine(deviceMaterialTextureIndexing)"), std::string::npos)
      << "VulkanDevice.cpp no longer logs which material shaders it picked";

    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " uniform-index twin violation(s):" << joinViolations(violations);
}

// Indexing a sampled-image array by a variable is valid only with shaderSampledImageArrayDynamicIndexing enabled.
TEST(BuildIntegrity, TheDeviceRequestsSampledImageArrayDynamicIndexing)
{
    const fs::path device_path = repoRoot() / "Src/GraphicsEngineVulkan/vulkan_base/VulkanDevice.cpp";
    const auto text = readFileText(device_path);
    ASSERT_TRUE(text.has_value()) << "could not open " << device_path.string();
    static const std::regex kRequest(
      R"(features2\.features\.shaderSampledImageArrayDynamicIndexing\s*=\s*available_features2\.features\.shaderSampledImageArrayDynamicIndexing)");
    EXPECT_TRUE(std::regex_search(*text, kRequest))
      << device_path.string()
      << " no longer enables shaderSampledImageArrayDynamicIndexing where the device offers it, though every material "
         "texture is sampled through a variable index";
}

// TextureLoadUnit and SkyBoxUnit test the fit helpers alone; past the limit only these calls keep vkCheck from aborting.
TEST(BuildIntegrity, OversizedTexturesAreFittedBeforeTheirImageIsCreated)
{
    struct UploadSite
    {
        const char *file;
        const char *function;
        std::vector<std::string> inOrder;
    };
    const std::array<UploadSite, 3> sites{ {
      { "Src/GraphicsEngineVulkan/scene/Texture.cpp",
        "Kataglyphis::Texture::uploadRgba",
        { "getMaxImageDimension2D()", "fitRgba8ToLimit(", "mipLevelCount(", "uploadLevels(" } },
      { "Src/GraphicsEngineVulkan/scene/Texture.cpp",
        "Kataglyphis::Texture::createFromLevels",
        { "getMaxImageDimension2D()", "uploadLevels(" } },
      { "Src/GraphicsEngineVulkan/scene/sky_box/SkyBox.cpp",
        "SkyBox::loadCubeMap",
        { "getMaxImageDimensionCube()", "fitCubemapFacesToLimit(", "uploadCubeMapFaces(" } },
    } };
    for (const UploadSite &site : sites) {
        const fs::path path = repoRoot() / site.file;
        const auto text = readFileText(path);
        ASSERT_TRUE(text.has_value()) << "could not open " << path.string();
        const auto span = function_body_span(*text, site.function);
        ASSERT_TRUE(span.has_value()) << path.string() << " no longer defines " << site.function;
        const std::string body = text->substr(span->first, span->second - span->first);
        std::size_t previous = 0;
        for (const std::string &needle : site.inOrder) {
            const std::size_t at = body.find(needle);
            ASSERT_NE(at, std::string::npos) << site.function << " no longer calls " << needle;
            EXPECT_GE(at, previous)
              << site.function << " reaches " << needle
              << " out of order: read the device limit, fit to it, then size and create the image";
            previous = at;
        }
    }
}

// Row members only inside their accessors and structs: direct use can pair one slot's row0 with another's row1.
TEST(BuildIntegrity, PerSlotUvTransformRowsAreSpelledInExactlyOnePlace)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = repo_root / "Resources/ShadersSlang";
    ASSERT_TRUE(sourceExists(slang_root)) << "could not find " << slang_root.string();

    static const std::regex kRowPattern(
      R"((normal|metallic_roughness|emissive)_uv_transform_row[01]|(base|mr|normal|emissive|occlusion)_uv_row[01])");
    static const std::set<fs::path> kAllowedFiles = {
        repo_root / "Resources/ShadersSlang/common/base_color.slang",
        repo_root / "Resources/ShadersSlang/common/scene_types.slang",
    };
    const fs::path forward_slang_path = repo_root / "Resources/ShadersSlang/forward/forward.slang";
    static const std::regex kForwardAllowedBlockStart(
      R"(^(struct PrimUniforms|float2 (base_color_uv_select|base_color_uv_rows|base_color_uv|metallic_roughness_uv|normal_uv|emissive_uv|occlusion_uv)\())");

    std::vector<std::string> violations;
    std::error_code source_error;
    for (const auto &physical : shaderSourceFiles(slang_root, source_error)) {
        const auto logical = slang_root / shaderRelativePath(physical, slang_root);
        const auto &entry_path = logical;
        if (!isReadableRegularFile(entry_path) || entry_path.extension() != ".slang") { continue; }
        if (kAllowedFiles.count(entry_path) != 0) { continue; }

        const auto text_opt = readFileText(entry_path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << entry_path.string();
        const std::string &text = *text_opt;
        const bool isForwardSlang = entry_path == forward_slang_path;

        std::istringstream stream(text);
        std::string line;
        int lineNumber = 0;
        bool inAllowedBlock = false;
        int allowedBlockBraceDepth = 0;
        while (std::getline(stream, line)) {
            ++lineNumber;

            if (isForwardSlang) {
                if (!inAllowedBlock && std::regex_search(line, kForwardAllowedBlockStart)) {
                    inAllowedBlock = true;
                    allowedBlockBraceDepth = 0;
                }
                if (inAllowedBlock) {
                    for (char c : line) {
                        if (c == '{') {
                            ++allowedBlockBraceDepth;
                        } else if (c == '}') {
                            --allowedBlockBraceDepth;
                            if (allowedBlockBraceDepth <= 0) { inAllowedBlock = false; }
                        }
                    }
                    continue;// inside an accessor body or the PrimUniforms declaration - allowed
                }
            }

            if (std::regex_search(line, kRowPattern)) {
                violations.push_back(
                  fs::relative(entry_path, repo_root).string() + ":" + std::to_string(lineNumber) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << "found a per-slot UV-transform row member named outside base_color.slang's/forward.slang's accessors and "
         "scene_types.slang's/forward.slang's struct declarations - call normal_uv()/metallic_roughness_uv()/"
         "emissive_uv() (or forward.slang's base_color_uv()/metallic_roughness_uv()/normal_uv()/emissive_uv()/"
         "occlusion_uv()) instead:\n"
      << joinViolations(violations, "", "\n");
}

// One uv-set mask read per slot accessor, so a hand-rolled selection cannot diverge from their bit assignment.
TEST(BuildIntegrity, ForwardShaderUvSetMaskHasOneOwnerPerSlot)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path forward_slang = repo_root / "Resources/ShadersSlang/forward/forward.slang";
    ASSERT_TRUE(sourceExists(forward_slang)) << "could not find " << forward_slang.string();

    const auto text_opt = readFileText(forward_slang);
    ASSERT_TRUE(text_opt.has_value()) << "could not open " << forward_slang.string();

    static const std::regex kMaskReadPattern(R"(prim\.material_flags\.y)");
    static constexpr int kSlotCount = 5;// base_color, metallic_roughness, normal, emissive, occlusion

    const auto matchCount =
      std::distance(std::sregex_iterator(text_opt->begin(), text_opt->end(), kMaskReadPattern), std::sregex_iterator());

    EXPECT_LE(matchCount, kSlotCount) << "forward.slang reads prim.material_flags.y in " << matchCount
                                      << " place(s), more than the " << kSlotCount
                                      << " per-slot UV accessors - a call site may be hand-rolling the uv-set mask "
                                         "instead of calling the accessor";
}

// Every shading path must swizzle metallic-roughness through material_rules.slang (G = roughness, B = metallic).
TEST(BuildIntegrity, MetallicRoughnessTextureIsSampledByEveryShadingPath)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "material_metallic_roughness() in common/material_rules.slang is the sole place the glTF G=roughness/B=metallic "
      "channel swizzle happens; a shading path that hand-rolls it instead risks silently diverging from the others";

    const fs::path material_rules_path = repo_root / "Resources/ShadersSlang/common/material_rules.slang";
    const auto material_rules_text_opt = readFileText(material_rules_path);
    ASSERT_TRUE(material_rules_text_opt.has_value()) << "could not open " << material_rules_path.string();
    const std::string &material_rules_text = *material_rules_text_opt;
    EXPECT_NE(material_rules_text.find("material_metallic(material.metallic, mrSample.b)"), std::string::npos)
      << "material_rules.slang's material_metallic_roughness() no longer hands gltf_material.slang the sampled B "
         "channel and the metallic factor. "
      << kFailureMessage;
    EXPECT_NE(material_rules_text.find("material_roughness(roughness_factor(material), mrSample.g)"), std::string::npos)
      << "material_rules.slang's material_metallic_roughness() no longer hands gltf_material.slang the sampled G "
         "channel and the roughness factor. "
      << kFailureMessage;

    // Every path, path_tracing.slang included, resolves the untextured factor through the same helper.
    const fs::path material_textures_path = repo_root / "Resources/ShadersSlang/common/material_textures.slang";
    const auto material_textures_text_opt = readFileText(material_textures_path);
    ASSERT_TRUE(material_textures_text_opt.has_value()) << "could not open " << material_textures_path.string();
    const std::string &material_textures_text = *material_textures_text_opt;
    EXPECT_NE(material_textures_text.find("material_metallic_roughness("), std::string::npos)
      << "material_textures.slang no longer calls material_metallic_roughness(). " << kFailureMessage;
    EXPECT_NE(material_textures_text.find("material.metallicRoughnessTextureID"), std::string::npos)
      << "material_textures.slang's resolved_metallic_roughness helpers no longer branch on "
         "ObjMaterial::metallicRoughnessTextureID. "
      << kFailureMessage;

    const std::vector<fs::path> brdf_shading_paths = {
        repo_root / "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        repo_root / "Resources/ShadersSlang/deferred/deferred.slang",
        repo_root / "Resources/ShadersSlang/common/traced_hit.slang",
    };
    for (const fs::path &path : brdf_shading_paths) {
        const auto text_opt = readFileText(path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
        const std::string &text = *text_opt;
        EXPECT_NE(text.find("import material_textures;"), std::string::npos)
          << path.string() << " no longer imports common/material_textures.slang. " << kFailureMessage;
        // A texture-gated read drops the untextured factor: the path tracer once ignored metallic_card's 0.75.
        const bool calls_resolved = text.find("resolved_metallic_roughness(") != std::string::npos
                                    || text.find("resolved_metallic_roughness_lod0(") != std::string::npos;
        EXPECT_TRUE(calls_resolved)
          << path.string() << " no longer calls resolved_metallic_roughness()/resolved_metallic_roughness_lod0(). "
          << kFailureMessage;
        EXPECT_EQ(text.find("mrSample.g"), std::string::npos)
          << path.string()
          << " hand-rolls the roughness (G) channel read instead of going through "
             "material_metallic_roughness(). "
          << kFailureMessage;
        EXPECT_EQ(text.find("mrSample.b"), std::string::npos)
          << path.string()
          << " hand-rolls the metallic (B) channel read instead of going through "
             "material_metallic_roughness(). "
          << kFailureMessage;
    }
}

// resolve_texture_slot() is the texture-slot clamp's one definition; a local copy could drift from it.
TEST(BuildIntegrity, TextureSlotClampHasOneDefinition)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    static const std::string kClampLiteral = "MAX_TEXTURE_COUNT - 1)";
    const fs::path scene_types_relative = fs::path("common") / "scene_types.slang";

    std::vector<std::string> other_definitions;
    bool found_in_scene_types = false;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }
        const std::string relative_path = shaderRelativePath(path, slang_root).generic_string();
        if (relative_path.starts_with("build/")) { continue; }

        const auto text = readFileText(path);
        if (!text.has_value() || text->find(kClampLiteral) == std::string::npos) { continue; }

        if (relative_path == scene_types_relative.generic_string()) {
            found_in_scene_types = true;
        } else {
            other_definitions.push_back(relative_path);
        }
    }

    EXPECT_TRUE(found_in_scene_types) << kClampLiteral << " not found in " << scene_types_relative.generic_string()
                                      << " - resolve_texture_slot() moved or was rewritten?";
    EXPECT_TRUE(other_definitions.empty())
      << other_definitions.size() << " .slang file(s) besides scene_types.slang re-derive the clamp instead of "
      << "calling resolve_texture_slot(): " << joinViolations(other_definitions);
}

// createImage must record mip_levels itself, or getMipLevel() and maxLod disagree; text-checked, as it needs a device.
TEST(BuildIntegrity, TextureCreateImageRecordsTheMipLevelItWasGiven)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path texture_cpp = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "Texture.cpp";
    const auto lines = readFileLines(texture_cpp);
    ASSERT_TRUE(lines.has_value()) << "could not open " << texture_cpp.string();

    static const std::string kFunctionSignature = "Kataglyphis::Texture::createImage(";
    static const std::string kAssignment = "mip_levels = in_mip_levels;";

    bool in_function = false;
    bool function_started = false;// seen the opening '{' of the definition body
    int brace_depth = 0;
    bool found_assignment = false;
    for (const auto &line : *lines) {
        if (!in_function) {
            if (line.find(kFunctionSignature) == std::string::npos) { continue; }
            in_function = true;
            continue;
        }

        brace_depth += static_cast<int>(std::count(line.begin(), line.end(), '{'));
        brace_depth -= static_cast<int>(std::count(line.begin(), line.end(), '}'));
        if (brace_depth > 0) { function_started = true; }
        if (line.find(kAssignment) != std::string::npos) { found_assignment = true; }
        if (function_started && brace_depth <= 0) { break; }// reached the closing '}'
    }

    ASSERT_TRUE(in_function) << texture_cpp.string() << " has no " << kFunctionSignature
                             << " definition to check - did createImage move or get renamed?";
    EXPECT_TRUE(found_assignment)
      << "Texture::createImage no longer assigns mip_levels from in_mip_levels - getMipLevel() and the "
         "sampler's maxLod would go back to depending on which constructor path ran.";
}

// VUID-VkSamplerCreateInfo-anisotropyEnable-01071: anisotropy via resolveMaxAnisotropy, queried in VulkanDevice.cpp.
TEST(BuildIntegrity, NoSamplerHardCodesItsMaxAnisotropy)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    static const std::string kSignature = "buildSamplerCreateInfo(";
    // A literal above 1; "1.0F" or "1.0f" is the disabled-anisotropy sentinel.
    static const std::regex kLiteralAboveOne(R"(^\s*(?:[2-9]|\d{2,})(?:\.\d+)?[fF]?\s*$)");
    static const char *const kGetFeatures = "getFeatures";

    std::vector<std::string> anisotropy_violations;
    std::vector<std::string> get_features_violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx" && path.extension() != ".hpp") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();

        const auto contentsOpt = readFileText(path);
        ASSERT_TRUE(contentsOpt.has_value()) << "could not open " << path.string();
        const std::string &contents = *contentsOpt;

        std::size_t sig_pos = 0;
        while ((sig_pos = contents.find(kSignature, sig_pos)) != std::string::npos) {
            const std::size_t args_begin = sig_pos + kSignature.size();

            // Balanced parens, not a lazy regex, which runs past a declaration's parameter list into unrelated code.
            std::size_t pos = args_begin;
            int paren_depth = 1;
            while (pos < contents.size() && paren_depth > 0) {
                if (contents[pos] == '(') {
                    ++paren_depth;
                } else if (contents[pos] == ')') {
                    --paren_depth;
                }
                ++pos;
            }
            sig_pos = pos;
            ASSERT_EQ(paren_depth, 0)
              << relative_file << ": unbalanced parentheses scanning a buildSamplerCreateInfo(...) argument list";

            const std::string call_args = contents.substr(args_begin, pos - 1 - args_begin);

            // Split on top-level commas only: maxAnisotropy is itself a nested call with its own comma.
            std::vector<std::string> args;
            std::size_t arg_start = 0;
            int depth = 0;
            for (std::size_t i = 0; i < call_args.size(); ++i) {
                const char character = call_args[i];
                if (character == '(') {
                    ++depth;
                } else if (character == ')') {
                    --depth;
                } else if (character == ',' && depth == 0) {
                    args.push_back(call_args.substr(arg_start, i - arg_start));
                    arg_start = i + 1;
                }
            }
            args.push_back(call_args.substr(arg_start));
            ASSERT_GE(args.size(), 5u)
              << relative_file
              << ": buildSamplerCreateInfo(...) does not have the expected (filter, addressMode, maxLod, "
                 "anisotropyEnable, maxAnisotropy, ...) shape - the scan needs updating:\n"
              << call_args;

            if (std::regex_match(args[4], kLiteralAboveOne)) {
                anisotropy_violations.push_back(relative_file + ": maxAnisotropy=" + args[4]);
            }
        }

        if (relative_file == "Src/GraphicsEngineVulkan/vulkan_base/VulkanDevice.cpp") { continue; }
        std::size_t pos = 0;
        while ((pos = contents.find(kGetFeatures, pos)) != std::string::npos) {
            get_features_violations.push_back(relative_file);
            pos += std::strlen(kGetFeatures);
        }
    }

    EXPECT_TRUE(anisotropy_violations.empty())
      << "buildSamplerCreateInfo call(s) hard-code a maxAnisotropy literal above the disabled sentinel - route "
         "through Kataglyphis::resolveMaxAnisotropy(anisotropyEnabled, device->maxSamplerAnisotropy()) instead:"
      << joinViolations(anisotropy_violations);

    EXPECT_TRUE(get_features_violations.empty())
      << "getFeatures()/getFeatures2() called outside VulkanDevice.cpp - device capability queries belong in one "
         "place so availability and the features actually enabled cannot diverge:"
      << joinViolations(get_features_violations);
}
