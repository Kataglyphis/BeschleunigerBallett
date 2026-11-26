// Build integrity: the clouds' dispatch, march steps, noise volume, queue and sampling match the shaders.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"
#include "common/SceneUboMarshal.hpp"
#include "scene/atmospheric_effects/clouds/CloudDispatch.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// A local copy of the march would let the two renderers' clouds drift apart again unseen.
TEST(BuildIntegrity, CppCloudsMarchThroughTheSharedModule)
{
    const fs::path clouds_path = slangRoot() / "compute" / "clouds.slang";
    const auto contents = readFileText(clouds_path);
    ASSERT_TRUE(contents.has_value()) << "missing " << clouds_path.string();
    ASSERT_TRUE(readFileText(sharedCloudsPath()).has_value()) << "missing " << sharedCloudsPath().string();

    static const std::regex kImport(R"((^|\n)\s*import\s+volumetric_clouds\s*;)");
    EXPECT_TRUE(std::regex_search(*contents, kImport)) << clouds_path.string() << " must import volumetric_clouds";
    EXPECT_NE(contents->find("march_clouds("), std::string::npos)
      << clouds_path.string() << " must march through volumetric_clouds.slang's march_clouds";
    EXPECT_NE(contents->find("cloud_unpack("), std::string::npos)
      << clouds_path.string() << " must unpack the SceneUBO through volumetric_clouds.slang's cloud_unpack";
    // The half-resolution upsample is OxidANT's cloud_upsample.slang, so both renderers fill alike.
    EXPECT_NE(contents->find("cloud_upsample(texels,"), std::string::npos)
      << clouds_path.string() << " must upsample through OxidANT's cloud_upsample.slang";
    EXPECT_NE(contents->find("half_texel_depth(texels,"), std::string::npos)
      << clouds_path.string() << " must take a half texel's depth from OxidANT's cloud_upsample.slang";
    for (const char *local : { "float sample_density(",
           "float phase_HG(",
           "bool box_intersect(",
           "float light_optical_depth(",
           "float cloud_upsample_depth_weight(" }) {
        EXPECT_EQ(contents->find(local), std::string::npos)
          << clouds_path.string() << " defines its own " << local << "...), a copy of the shared cloud math";
    }
}

