// Build integrity: cloud scattering, box, UBO and wind, and the sky's and furnace's constants match their sources.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"
#include "GoldenMetrics.hpp"
#include "common/SceneUboMarshal.hpp"
#include "scene/atmospheric_effects/clouds/CloudDispatch.hpp"
#include "scene/sky_box/AnalyticSky.hpp"
#include "scene/sky_box/CloudAmbient.hpp"
#include "scene/sky_box/SkyUniforms.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// Text pins with no numerical oracle: the phase denominator's sign, and powder kept off transmittance.
TEST(BuildIntegrity, CloudScatteringKeepsItsPhaseSignAndItsMonotonicTransmittance)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path clouds_path = sharedCloudsPath();
    const auto contentsOpt = readFileText(clouds_path);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << clouds_path.string();
    const std::string &contents = *contentsOpt;

    // Positive g peaks toward the sun (cosTheta = +1) only if 2*g*cosTheta is subtracted.
    static const std::regex kPhaseSignFixed(R"(1\.0\s*\+\s*g\s*\*\s*g\s*-\s*2\.0\s*\*\s*g)");
    EXPECT_TRUE(std::regex_search(contents, kPhaseSignFixed))
      << clouds_path.string()
      << "'s phase_HG denominator must be 1.0 + g*g - 2.0*g*cosTheta so a positive g peaks "
         "toward the sun (cosTheta = +1), not away from it";

    static const std::regex kPhaseSignBroken(R"(1\.0\s*\+\s*g\s*\*\s*g\s*\+\s*2\.0\s*\*\s*g)");
    EXPECT_FALSE(std::regex_search(contents, kPhaseSignBroken))
      << clouds_path.string()
      << " contains the sign-flipped phase denominator (1.0 + g*g + 2.0*g*cosTheta), which "
         "peaks away from the sun instead of toward it";

    // Powder belongs on the sun's in-scattering, never in a transmittance assignment.
    const auto lines = readFileLines(clouds_path);
    ASSERT_TRUE(lines.has_value()) << "missing " << clouds_path.string();
    static const std::regex kPlainTransmittanceAssign(R"(transmittance\s*=[^=*])");
    for (std::size_t i = 0; i < lines->size(); ++i) {
        const std::string &line = (*lines)[i];
        if (!std::regex_search(line, kPlainTransmittanceAssign)) { continue; }
        EXPECT_EQ(line.find("powder"), std::string::npos)
          << clouds_path.string() << ":" << (i + 1)
          << " assigns transmittance from an expression mentioning powder - the powder term "
             "must attenuate the sun's in-scattering, not raise transmittance ("
          << line << ")";
    }

    // In the march loop transmittance only decreases, through the Beer-Lambert `*= exp(-...)`.
    const std::size_t loop_start = contents.find("for (int i = 0; i < cloud.num_march_steps; i++)");
    ASSERT_NE(loop_start, std::string::npos) << clouds_path.string() << " is missing the primary march loop";
    // Tolerate CRLF: the file is checked in with Windows line endings.
    static const std::regex kLoopClosingBrace(R"(\r?\n        \}\r?\n)");
    std::size_t loop_body_end = std::string::npos;
    for (auto it = std::sregex_iterator(contents.begin(), contents.end(), kLoopClosingBrace),
              end = std::sregex_iterator();
      it != end;
      ++it) {
        if (static_cast<std::size_t>(it->position()) >= loop_start) {
            loop_body_end = static_cast<std::size_t>(it->position());
            break;
        }
    }
    ASSERT_NE(loop_body_end, std::string::npos)
      << clouds_path.string() << "'s primary march loop is missing its closing brace";
    const std::string loop_body = contents.substr(loop_start, loop_body_end - loop_start);

    static const std::regex kTransmittanceAssignAnyForm(R"(transmittance\s*(?:\*=|\+=|-=|=[^=])\s*[^;]*)");
    auto begin = std::sregex_iterator(loop_body.begin(), loop_body.end(), kTransmittanceAssignAnyForm);
    const auto end = std::sregex_iterator();
    ASSERT_NE(begin, end) << clouds_path.string() << "'s march loop never assigns transmittance";
    for (auto it = begin; it != end; ++it) {
        const std::string matched = it->str();
        EXPECT_TRUE(matched.find("*=") != std::string::npos && matched.find("exp(-") != std::string::npos)
          << clouds_path.string()
          << "'s march loop assigns transmittance with something other than `*= exp(-...)`: " << matched;
    }
}

