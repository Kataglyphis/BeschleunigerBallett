// Build integrity: the app, the loaders and the Rust bridge: teardown, exit codes, test hooks, uploads, crates.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// Texture slots, meshes and upload preconditions go through ModelAssembly.ixx, so the loaders cannot drift apart.
TEST(BuildIntegrity, ModelUploadGoesThroughTheSharedAssembly)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    // The wrapped functions' own definitions, and the two loaders as the shared guard's intended callers.
    static const std::array<const char *, 4> kExemptFiles = { "Src/GraphicsEngineVulkan/scene/ModelAssembly.ixx",
        "Src/GraphicsEngineVulkan/scene/Texture.ixx",
        "Src/GraphicsEngineVulkan/scene/Texture.cpp",
        "Src/GraphicsEngineVulkan/scene/GltfLoader.cpp" };
    static const std::array<const char *, 2> kBannedCalls = { "createDefaultTexture(", "uploadPreconditionsMet(" };

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (std::find(kExemptFiles.begin(), kExemptFiles.end(), relative_file) != kExemptFiles.end()) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            for (const char *banned : kBannedCalls) {
                if (line.find(banned) == std::string::npos) { continue; }
                violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " call(s) to createDefaultTexture/uploadPreconditionsMet found outside their sanctioned "
         "callers under Src/ - build meshes and texture slots through "
         "addMeshesForRangesInPlace/addTextureOrDefault/ensureAtLeastOneTexture and check upload preconditions through "
         "uploadPreconditionsMet (kataglyphis.vulkan.model_assembly) instead:"
      << joinViolations(violations);
}

// A copy of the bridge's arrays into C++ containers held the Colosseum's 0.5 GB twice at the parse's peak.
TEST(BuildIntegrity, GltfLoaderUploadsTheBridgeArraysWithoutACopy)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path scene_dir = repo_root / "Src" / "GraphicsEngineVulkan" / "scene";

    std::vector<std::string> violations;
    const auto scan = [&](const char *file, std::initializer_list<const char *> banned) {
        const auto lines = readFileLines(scene_dir / file);
        ASSERT_TRUE(lines.has_value()) << "could not open " << file;
        std::size_t line_number = 0;
        for (const std::string &raw : *lines) {
            ++line_number;
            const std::string line = strip_line_comment(raw);
            const bool copies = std::ranges::any_of(
              banned, [&line](const char *needle) { return line.find(needle) != std::string::npos; });
            if (copies) { violations.push_back(std::string(file) + ":" + std::to_string(line_number) + ": " + raw); }
        }
    };
    // An owning container for the vertices, indices, material ids or encoded images is the copy.
    scan("GltfLoader.ixx",
      { "std::vector<Vertex>", "std::vector<unsigned int>", "std::vector<std::vector<unsigned char>>" });
    // The bridge's arrays are read through data() and size() only; anything that iterates or memcpys them copies.
    scan("GltfLoaderRust.cpp", { "memcpy(", ".assign(", ".begin()", ".end()", ".resize(" });

    const auto upload_text = readFileText(scene_dir / "GltfLoader.cpp");
    ASSERT_TRUE(upload_text.has_value()) << "could not open GltfLoader.cpp";
    const std::string upload_body = extract_function_body(*upload_text, "GltfLoader::uploadParsed(");
    ASSERT_FALSE(upload_body.empty()) << "could not locate GltfLoader::uploadParsed";
    if (upload_body.find("addMeshesForRangesInPlace(") == std::string::npos) {
        violations.emplace_back(
          "GltfLoader::uploadParsed builds its meshes without addMeshesForRangesInPlace, "
          "so each range is sliced into a copy first");
    }

    EXPECT_TRUE(violations.empty()) << "GltfLoader must keep OxidANT's result and upload from views of it:"
                                    << joinViolations(violations);
}