// Dispatch constants have no compiler link to [numthreads]; a mismatch leaves most of the noise volume undefined.
TEST(BuildIntegrity, CloudDispatchGridsMatchTheShaderWorkgroupSizes)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path noise_path = slangRoot() / "compute" / "noise_volume.slang";
    const fs::path clouds_path = slangRoot() / "compute" / "clouds.slang";

    const auto noise_threads = parse_numthreads(noise_path);
    ASSERT_TRUE(noise_threads.has_value()) << "no [numthreads(...)] attribute found in " << noise_path.string();
    EXPECT_EQ((*noise_threads)[0], static_cast<int>(Kataglyphis::kNoiseWorkgroupSize))
      << noise_path.string() << "'s [numthreads(" << (*noise_threads)[0] << ", " << (*noise_threads)[1] << ", "
      << (*noise_threads)[2] << ")] X does not match CloudDispatch.hpp's kNoiseWorkgroupSize ("
      << Kataglyphis::kNoiseWorkgroupSize << ')';
    EXPECT_EQ((*noise_threads)[1], static_cast<int>(Kataglyphis::kNoiseWorkgroupSize))
      << noise_path.string() << "'s [numthreads(" << (*noise_threads)[0] << ", " << (*noise_threads)[1] << ", "
      << (*noise_threads)[2] << ")] Y does not match CloudDispatch.hpp's kNoiseWorkgroupSize ("
      << Kataglyphis::kNoiseWorkgroupSize << ')';
    EXPECT_EQ((*noise_threads)[2], static_cast<int>(Kataglyphis::kNoiseWorkgroupSize))
      << noise_path.string() << "'s [numthreads(" << (*noise_threads)[0] << ", " << (*noise_threads)[1] << ", "
      << (*noise_threads)[2] << ")] Z does not match CloudDispatch.hpp's kNoiseWorkgroupSize ("
      << Kataglyphis::kNoiseWorkgroupSize << ')';

    const auto cloud_threads = parse_numthreads(clouds_path);
    ASSERT_TRUE(cloud_threads.has_value()) << "no [numthreads(...)] attribute found in " << clouds_path.string();
    EXPECT_EQ((*cloud_threads)[0], static_cast<int>(Kataglyphis::kCloudWorkgroupSize))
      << clouds_path.string() << "'s [numthreads(" << (*cloud_threads)[0] << ", " << (*cloud_threads)[1] << ", "
      << (*cloud_threads)[2] << ")] X does not match CloudDispatch.hpp's kCloudWorkgroupSize ("
      << Kataglyphis::kCloudWorkgroupSize << ')';
    EXPECT_EQ((*cloud_threads)[1], static_cast<int>(Kataglyphis::kCloudWorkgroupSize))
      << clouds_path.string() << "'s [numthreads(" << (*cloud_threads)[0] << ", " << (*cloud_threads)[1] << ", "
      << (*cloud_threads)[2] << ")] Y does not match CloudDispatch.hpp's kCloudWorkgroupSize ("
      << Kataglyphis::kCloudWorkgroupSize << ')';
    EXPECT_EQ((*cloud_threads)[2], 1) << clouds_path.string() << "'s [numthreads(" << (*cloud_threads)[0] << ", "
                                      << (*cloud_threads)[1] << ", " << (*cloud_threads)[2] << ")] Z is not 1";

    // The march and the upsample both dispatch over kCloudWorkgroupSize tiles, so every entry must declare it.
    const auto clouds_contents = readFileText(clouds_path);
    ASSERT_TRUE(clouds_contents.has_value()) << "missing " << clouds_path.string();
    static const std::regex kEveryNumThreads(R"(\[numthreads\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)\])");
    std::size_t entries = 0;
    for (auto it = std::sregex_iterator(clouds_contents->begin(), clouds_contents->end(), kEveryNumThreads);
      it != std::sregex_iterator();
      ++it) {
        ++entries;
        const std::string tile = (*it)[1].str() + ", " + (*it)[2].str() + ", " + (*it)[3].str();
        const std::string expected = std::to_string(Kataglyphis::kCloudWorkgroupSize) + ", "
                                     + std::to_string(Kataglyphis::kCloudWorkgroupSize) + ", 1";
        EXPECT_EQ(tile, expected) << clouds_path.string() << " has an entry with [numthreads(" << tile
                                  << ")], not CloudDispatch.hpp's kCloudWorkgroupSize tile";
    }
    EXPECT_EQ(entries, 2U) << clouds_path.string() << " should hold the march and the upsample entry";
}

// [min, max] of the num_march_steps clamp; std::nullopt when the expression is not found.
std::optional<std::pair<float, float>> parse_cloud_march_steps_range(const std::string &contents)
{
    static const std::regex kPattern(
      R"(num_march_steps\s*=\s*int\(\s*clamp\(\s*scene\.cloudParameters\.w\s*,\s*([0-9.]+)\s*,\s*([0-9.]+)\s*\)\s*\))");
    std::smatch match;
    if (!std::regex_search(contents, match, kPattern)) { return std::nullopt; }
    return std::make_pair(std::stof(match[1].str()), std::stof(match[2].str()));
}

// [min, max] of the num_march_steps_to_light clamp.
std::optional<std::pair<float, float>> parse_cloud_light_march_steps_range(const std::string &contents)
{
    static const std::regex kPattern(R"(num_march_steps_to_light\s*=\s*int\(\s*clamp\(\s*scene\.cloudLightMarch\.x\s*,)"
                                     R"(\s*([0-9.]+)\s*,\s*([0-9.]+)\s*\)\s*\))");
    std::smatch match;
    if (!std::regex_search(contents, match, kPattern)) { return std::nullopt; }
    return std::make_pair(std::stof(match[1].str()), std::stof(match[2].str()));
}