// Slang's floatNxM(1.0) fills every element: the cloud box was once a sheared slab its Offset could not move.
TEST(BuildIntegrity, CloudBoxIsAnAxisAlignedBoxAroundItsOffset)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    static const std::regex kScalarMatrix(R"(float[234]x[234]\(\s*[-+]?[0-9.]+[fF]?\s*\))");
    std::vector<std::string> scalar_matrices;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const auto lines = readFileLines(shader_path);
        if (!lines) { continue; }
        for (std::size_t i = 0; i < lines->size(); ++i) {
            const std::string line = strip_line_comment((*lines)[i]);
            if (std::regex_search(line, kScalarMatrix)) {
                scalar_matrices.push_back(
                  shaderRelativePath(shader_path, slang_root).generic_string() + ":" + std::to_string(i + 1));
            }
        }
    }
    EXPECT_TRUE(scalar_matrices.empty())
      << "a matrix built from one scalar is all that scalar in Slang, not a scaled identity:"
      << joinViolations(scalar_matrices);

    const fs::path clouds_path = sharedCloudsPath();
    const auto clouds_text = readFileText(clouds_path);
    ASSERT_TRUE(clouds_text.has_value()) << "missing " << clouds_path.string();

    const std::string box_body =
      extract_function_body(*clouds_text, "bool box_intersect(float3 ro, float3 rd, Clouds cloud, out float2 oT)");
    ASSERT_FALSE(box_body.empty()) << "could not locate box_intersect's body in " << clouds_path.string();
    EXPECT_NE(box_body.find("ro - cloud.offset"), std::string::npos)
      << "box_intersect must centre the box on cloud.offset, or the Offset control no longer moves the volume";
    EXPECT_NE(box_body.find("cloud.radius"), std::string::npos)
      << "box_intersect must take its half extents from cloud.radius, the Scale x 10 extent";
    EXPECT_EQ(clouds_text->find("inv_model_to_world"), std::string::npos)
      << clouds_path.string() << " builds a box matrix again; an axis-aligned box needs none";

    const std::string density_body =
      extract_function_body(*clouds_text, "float sample_density(ICloudNoise noise, float3 position, Clouds cloud)");
    ASSERT_FALSE(density_body.empty()) << "could not locate sample_density's body in " << clouds_path.string();
    EXPECT_NE(density_body.find("position - cloud.offset"), std::string::npos)
      << "sample_density must sample box-local noise, or moving the box slides the clouds through it";
}

// The host-to-shader cloud UBO pairs, shared by the unpack test and the doc-table test so their truths cannot diverge.
struct CloudUboFieldPair
{
    const char *shader_field;
    const char *scene_field;
    const char *component;
};
constexpr std::array<CloudUboFieldPair, 12> kCloudUboFieldPairs{ {
  { "cloud.num_march_steps", "cloudParameters", "w" },
  { "cloud.num_march_steps_to_light", "cloudLightMarch", "x" },
  { "cloud.scale", "cloudMeshScale", "w" },
  { "cloud.threshold", "cloudMeshOffset", "w" },
  { "cloud.pillowness", "cloudParameters", "x" },
  { "cloud.cirrus_effect", "cloudParameters", "y" },
  { "cloud.powder_effect", "cloudParameters", "z" },
  { "cloud.radius", "cloudMeshScale", "xyz" },
  { "cloud.offset", "cloudMeshOffset", "xyz" },
  { "cloud.wind_offset", "cloudLightMarch", "yzw" },
  { "ambient.top", "cloudAmbientTop", "xyz" },
  { "ambient.bottom", "cloudAmbientBottom", "xyz" },
} };