// Every class with cleanUp() calls it from its destructor, so a skipped explicit call still tears down.
TEST(BuildIntegrity, EveryCleanUpIsCalledFromItsDestructor)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

    // Mesh and VulkanBufferManager are fully RAII.
    static const std::array<const char *, 2> kExemptClasses = { "Mesh", "VulkanBufferManager" };

    // Anchored at line start, so prose mentioning a class cannot pass for a declaration.
    const std::regex class_pattern(R"(\nclass\s+(\w+))");

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".ixx") { continue; }

        const auto ixx_contents_opt = readFileText(path);
        if (!ixx_contents_opt) { continue; }
        const std::string &ixx_contents = *ixx_contents_opt;

        if (ixx_contents.find("void cleanUp();") == std::string::npos) { continue; }

        std::smatch match;
        if (!std::regex_search(ixx_contents, match, class_pattern)) { continue; }
        const std::string class_name = match[1].str();

        if (std::find(kExemptClasses.begin(), kExemptClasses.end(), class_name) != kExemptClasses.end()) { continue; }

        std::string combined_contents = ixx_contents;
        const fs::path cpp_path = fs::path(path).replace_extension(".cpp");
        if (const auto cpp_contents = readFileText(cpp_path)) { combined_contents += *cpp_contents; }

        // Matches "~Name() { cleanUp(); }" inline or out-of-line, possibly namespace-qualified.
        const std::regex dtor_pattern(R"(~)" + class_name + R"(\s*\(\s*\)\s*\{\s*cleanUp\s*\(\s*\)\s*;\s*\})");
        if (std::regex_search(combined_contents, dtor_pattern)) { continue; }

        violations.push_back(fs::relative(path, repo_root).generic_string() + ": class " + class_name
                             + " declares cleanUp() but its destructor does not call it");
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " class(es) under Src/GraphicsEngineVulkan/ declare cleanUp() without a destructor that calls it "
         "(`~Name() { cleanUp(); }`) - AGENTS.md requires cleanUp() to double as the destructor body so a "
         "forgotten explicit call still tears the object down. Add the destructor call, or add the class to "
         "kExemptClasses with a reason if it genuinely cannot call cleanUp() from its destructor:"
      << joinViolations(violations);
}

// App::run() derives its exit code via appExitCode, or a lost device reads as a clean quit.
TEST(BuildIntegrity, AppRunDoesNotReturnABareExitSuccess)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path app_cpp = repo_root / "Src" / "GraphicsEngineVulkan" / "app" / "App.cpp";
    ASSERT_TRUE(sourceExists(app_cpp)) << "missing " << app_cpp.string();

    const auto contentsOpt = readFileText(app_cpp);
    ASSERT_TRUE(contentsOpt.has_value()) << "could not open " << app_cpp.string();
    const std::string &contents = *contentsOpt;

    EXPECT_EQ(contents.find("return EXIT_SUCCESS;"), std::string::npos)
      << "App.cpp contains a bare \"return EXIT_SUCCESS;\" - App::run()'s exit code must be derived from "
         "whether the frame loop hit a device loss or fatal frame error (Kataglyphis::appExitCode), not "
         "hard-coded, or a broken run is reported as a clean quit again.";
}

// Teardown is the locals' destruction order; an explicit cleanUp() there could free the device under the scene again.
TEST(BuildIntegrity, AppRunTearsDownByScopeAlone)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const auto contents = readFileText(repo_root / "Src" / "GraphicsEngineVulkan" / "app" / "App.cpp");
    ASSERT_TRUE(contents.has_value()) << "could not open App.cpp";

    EXPECT_EQ(contents->find("cleanUp("), std::string::npos)
      << "App.cpp calls a cleanUp() - App::run tears down by its locals' reverse declaration order, the renderer "
         "first, and the scene and GUI keep the device alive until they release theirs (Integration.SimulatedDevice"
         "LossEndsTheRunAsAFailureAndTearsDownCleanly).";
}

// The device-loss and failed-bake seams are test instrumentation: no engine code may arm them.
TEST(BuildIntegrity, DeviceLossSimulationIsArmedOnlyByTests)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path renderer_dir = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer";
    const std::array<std::pair<std::string_view, std::set<fs::path>>, 2> seams = {
        std::pair<std::string_view, std::set<fs::path>>{
          "simulateDeviceLossForTesting", { renderer_dir / "VulkanRenderer.ixx" } },
        std::pair<std::string_view, std::set<fs::path>>{ "simulateFailedAmbientBakeForTesting",
          { renderer_dir / "VulkanRenderer.ixx", renderer_dir / "VulkanRenderer.cpp" } },
    };
    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(repo_root / "Src", error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        const auto contents = readFileText(path);
        if (!contents) { continue; }
        for (const auto &[seam, declaring_files] : seams) {
            if (!declaring_files.contains(path) && contents->find(seam) != std::string::npos) {
                violations.push_back(fs::relative(path, repo_root).generic_string() + " arms " + std::string(seam));
            }
        }
    }

    EXPECT_TRUE(violations.empty()) << "engine source arms a test seam:" << joinViolations(violations);
}