// The shader's defensive clamps must match CloudDispatch.hpp, which the GUI slider and host packer also use.
TEST(BuildIntegrity, CloudMarchStepBoundsMatchTheShaderClamps)
{
    const fs::path clouds_path = sharedCloudsPath();
    const auto contents = readFileText(clouds_path);
    ASSERT_TRUE(contents.has_value()) << "missing " << clouds_path.string();

    const auto march_steps_range = parse_cloud_march_steps_range(*contents);
    ASSERT_TRUE(march_steps_range.has_value())
      << clouds_path.string() << ": no `num_march_steps = int(clamp(scene.cloudParameters.w, ...))` found";
    EXPECT_FLOAT_EQ(march_steps_range->first, static_cast<float>(Kataglyphis::kMinCloudMarchSteps))
      << clouds_path.string()
      << "'s num_march_steps lower bound does not match CloudDispatch.hpp's kMinCloudMarchSteps ("
      << Kataglyphis::kMinCloudMarchSteps << ')';
    EXPECT_FLOAT_EQ(march_steps_range->second, static_cast<float>(Kataglyphis::kMaxCloudMarchSteps))
      << clouds_path.string()
      << "'s num_march_steps upper bound does not match CloudDispatch.hpp's kMaxCloudMarchSteps ("
      << Kataglyphis::kMaxCloudMarchSteps << ')';

    const auto light_march_steps_range = parse_cloud_light_march_steps_range(*contents);
    ASSERT_TRUE(light_march_steps_range.has_value())
      << clouds_path.string() << ": no `num_march_steps_to_light = int(clamp(scene.cloudLightMarch.x, ...))` found";
    EXPECT_FLOAT_EQ(light_march_steps_range->first, static_cast<float>(Kataglyphis::kMinCloudLightMarchSteps))
      << clouds_path.string()
      << "'s num_march_steps_to_light lower bound does not match CloudDispatch.hpp's kMinCloudLightMarchSteps ("
      << Kataglyphis::kMinCloudLightMarchSteps << ')';
    EXPECT_FLOAT_EQ(light_march_steps_range->second, static_cast<float>(Kataglyphis::kMaxCloudLightMarchSteps))
      << clouds_path.string()
      << "'s num_march_steps_to_light upper bound does not match CloudDispatch.hpp's kMaxCloudLightMarchSteps ("
      << Kataglyphis::kMaxCloudLightMarchSteps << ')';
}