// Offsets are pinned elsewhere, but a component swap is not; each host component must land in its shader field.
TEST(BuildIntegrity, CloudUboPackingMatchesTheShaderUnpack)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path shared_path = sharedCloudsPath();
    const auto shared_contents = readFileText(shared_path);
    ASSERT_TRUE(shared_contents.has_value()) << "missing " << shared_path.string();
    // The ambient is the C++ engine's own input, unpacked in its entry rather than the shared cloud_unpack.
    const fs::path entry_path = slangRoot() / "compute" / "clouds.slang";
    const auto entry_contents = readFileText(entry_path);
    ASSERT_TRUE(entry_contents.has_value()) << "missing " << entry_path.string();

    for (const auto &pair : kCloudUboFieldPairs) {
        const std::string field = pair.shader_field;
        const bool in_entry = field.starts_with("ambient.");
        const fs::path &clouds_path = in_entry ? entry_path : shared_path;
        const std::string &contents = in_entry ? *entry_contents : *shared_contents;
        const std::string pattern = std::regex_replace(field, std::regex(R"(\.)"), R"(\.)") + R"(\s*=[^;]*scene\.)"
                                    + pair.scene_field + "\\." + pair.component + "\\b";
        const std::regex field_regex(pattern);
        EXPECT_TRUE(std::regex_search(contents, field_regex))
          << clouds_path.string() << " no longer assigns " << pair.shader_field << " from scene." << pair.scene_field
          << '.' << pair.component
          << " - the host packer (SceneUboMarshal.hpp's fillSceneUboClouds) and this shader's unpack "
             "must agree on which component every cloud slider lands in";
    }
}

// cloudWindOffset wraps to kCloudNoisePeriod; a wrap the noise does not repeat at would make the clouds jump.
TEST(BuildIntegrity, CloudWindWrapsAtTheNoisePeriod)
{
    const fs::path clouds_path = sharedCloudsPath();
    const auto contents = readFileText(clouds_path);
    ASSERT_TRUE(contents.has_value()) << "missing " << clouds_path.string();
    const std::string density_body =
      extract_function_body(*contents, "float sample_density(ICloudNoise noise, float3 position, Clouds cloud)");
    ASSERT_FALSE(density_body.empty()) << "could not locate sample_density's body in " << clouds_path.string();
    EXPECT_NE(density_body.find("- cloud.wind_offset"), std::string::npos)
      << "sample_density no longer moves the noise by the wind offset the host packs";

    // Every frac(local / <period> ...) must tile the wrap: the period divides kCloudNoisePeriod.
    static const std::regex kPeriod(R"(frac\(\s*local\s*/\s*([0-9.]+))");
    std::vector<double> periods;
    for (auto match = std::sregex_iterator(density_body.begin(), density_body.end(), kPeriod);
      match != std::sregex_iterator();
      ++match) {
        periods.push_back(std::stod((*match)[1].str()));
    }
    ASSERT_FALSE(periods.empty()) << "found no frac(local / <period>) noise lookup in sample_density";
    EXPECT_DOUBLE_EQ(*std::max_element(periods.begin(), periods.end()), Kataglyphis::kCloudNoisePeriod)
      << "kCloudNoisePeriod is not the coarsest noise period sample_density reads";
    for (const double period : periods) {
        EXPECT_DOUBLE_EQ(std::fmod(Kataglyphis::kCloudNoisePeriod, period), 0.0)
          << "the noise period " << period << " does not divide kCloudNoisePeriod";
    }
}

// CloudAmbient.hpp's navy feeds the cloud ambient with the sky off; it must be the navy the sky and a camera miss show.
TEST(BuildIntegrity, CloudAmbientNavyIsTheDisabledSkyRadiance)
{
    const std::array<std::pair<fs::path, const char *>, 1> sources = {
        std::pair{ sharedSlangRoot() / "common" / "drawn_sky.slang", "SKYBOX_DISABLED" },
    };
    for (const auto &[path, name] : sources) {
        const auto contents = readFileText(path);
        ASSERT_TRUE(contents.has_value()) << "missing " << path.string();
        std::smatch match;
        ASSERT_TRUE(std::regex_search(*contents,
          match,
          std::regex(std::string(name) + R"(\s*=\s*float3\(\s*([0-9.]+)\s*,\s*([0-9.]+)\s*,\s*([0-9.]+)\s*\))")))
          << path.string() << " no longer defines " << name << " as a float3 literal";
        for (int channel = 0; channel < 3; ++channel) {
            EXPECT_FLOAT_EQ(std::stof(match[static_cast<std::size_t>(channel) + 1U].str()),
              Kataglyphis::kSkyboxDisabledRadiance[channel])
              << "CloudAmbient.hpp's kSkyboxDisabledRadiance channel " << channel << " differs from " << path.string();
        }
    }
}