// The benchmark seam records without submitting, so a frame of engine code that called it would never be presented.
TEST(BuildIntegrity, UnsubmittedRecordingIsCalledOnlyByBenchmarks)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path renderer_dir = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer";
    const std::set<fs::path> declaring_files = { renderer_dir / "VulkanRenderer.ixx",
        renderer_dir / "VulkanRenderer.cpp" };
    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(repo_root / "Src", error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || declaring_files.contains(path)) { continue; }
        const auto contents = readFileText(path);
        if (contents && contents->find("recordCommandsUnsubmittedForBenchmark") != std::string::npos) {
            violations.push_back(fs::relative(path, repo_root).generic_string());
        }
    }
    EXPECT_TRUE(violations.empty()) << "engine source records frames it never submits:" << joinViolations(violations);

    const auto perf_suite = readFileText(repo_root / "Test" / "perf" / "perfSuite.cpp");
    ASSERT_TRUE(perf_suite.has_value()) << "could not open Test/perf/perfSuite.cpp";
    EXPECT_NE(perf_suite->find("recordCommandsUnsubmittedForBenchmark()"), std::string::npos)
      << "no benchmark calls the seam any more; delete it from VulkanRenderer";
}

// abseil is the one CLI front end, so no dead hand-rolled parser can come back.
TEST(BuildIntegrity, MainHasOneCommandLineParser)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path source = repo_root / "Src" / "GraphicsEngineVulkan" / "Main.cpp";
    const auto text = readFileText(source);
    ASSERT_TRUE(text.has_value()) << "could not read " << source.string();

    EXPECT_NE(text->find("absl::ParseCommandLine"), std::string::npos)
      << "Main.cpp must parse arguments via absl::ParseCommandLine in " << source.string();
    EXPECT_EQ(text->find("parse_command_line"), std::string::npos)
      << "Main.cpp must not carry a hand-rolled parse_command_line() alongside absl::ParseCommandLine in "
      << source.string();
    EXPECT_EQ(text->find("print_usage"), std::string::npos)
      << "Main.cpp must not carry a hand-rolled print_usage() alongside absl::ParseCommandLine in " << source.string();
}

// Model owns the transform; a per-mesh matrix would be a second truth that no draw path reads.
TEST(BuildIntegrity, MeshDoesNotHoldAModelMatrix)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path mesh_ixx_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "Mesh.ixx";
    const auto text_opt = readFileText(mesh_ixx_path);
    ASSERT_TRUE(text_opt.has_value()) << "could not read " << mesh_ixx_path.string();
    const std::string &text = *text_opt;

    static const char *kFailureMessage =
      "the per-model transform is owned by Model::set_model/Model::getModel and reached through "
      "Scene::update_model_matrix/Scene::getModelMatrix; a per-mesh copy would be a second source of truth that no "
      "draw path reads";

    EXPECT_EQ(text.find("setModel"), std::string::npos) << "Mesh.ixx must not declare setModel(). " << kFailureMessage;
    EXPECT_EQ(text.find("getModel"), std::string::npos) << "Mesh.ixx must not declare getModel(). " << kFailureMessage;
    EXPECT_EQ(text.find("glm::mat4 model"), std::string::npos)
      << "Mesh.ixx must not hold a glm::mat4 model member. " << kFailureMessage;
}

namespace {

// OxidANT's bridge files: the cxx_bridge crate's src/*.rs that open a `#[cxx::bridge(` module.
std::set<std::string> oxidant_bridge_files(const fs::path &bridge_src)
{
    std::set<std::string> files;
    std::error_code error;
    for (fs::directory_iterator it(bridge_src, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".rs") { continue; }
        const auto lines = readFileLines(it->path());
        if (!lines) { continue; }
        const bool bridges = std::any_of(
          lines->begin(), lines->end(), [](const std::string &line) { return line.starts_with("#[cxx::bridge("); });
        if (bridges) { files.insert(it->path().filename().string()); }
    }
    return files;
}

// The FILES of Src/CMakeLists.txt's corrosion_add_cxxbridge call; nullopt without one.
std::optional<std::set<std::string>> cxxbridge_files_in_cmake(const std::string &cmake_text)
{
    static const std::regex kCall(R"(corrosion_add_cxxbridge\(([^)]*)\))");
    std::smatch call;
    if (!std::regex_search(cmake_text, call, kCall)) { return std::nullopt; }
    std::istringstream words(call[1].str());
    std::set<std::string> files;
    bool in_files = false;
    for (std::string word; words >> word;) {
        if (in_files) { files.insert(word); }
        in_files = in_files || word == "FILES";
    }
    return files;
}

}// namespace