// Operands of noise_volume_texel's returned float4, split on top-level commas; std::nullopt when absent.
std::optional<std::vector<std::string>> parse_noise_volume_write_operands(const std::string &contents)
{
    static const std::regex kAssignStart(R"(return\s+float4\()");
    std::smatch match;
    if (!std::regex_search(contents, match, kAssignStart)) { return std::nullopt; }

    size_t pos = static_cast<size_t>(match.position(0)) + static_cast<size_t>(match.length(0));
    int depth = 1;
    std::string operandsText;
    for (; pos < contents.size() && depth > 0; ++pos) {
        const char c = contents[pos];
        if (c == '(') {
            ++depth;
            operandsText += c;
        } else if (c == ')') {
            --depth;
            if (depth > 0) { operandsText += c; }
        } else {
            operandsText += c;
        }
    }
    if (depth != 0) { return std::nullopt; }

    std::vector<std::string> operands;
    std::string current;
    int nestDepth = 0;
    for (const char c : operandsText) {
        if (c == '(') {
            ++nestDepth;
            current += c;
        } else if (c == ')') {
            --nestDepth;
            current += c;
        } else if (c == ',' && nestDepth == 0) {
            operands.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    operands.push_back(current);

    for (auto &operand : operands) {
        const size_t start = operand.find_first_not_of(" \t\r\n");
        const size_t end = operand.find_last_not_of(" \t\r\n");
        operand = (start == std::string::npos) ? "" : operand.substr(start, end - start + 1);
    }
    return operands;
}

// The noise must span the whole volume and fill every channel sample_density reads; neither gap is compiler-visible.
TEST(BuildIntegrity, CloudNoiseVolumeCoversItsFullDomainAndWritesEveryChannelTheMarchReads)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path entry_path = slangRoot() / "compute" / "noise_volume.slang";
    const fs::path noise_path = sharedCloudsPath();
    const fs::path clouds_path = sharedCloudsPath();

    const auto entry_contents = readFileText(entry_path);
    ASSERT_TRUE(entry_contents.has_value()) << "could not read " << entry_path.string();
    static const std::regex kEntryWritesTheSharedTexel(R"(noiseVolume\[tid\]\s*=\s*noise_volume_texel\(tid\))");
    EXPECT_TRUE(std::regex_search(*entry_contents, kEntryWritesTheSharedTexel))
      << entry_path.string() << " must write volumetric_clouds.slang's noise_volume_texel(tid), the texel both "
      << "renderers' marches read";

    const auto noise_contents = readFileText(noise_path);
    ASSERT_TRUE(noise_contents.has_value()) << "could not read " << noise_path.string();

    // (a) NOISE_VOLUME_EXTENT must equal CloudDispatch.hpp's kNoiseVolumeExtent.
    static const std::regex kExtentPattern(R"(NOISE_VOLUME_EXTENT\s*=\s*([0-9]+(?:\.[0-9]*)?))");
    std::smatch extent_match;
    ASSERT_TRUE(std::regex_search(*noise_contents, extent_match, kExtentPattern))
      << "no NOISE_VOLUME_EXTENT initializer found in " << noise_path.string();
    const auto parsed_extent = static_cast<uint32_t>(std::stod(extent_match[1].str()));
    EXPECT_EQ(parsed_extent, Kataglyphis::kNoiseVolumeExtent)
      << noise_path.string() << "'s NOISE_VOLUME_EXTENT (" << parsed_extent
      << ") does not match CloudDispatch.hpp's kNoiseVolumeExtent (" << Kataglyphis::kNoiseVolumeExtent << ')';

    // (b) No float4() operand may be a bare numeric literal, a hard-coded channel.
    const auto operands = parse_noise_volume_write_operands(
      extract_function_body(*noise_contents, "float4 noise_volume_texel(uint3 texel)"));
    ASSERT_TRUE(operands.has_value()) << "no `return float4( ... );` found in " << noise_path.string()
                                      << "'s noise_volume_texel";
    ASSERT_EQ(operands->size(), 4u) << noise_path.string()
                                    << "'s noise_volume_texel does not return a float4 of 4 operands";

    static const std::regex kBareNumericLiteral(R"(^[+-]?[0-9]+(\.[0-9]*)?[fF]?$)");
    static const char *const kChannelNames[4] = { ".r", ".g", ".b", ".a" };
    for (size_t i = 0; i < operands->size(); ++i) {
        EXPECT_FALSE(std::regex_match((*operands)[i], kBareNumericLiteral))
          << noise_path.string() << "'s noise_volume_texel float4(...) operand " << (i + 1) << " (channel "
          << kChannelNames[i] << ", value \"" << (*operands)[i]
          << "\") is a bare numeric literal, not a computed value - sample_density weights every "
             "channel into baseDensity/the cirrus band, so a constant channel silently flattens part of the "
             "cloud shape";
    }

    // sample_density must read exactly r, g, b and a, so a changed consumer fails instead of checking a stale set.
    const auto clouds_contents = readFileText(clouds_path);
    ASSERT_TRUE(clouds_contents.has_value()) << "could not read " << clouds_path.string();
    const std::string sample_density_body =
      extract_function_body(*clouds_contents, "float sample_density(ICloudNoise noise, float3 position, Clouds cloud)");
    ASSERT_FALSE(sample_density_body.empty()) << "could not locate sample_density's body in " << clouds_path.string();

    static const std::regex kSwizzleRead(R"(noise(?:Coarse|Fine)\.([rgba]))");
    std::set<char> swizzle_components;
    for (auto it = std::sregex_iterator(sample_density_body.begin(), sample_density_body.end(), kSwizzleRead);
      it != std::sregex_iterator();
      ++it) {
        swizzle_components.insert((*it)[1].str()[0]);
    }
    const std::set<char> expected_components = { 'r', 'g', 'b', 'a' };
    EXPECT_EQ(swizzle_components, expected_components)
      << clouds_path.string()
      << "'s sample_density reads a different swizzle-component set off noiseCoarse/noiseFine than {r, g, b, a}";
}

// The eExclusive noise image belongs to the graphics family, so it is written there, with no compute queue or pool.
TEST(BuildIntegrity, CloudResourcesAreProducedAndConsumedOnOneQueue)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path clouds_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "atmospheric_effects" / "clouds" / "Clouds.cpp";
    const auto clouds_source_opt = readFileText(clouds_path);
    ASSERT_TRUE(clouds_source_opt.has_value()) << "missing " << clouds_path.string();
    const std::string &clouds_source = *clouds_source_opt;

    EXPECT_EQ(clouds_source.find("getComputeQueue"), std::string::npos)
      << "Clouds.cpp must dispatch noise generation on the graphics queue that owns the eExclusive noise image, "
         "not a separate compute queue";
    EXPECT_EQ(clouds_source.find("createCommandPool"), std::string::npos)
      << "Clouds.cpp must reuse the graphics command pool passed into init(), not create its own transient pool "
         "for a different queue family";

    const fs::path device_header_path = repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base" / "VulkanDevice.ixx";
    const auto device_header_source_opt = readFileText(device_header_path);
    ASSERT_TRUE(device_header_source_opt.has_value()) << "missing " << device_header_path.string();
    const std::string &device_header_source = *device_header_source_opt;

    EXPECT_EQ(device_header_source.find("getComputeQueue"), std::string::npos)
      << "VulkanDevice must not expose getComputeQueue() - the only caller (Clouds.cpp) now dispatches on the "
         "graphics queue instead";
}