// SkyUniforms.hpp's modes must land in the branches drawn_sky.slang takes for them, which sky.slang hands its mode.
TEST(BuildIntegrity, SkyModesSelectTheCubeMapAndTheNavyInSkySlang)
{
    const fs::path sky_slang_path = sharedSlangRoot() / "sky" / "sky.slang";
    const auto sky_slang = readFileText(sky_slang_path);
    ASSERT_TRUE(sky_slang.has_value()) << "missing " << sky_slang_path.string()
                                       << " - is the OxidANT submodule checked out?";
    EXPECT_NE(sky_slang->find("drawn_sky(sky.mode.x, cube,"), std::string::npos)
      << sky_slang_path.string() << " no longer draws drawn_sky() with its mode and its cube map's sample";
    EXPECT_NE(sky_slang->find("float3 cube = skyboxTex.SampleLevel("), std::string::npos)
      << sky_slang_path.string() << " no longer samples skyboxTex for drawn_sky()";
    const fs::path sky_path = sharedSlangRoot() / "common" / "drawn_sky.slang";
    const auto contents = readFileText(sky_path);
    ASSERT_TRUE(contents.has_value()) << "missing " << sky_path.string() << " - is the OxidANT submodule checked out?";
    std::vector<float> thresholds;
    static const std::regex kBranch(
      R"(if\s*\(\s*mode\s*>\s*([0-9.]+)\s*\)\s*\n(?:\s*//[^\n]*\n)*\s*return\s+([^;]*);)");
    std::vector<std::string> returns;
    for (std::sregex_iterator it(contents->begin(), contents->end(), kBranch), end; it != end; ++it) {
        thresholds.push_back(std::stof((*it)[1].str()));
        returns.push_back((*it)[2].str());
    }
    ASSERT_EQ(thresholds.size(), 2U) << sky_path.string() << " no longer branches twice on mode";
    // The higher threshold comes first and draws the navy; the lower one decodes the cube map's sample.
    EXPECT_NE(returns[0].find("SKYBOX_DISABLED"), std::string::npos) << returns[0];
    EXPECT_NE(returns[1].find("aces_tonemap_inverse(cube)"), std::string::npos) << returns[1];
    EXPECT_GT(Kataglyphis::kSkyModeSwitchedOff, thresholds[0]);
    EXPECT_GT(Kataglyphis::kSkyModeCubeMap, thresholds[1]);
    EXPECT_LE(Kataglyphis::kSkyModeCubeMap, thresholds[0]);
}

// The furnace goldens grade against a C++ copy of brdf.slang, whose floor and guard once drifted from the shader's.
TEST(BuildIntegrity, FurnaceOracleUsesBrdfSlangsConstants)
{
    const fs::path constants_path = sharedSlangRoot() / "common" / "constants.slang";
    const auto constants = readFileText(constants_path);
    ASSERT_TRUE(constants.has_value()) << "missing " << constants_path.string()
                                       << " - is the OxidANT submodule checked out?";
    std::smatch floor;
    ASSERT_TRUE(std::regex_search(*constants, floor, std::regex(R"(MIN_ROUGHNESS\s*=\s*([0-9.]+)\s*;)")))
      << constants_path.string() << " no longer defines MIN_ROUGHNESS as a literal";
    EXPECT_DOUBLE_EQ(std::stod(floor[1].str()), Kataglyphis::Test::GoldenMetrics::kBrdfMinRoughness)
      << "GoldenMetrics.hpp's kBrdfMinRoughness differs from " << constants_path.string();
    const fs::path brdf_path = sharedSlangRoot() / "common" / "brdf.slang";
    const auto contents = readFileText(brdf_path);
    ASSERT_TRUE(contents.has_value()) << "missing " << brdf_path.string();
    std::smatch guard;
    ASSERT_TRUE(std::regex_search(
      *contents, guard, std::regex(R"(max\(\s*denom\s*\*\s*denom\s*,\s*([0-9.]+(?:[eE][-+]?[0-9]+)?)\s*\))")))
      << brdf_path.string() << " no longer guards distribution_ggx's denom * denom with a literal";
    EXPECT_DOUBLE_EQ(std::stod(guard[1].str()), Kataglyphis::Test::GoldenMetrics::kBrdfNdfDenominatorGuard)
      << "GoldenMetrics.hpp's kBrdfNdfDenominatorGuard differs from " << brdf_path.string();
}