// Corrosion generates a header per listed file, so a bridge file left out has no C++ half to call or link.
TEST(BuildIntegrity, CxxbridgeFilesAreExactlyOxidantsBridgeFiles)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path bridge_src = repo_root / "third_party" / "OxidANT" / "crates" / "cxx_bridge" / "src";
    if (!sourceExists(bridge_src)) { GTEST_SKIP() << "the OxidANT submodule is not checked out here"; }

    const fs::path cmake_path = repo_root / "Src" / "CMakeLists.txt";
    const auto cmake_text = readFileText(cmake_path);
    ASSERT_TRUE(cmake_text.has_value()) << "could not read " << cmake_path.string();
    const auto listed = cxxbridge_files_in_cmake(*cmake_text);
    ASSERT_TRUE(listed.has_value()) << cmake_path.string() << " has no corrosion_add_cxxbridge call";

    const std::set<std::string> declared = oxidant_bridge_files(bridge_src);
    ASSERT_GE(declared.size(), 2U) << "found only " << declared.size() << " bridge file(s) under "
                                   << bridge_src.string() << " - the scan broke";
    EXPECT_EQ(*listed, declared)
      << "corrosion_add_cxxbridge's FILES must name each OxidANT bridge file, and only those";
}

// OxidANT's release profile aborts on a panic, so guarded() caught none and a loader panic ended a Release engine.
TEST(BuildIntegrity, NonDebugBuildsLinkTheBridgeWithAPanicThatUnwinds)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path cargo_path = repo_root / "third_party" / "OxidANT" / "Cargo.toml";
    if (!sourceExists(cargo_path)) { GTEST_SKIP() << "the OxidANT submodule is not checked out here"; }

    const auto cargo = readFileText(cargo_path);
    ASSERT_TRUE(cargo.has_value()) << "could not read " << cargo_path.string();
    static const std::regex kBridgeProfile(R"((^|\n)\[profile\.bridge\]\s*\n([^\[]*))");
    std::smatch table;
    ASSERT_TRUE(std::regex_search(*cargo, table, kBridgeProfile)) << cargo_path.string() << " has no [profile.bridge]";
    const std::string keys = table[2].str();
    EXPECT_TRUE(std::regex_search(keys, std::regex(R"((^|\n)\s*inherits\s*=\s*"release")")))
      << "[profile.bridge] must inherit the release profile";
    EXPECT_TRUE(std::regex_search(keys, std::regex(R"((^|\n)\s*panic\s*=\s*"unwind")")))
      << "[profile.bridge] must unwind, or guarded()'s catch_unwind catches nothing";

    const fs::path cmake_path = repo_root / "Src" / "CMakeLists.txt";
    const auto cmake_text = readFileText(cmake_path);
    ASSERT_TRUE(cmake_text.has_value()) << "could not read " << cmake_path.string();
    static const std::regex kSelection(
      R"(set_property\(\s*TARGET\s+kataglyphis_cxx_bridge\s+PROPERTY\s+)"
      R"(INTERFACE_CORROSION_CARGO_PROFILE\s+"?\$<\$<NOT:\$<CONFIG:Debug>>:bridge>"?\s*\))");
    EXPECT_TRUE(std::regex_search(*cmake_text, kSelection))
      << cmake_path.string() << " must select OxidANT's bridge profile for every configuration but Debug";
}

// A generated bridge header in a module interface or header would rebuild every importer on each bridge edit.
TEST(BuildIntegrity, BridgeHeadersAreIncludedFromCppFilesOnly)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::regex kBridgeInclude(R"((^|\n)\s*#\s*include\s*[<"]oxidant_bridge/)");
    std::vector<std::string> violations;
    std::size_t cpp_includers = 0;
    std::error_code error;
    for (const fs::path &root : { repo_root / "Src", repo_root / "Test" }) {
        for (fs::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            const fs::path &path = it->path();
            const auto extension = path.extension();
            if (!it->is_regular_file(error)
                || (extension != ".cpp" && extension != ".ixx" && extension != ".hpp" && extension != ".h")) {
                continue;
            }
            const auto text = readFileText(path);
            if (!text || !std::regex_search(*text, kBridgeInclude)) { continue; }
            if (extension == ".cpp") {
                ++cpp_includers;
            } else {
                violations.push_back(fs::relative(path, repo_root).generic_string());
            }
        }
    }

    EXPECT_GE(cpp_includers, 2U) << "found only " << cpp_includers << " .cpp file(s) including oxidant_bridge/ headers"
                                 << " - the scan broke";
    EXPECT_TRUE(violations.empty()) << "include oxidant_bridge/ headers from .cpp files only:"
                                    << joinViolations(violations);
}