// abs(fmod(x, N)) maps -x and +x onto one texel, a mirror; frac() wraps negative positions correctly.
TEST(BuildIntegrity, CloudNoiseSamplingWrapsRatherThanMirrors)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path clouds_path = sharedCloudsPath();
    const auto contentsOpt = readFileText(clouds_path);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << clouds_path.string();
    const std::string &contents = *contentsOpt;

    const std::string sample_density_body =
      extract_function_body(contents, "float sample_density(ICloudNoise noise, float3 position, Clouds cloud)");
    ASSERT_FALSE(sample_density_body.empty()) << "could not locate sample_density's body in " << clouds_path.string();

    EXPECT_EQ(sample_density_body.find("abs(fmod("), std::string::npos)
      << clouds_path.string()
      << "'s sample_density wraps the sample position with abs(fmod(...)), which mirrors the "
         "noise field across the origin plane for negative coordinates instead of wrapping it - "
         "the default cloud box straddles that plane, so this renders as a mirror image of "
         "itself. Use frac(x), which returns [0, 1) for negative x too.";
    EXPECT_NE(sample_density_body.find("frac("), std::string::npos)
      << clouds_path.string()
      << "'s sample_density must wrap the sample position with frac(), "
         "not abs(fmod(...))";
}

// Density scales by a constant step length, not distance travelled, or the quality slider becomes a density slider.
TEST(BuildIntegrity, CloudRayMarchesUseAConstantStepLength)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path clouds_path = sharedCloudsPath();
    const auto contentsOpt = readFileText(clouds_path);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << clouds_path.string();
    const std::string &contents = *contentsOpt;

    static const std::regex kDistanceAsStepSize(R"(float\(i\)\s*/\s*float\()");
    EXPECT_FALSE(std::regex_search(contents, kDistanceAsStepSize))
      << clouds_path.string()
      << " contains the distance-as-step-size shape (float(i) / float(...)) - "
         "step length must be (segment length) / (step count), not a fraction "
         "of distance already travelled";

    static const std::regex kLightMarchOriginIsSamplePos(R"(box_intersect\(samplePos,)");
    EXPECT_TRUE(std::regex_search(contents, kLightMarchOriginIsSamplePos))
      << clouds_path.string()
      << "'s light_optical_depth must intersect the box from the point being "
         "shadowed (samplePos), not the camera";

    EXPECT_EQ(contents.find("totalDensity /= "), std::string::npos)
      << clouds_path.string()
      << " must not average the light march's density samples - exp(-totalDensity) needs an "
         "integrated optical depth (density * step length), not a mean density";
}