// The sky a missing skybox bakes is OxidANT's procedural one: AnalyticSky.hpp copies sky_model.slang's three colours.
TEST(BuildIntegrity, AnalyticSkyMatchesOxidantsSkyModel)
{
    const fs::path model_path = sharedSlangRoot() / "common" / "sky_model.slang";
    const auto contents = readFileText(model_path);
    ASSERT_TRUE(contents.has_value()) << "missing " << model_path.string()
                                      << " - is the OxidANT submodule checked out?";
    const std::array<std::pair<const char *, glm::vec3>, 3> colours{ std::pair{
                                                                       "SKY_ZENITH", Kataglyphis::kAnalyticSkyZenith },
        std::pair{ "SKY_HORIZON", Kataglyphis::kAnalyticSkyHorizon },
        std::pair{ "SKY_GROUND", Kataglyphis::kAnalyticSkyGround } };
    for (const auto &[name, colour] : colours) {
        std::smatch match;
        const std::regex literal(
          std::string(name) + R"(\s*=\s*float3\(\s*([0-9.]+)\s*,\s*([0-9.]+)\s*,\s*([0-9.]+)\s*\))");
        ASSERT_TRUE(std::regex_search(*contents, match, literal))
          << model_path.string() << " no longer defines " << name << " as a float3 literal";
        for (int channel = 0; channel < 3; ++channel) {
            EXPECT_FLOAT_EQ(std::stof(match[static_cast<std::size_t>(channel) + 1U].str()), colour[channel])
              << "AnalyticSky.hpp's " << name << " channel " << channel << " differs from " << model_path.string();
        }
    }
    // The gradient's shape: pow(y, 0.7) above the horizon, -y * 3 below it.
    EXPECT_NE(contents->find("pow(clamp(dir.y, 0.0, 1.0), 0.7)"), std::string::npos)
      << "sky_gradient's upper curve changed; update analyticSkyRadiance";
    EXPECT_NE(contents->find("clamp(-dir.y * 3.0, 0.0, 1.0)"), std::string::npos)
      << "sky_gradient's lower curve changed; update analyticSkyRadiance";
}

// Owning counterpart of CloudUboFieldPair, whose const char* fields may only point at literals.
struct ParsedCloudUboRow
{
    std::string shader_field;
    std::string scene_field;
    std::string component;
};

// docs/source/clouds.md's cloud-ubo marker table; std::nullopt without the markers.
std::optional<std::vector<ParsedCloudUboRow>> parse_cloud_ubo_doc_table(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kRowPattern(R"(\|[^|]*\|\s*`([a-z]+\.[A-Za-z_]+)`\s*\|\s*`([A-Za-z]+)\.([A-Za-z]+)`\s*\|)");

    std::vector<ParsedCloudUboRow> rows;
    bool in_block = false;
    bool saw_block = false;
    for (const auto &line : *lines) {
        if (line.find("<!-- cloud-ubo:begin -->") != std::string::npos) {
            in_block = true;
            saw_block = true;
            continue;
        }
        if (line.find("<!-- cloud-ubo:end -->") != std::string::npos) {
            in_block = false;
            continue;
        }
        if (!in_block) { continue; }

        std::smatch match;
        if (std::regex_search(line, match, kRowPattern)) {
            rows.push_back({ match[1].str(), match[2].str(), match[3].str() });
        }
    }

    if (!saw_block) { return std::nullopt; }
    return rows;
}

// docs/source/clouds.md's cloud-constants marker table; std::nullopt without the markers.
std::optional<std::map<std::string, long long>> parse_cloud_constants_doc_table(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kRowPattern(R"(\|\s*`(k[A-Za-z]+)`\s*\|\s*([0-9]+)\s*\|)");

    std::map<std::string, long long> rows;
    bool in_block = false;
    bool saw_block = false;
    for (const auto &line : *lines) {
        if (line.find("<!-- cloud-constants:begin -->") != std::string::npos) {
            in_block = true;
            saw_block = true;
            continue;
        }
        if (line.find("<!-- cloud-constants:end -->") != std::string::npos) {
            in_block = false;
            continue;
        }
        if (!in_block) { continue; }

        std::smatch match;
        if (std::regex_search(line, match, kRowPattern)) { rows[match[1].str()] = std::stoll(match[2].str()); }
    }

    if (!saw_block) { return std::nullopt; }
    return rows;
}