// One glTF fixture set (plan X5): a GltfTest file OxidANT also holds is a copy, kept only where the library needs it.
TEST(BuildIntegrity, GltfTestHoldsNoCopyOfOxidantsFixturesButTheLibrarysCheckerCube)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path oxidant = repo_root / "third_party" / "OxidANT" / "crates" / "assets" / "tests" / "assets";
    if (!sourceExists(oxidant)) { GTEST_SKIP() << "the OxidANT submodule is not checked out here"; }
    const std::map<std::string, std::string> kKept{
        { "cube_textured.gltf", "the editor library lists it, and two goldens reload it through the library" },
    };

    std::vector<std::string> violations;
    std::size_t scanned = 0;
    std::error_code error;
    for (fs::directory_iterator it(repo_root / "Resources" / "Models" / "GltfTest", error), end; it != end;
      it.increment(error)) {
        if (error) { break; }
        ++scanned;
        const std::string name = it->path().filename().string();
        const fs::path twin = oxidant / name;
        std::error_code twin_error;
        if (!fs::exists(twin, twin_error)) { continue; }
        if (!kKept.contains(name)) {
            violations.push_back(name + ": OxidANT holds it; read it from " + twin.generic_string() + " instead");
        } else if (readFileText(it->path()) != readFileText(twin)) {
            violations.push_back(name + ": differs from OxidANT's copy; the two must stay byte-identical");
        }
    }
    EXPECT_GT(scanned, 5U) << "found only " << scanned << " GltfTest file(s) - the scan broke";
    EXPECT_TRUE(violations.empty()) << joinViolations(violations);
}

// The loader reads no environment (plan X14): the variable is read once, at the engine's start.
TEST(BuildIntegrity, TheMikkTSpaceVariableIsReadOnlyAtTheEngineStart)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path engine = repo_root / "Src" / "GraphicsEngineVulkan";

    std::vector<std::string> readers;
    std::vector<std::string> callers;
    std::error_code error;
    for (fs::recursive_directory_iterator it(engine, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error)) { continue; }
        const auto contents = readFileText(it->path());
        if (!contents) { continue; }
        const std::string relative = fs::relative(it->path(), engine).generic_string();
        if (contents->find("KATAGLYPHIS_MIKKTSPACE_TANGENTS") != std::string::npos) { readers.push_back(relative); }
        if (contents->find("gltfLoadOptionsFromEnvironment()") != std::string::npos) { callers.push_back(relative); }
    }
    const std::vector<std::string> kReaders{ "scene/GltfLoader.cpp", "scene/GltfLoader.ixx" };
    const std::vector<std::string> kCallers{ "app/App.cpp", "scene/GltfLoader.cpp", "scene/GltfLoader.ixx" };
    std::sort(readers.begin(), readers.end());
    std::sort(callers.begin(), callers.end());
    EXPECT_EQ(readers, kReaders) << "only gltfLoadOptionsFromEnvironment() may name the variable";
    EXPECT_EQ(callers, kCallers) << "App::run alone calls gltfLoadOptionsFromEnvironment()";
}

// The bridge's crate graph (plan X4): the hub stager fails a forbidden family or a graph past the measured budget.
TEST(BuildIntegrity, TheBridgeCrateGraphForbidsTheHeavyFamiliesAndHasABudget)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path cmake_path = repo_root / "cmake" / "CargoLicenses.cmake";
    const auto cmake = readFileText(cmake_path);
    ASSERT_TRUE(cmake.has_value()) << "could not read " << cmake_path.string();

    std::vector<std::string> missing;
    for (const std::string pattern :
      { "\"wgpu*\"", "\"naga*\"", "\"winit*\"", "\"egui*\"", "\"tokio*\"", "\"gstreamer*\"", "\"ort-*\"" }) {
        if (cmake->find(pattern) == std::string::npos) { missing.push_back(pattern); }
    }
    // ort itself, unquoted: the bare name, not a prefix of another.
    static const std::regex kOrt(R"(--forbid-package\s+ort(\s|\)))");
    if (!std::regex_search(*cmake, kOrt)) { missing.push_back("ort"); }
    EXPECT_TRUE(missing.empty()) << cmake_path.string() << " no longer forbids:" << joinViolations(missing);
    EXPECT_NE(cmake->find("--max-runtime-packages ${_myproject_cargo_runtime_budget}"), std::string::npos)
      << cmake_path.string() << " passes the stager no runtime package budget";
    EXPECT_NE(cmake->find("${_myproject_cargo_forbidden}"), std::string::npos)
      << cmake_path.string() << " passes the stager no forbidden list";
}