// Both hand-maintained clouds.md tables must match their sources, kCloudUboFieldPairs and the compiled constants.
TEST(BuildIntegrity, CloudsDocTablesMatchTheirSources)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "source" / "clouds.md";
    if (!sourceExists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto ubo_rows = parse_cloud_ubo_doc_table(doc_path);
    ASSERT_TRUE(ubo_rows.has_value())
      << doc_path.string() << " is missing its '<!-- cloud-ubo:begin -->' / '<!-- cloud-ubo:end -->' marker block";

    ASSERT_EQ(ubo_rows->size(), kCloudUboFieldPairs.size())
      << doc_path.string() << "'s cloud-ubo table has " << ubo_rows->size() << " row(s), expected "
      << kCloudUboFieldPairs.size() << " to match kCloudUboFieldPairs";
    for (std::size_t i = 0; i < kCloudUboFieldPairs.size(); ++i) {
        const auto &expected = kCloudUboFieldPairs[i];
        const auto &actual = (*ubo_rows)[i];
        EXPECT_EQ(actual.shader_field, expected.shader_field)
          << doc_path.string() << "'s cloud-ubo table row " << i << " has shader field \"" << actual.shader_field
          << "\", expected \"" << expected.shader_field << '"';
        EXPECT_EQ(actual.scene_field, expected.scene_field)
          << doc_path.string() << "'s cloud-ubo table row " << i << " (" << expected.shader_field
          << ") has SceneUBO field \"" << actual.scene_field << "\", expected \"" << expected.scene_field << '"';
        EXPECT_EQ(actual.component, expected.component)
          << doc_path.string() << "'s cloud-ubo table row " << i << " (" << expected.shader_field
          << ") has SceneUBO component \"" << actual.component << "\", expected \"" << expected.component << '"';
    }

    const auto constants_rows = parse_cloud_constants_doc_table(doc_path);
    ASSERT_TRUE(constants_rows.has_value())
      << doc_path.string()
      << " is missing its '<!-- cloud-constants:begin -->' / '<!-- cloud-constants:end -->' marker block";

    const std::map<std::string, long long> truth{
        { "kNoiseVolumeExtent", static_cast<long long>(Kataglyphis::kNoiseVolumeExtent) },
        { "kNoiseWorkgroupSize", static_cast<long long>(Kataglyphis::kNoiseWorkgroupSize) },
        { "kCloudWorkgroupSize", static_cast<long long>(Kataglyphis::kCloudWorkgroupSize) },
        { "kMinCloudMarchSteps", static_cast<long long>(Kataglyphis::kMinCloudMarchSteps) },
        { "kMaxCloudMarchSteps", static_cast<long long>(Kataglyphis::kMaxCloudMarchSteps) },
        { "kMinCloudLightMarchSteps", static_cast<long long>(Kataglyphis::kMinCloudLightMarchSteps) },
        { "kMaxCloudLightMarchSteps", static_cast<long long>(Kataglyphis::kMaxCloudLightMarchSteps) },
    };

    for (const auto &[name, value] : truth) {
        const auto it = constants_rows->find(name);
        ASSERT_TRUE(it != constants_rows->end())
          << doc_path.string() << "'s cloud-constants table is missing a row for `" << name << '`';
        EXPECT_EQ(it->second, value) << doc_path.string() << "'s cloud-constants table says " << name << " = "
                                     << it->second << ", CloudDispatch.hpp says " << value;
    }
    for (const auto &[name, value] : *constants_rows) {
        if (truth.contains(name)) { continue; }
        ADD_FAILURE() << doc_path.string() << "'s cloud-constants table has an unexpected row `" << name
                      << "` = " << value << " with no matching CloudDispatch.hpp constant";
    }
}

// CloudAmbient.hpp's CPU tonemap pair untonemaps the skybox for the cloud ambient as sky.slang does on the GPU.
TEST(BuildIntegrity, CloudAmbientAcesCurveIsAcesSlangs)
{
    const fs::path aces_path = sharedSlangRoot() / "common" / "aces.slang";
    const auto contents = readFileText(aces_path);
    ASSERT_TRUE(contents.has_value()) << "missing " << aces_path.string();
    for (std::size_t i = 0; i < Kataglyphis::kAcesCoefficients.size(); ++i) {
        const std::string name = std::string("ACES_") + static_cast<char>('A' + i);
        std::smatch match;
        ASSERT_TRUE(std::regex_search(
          *contents, match, std::regex(R"(static\s+const\s+float\s+)" + name + R"(\s*=\s*([0-9.]+)\s*;)")))
          << aces_path.string() << " no longer defines " << name << " as a literal";
        EXPECT_FLOAT_EQ(std::stof(match[1].str()), Kataglyphis::kAcesCoefficients[i])
          << "CloudAmbient.hpp's kAcesCoefficients[" << i << "] differs from " << name;
    }
}
