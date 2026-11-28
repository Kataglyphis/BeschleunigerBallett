// Build integrity: the editor has a control for every setting and binding, and times passes as the Rust GUI.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"
#include "renderer/PathTracingDispatch.hpp"
#include "renderer/PathTracingHistory.hpp"

import kataglyphis.vulkan.command_line;
import kataglyphis.vulkan.gpu_timing;
import kataglyphis.vulkan.gui_renderer_shared_vars;
import kataglyphis.vulkan.gui_scene_shared_vars;
import kataglyphis.vulkan.render_scale;

using namespace Kataglyphis::TestSupport::BuildIntegrity;

namespace {

// Declared member names of a plain C++ struct: the identifier before its initializer, array bound or semicolon.
std::vector<std::string> parse_cpp_struct_member_names(const fs::path &header_path, const std::string &decl)
{
    const auto struct_body = cpp_struct_body(readFileText(header_path).value_or(std::string{}), decl);
    if (!struct_body) { return {}; }
    // A type of words, scopes, templates and pointers first: a static_assert or its continuation never matches.
    static const std::regex kMemberDecl(R"(^\s*[\w:<>,\s\*&]+?\s+(\w+)\s*(=|;|\{|\[))");
    std::vector<std::string> names;
    std::istringstream body_stream(*struct_body);
    std::string line;
    while (std::getline(body_stream, line)) {
        const auto comment_pos = line.find("//");
        if (comment_pos != std::string::npos) { line.resize(comment_pos); }
        std::smatch match;
        if (std::regex_search(line, match, kMemberDecl)) { names.push_back(match[1].str()); }
    }
    return names;
}

}// namespace

// The menu bar's and the inspector's mode controls show the drawn mode, so only a reselectable switch can cancel a pick.
TEST(BuildIntegrity, BothRenderModeControlsShareTheReselectableSwitch)
{
    const fs::path gui_path = repoRoot() / "Src" / "GraphicsEngineVulkan" / "gui" / "GUI.cpp";
    const auto lines = readFileLines(gui_path);
    ASSERT_TRUE(lines.has_value()) << "could not open " << gui_path.string();

    std::size_t switch_calls = 0;
    std::vector<std::string> segmented_mode_controls;
    for (const std::string &raw : *lines) {
        const std::string line = strip_line_comment(raw);
        if (line.find("renderModeSwitch(\"##renderer_switch\"") != std::string::npos
            || line.find("renderModeSwitch(\"##render_mode\"") != std::string::npos) {
            ++switch_calls;
        }
        if (line.find("studioSegmented(") != std::string::npos && line.find("kRenderModeLabels") != std::string::npos) {
            segmented_mode_controls.push_back(line);
        }
    }
    EXPECT_EQ(switch_calls, 2U)
      << gui_path.string() << " draws the menu bar's or the inspector's render-mode control outside renderModeSwitch";
    ASSERT_EQ(segmented_mode_controls.size(), 1U)
      << gui_path.string()
      << " builds a render-mode control outside renderModeSwitch:" << joinViolations(segmented_mode_controls);
    EXPECT_NE(segmented_mode_controls.front().find(", true)"), std::string::npos)
      << "renderModeSwitch's control is not reselectable, so Raster cannot cancel a pick that waits for a TLAS";
}

// ImGui's colour picker leaves a typed RGB or HSV value unclamped, so an editor colour goes through studioColorEdit3.
TEST(BuildIntegrity, EditorColourEditsHoldTheirRange)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path widgets = repo_root / "Src" / "shared" / "imgui" / "KataglyphisImGuiWidgets.ixx";
    ASSERT_TRUE(sourceExists(widgets)) << "missing " << widgets.string();

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(repo_root / "Src", error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || fs::equivalent(path, widgets, error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx" && path.extension() != ".hpp") { continue; }
        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        for (std::size_t i = 0; i < lines->size(); ++i) {
            const std::string line = strip_line_comment((*lines)[i]);
            if (line.find("ImGui::ColorEdit") != std::string::npos
                || line.find("ImGui::ColorPicker") != std::string::npos) {
                violations.push_back(fs::relative(path, repo_root).generic_string() + ":" + std::to_string(i + 1));
            }
        }
    }
    EXPECT_TRUE(violations.empty()) << "a colour edit outside studioColorEdit3 takes a typed value outside [0, 1]:"
                                    << joinViolations(violations);
}

namespace {

// GUI.cpp (or <consumer_rel>) must name every member of a shared-vars struct, or the member has a reason in <exempt>.
void expect_every_member_has_a_gui_control(const std::string &struct_name,
  const std::string &header_rel,
  const std::string &gui_prefix,
  std::size_t min_members,
  const std::map<std::string, std::string> &exempt,
  const std::string &consumer_rel = "Src/GraphicsEngineVulkan/gui/GUI.cpp")
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path header_path = repo_root / fs::path(header_rel);
    const fs::path gui_path = repo_root / fs::path(consumer_rel);
    const auto gui_text = readFileText(gui_path);
    ASSERT_TRUE(gui_text.has_value()) << "could not open " << gui_path.string();

    const std::vector<std::string> members = parse_cpp_struct_member_names(header_path, "struct " + struct_name);
    // A regex that silently matches nothing would pass every member below.
    ASSERT_GE(members.size(), min_members) << "parsed only " << members.size() << " member(s) of " << struct_name
                                           << " from " << header_path.string() << " - the parse broke, not the GUI";

    std::vector<std::string> missing;
    for (const auto &member : members) {
        if (exempt.contains(member)) { continue; }
        if (gui_text->find(gui_prefix + member) == std::string::npos) { missing.push_back(member); }
    }
    std::vector<std::string> stale;
    for (const auto &[name, reason] : exempt) {
        if (std::find(members.begin(), members.end(), name) == members.end()) {
            stale.push_back(name + " (" + reason + ")");
        }
    }

    EXPECT_TRUE(missing.empty()) << gui_path.string() << " does not read/write the following " << struct_name
                                 << " member(s) - add a control, or an exemption with its reason in this test:"
                                 << joinViolations(missing);
    EXPECT_TRUE(stale.empty()) << "exempted member(s) no longer in " << struct_name
                               << " - delete the exemption:" << joinViolations(stale);
}

}// namespace

// Every tunable scene var needs a GUI control; the member list is the header's, so a new member is checked on arrival.
TEST(BuildIntegrity, EveryTunableGuiSceneVarHasAControl)
{
    const std::map<std::string, std::string> kExempt{
        { "shadow_resolution_changed", "a latch the resolution combo raises for the renderer, not a tunable" },
        { "model_reload_requested", "a latch the model picker raises, not a tunable" },
        { "model_transform_changed", "a latch the transform controls raise, not a tunable" },
        { "selected_model_index", "the model picker's selection, not a tunable" },
        { "available_shadow_map_resolutions", "the resolution combo's labels, a constant table" },
    };
    expect_every_member_has_a_gui_control("GUISceneSharedVars",
      "Src/GraphicsEngineVulkan/scene/GUISceneSharedVars.ixx",
      "guiSceneSharedVars.",
      20,
      kExempt);
}

// A scene var OxidANT's settings also carry is compared with them; the rest name why not (plan B10).
TEST(BuildIntegrity, EveryGuiSceneVarIsComparedWithOxidantsSettingsOrExempt)
{
    const std::map<std::string, std::string> kExempt{
        { "shadow_resolution_changed", "a latch the resolution combo raises for the renderer, not a setting" },
        { "model_reload_requested", "a latch the model picker raises, not a setting" },
        { "model_transform_changed", "a latch the transform controls raise, not a setting" },
        { "selected_model_index", "the model picker's selection, not a setting" },
        { "available_shadow_map_resolutions",
          "the combo's labels; the texels are compared through kShadowMapResolutions" },
        { "skybox_enabled", "OxidANT's settings carry no skybox toggle yet" },
        { "camera_fov", "the camera, which OxidANT's settings do not carry" },
        { "camera_fly_speed", "the camera; TheFlySpeedTakesOxidantsDefaultAndRange compares it with render_core's" },
        { "model_position", "the Model tab's pose, compared through parity_model_transform (rendererParitySuite.cpp)" },
        { "model_rotation", "the Model tab's pose, compared through parity_model_transform (rendererParitySuite.cpp)" },
    };
    expect_every_member_has_a_gui_control("GUISceneSharedVars",
      "Src/GraphicsEngineVulkan/scene/GUISceneSharedVars.ixx",
      "defaults.",
      20,
      kExempt,
      "Test/commit/VulkanEngine/sceneSettingsDriftSuite.cpp");
}

// The renderer's shared vars had no gate; all of them are read or written by the GUI today, and this keeps it so.
TEST(BuildIntegrity, EveryGuiRendererSharedVarHasAControl)
{
    expect_every_member_has_a_gui_control("GUIRendererSharedVars",
      "Src/GraphicsEngineVulkan/renderer/GUIRendererSharedVars.ixx",
      "guiRendererSharedVars.",
      8,
      {});
}

// The renderer reports through RendererFeedback alone; a field the editor never shows is output nobody reads.
TEST(BuildIntegrity, EveryRendererFeedbackFieldReachesTheEditor)
{
    const std::map<std::string, std::string> kExempt{
        { "publishedModelIndex", "applyRendererFeedback moves the editor's selection to it; nothing displays it" },
        { "publishedSun", "applyRendererFeedback writes it into the editor's Sun, which the Sun inspector shows" },
        { "publishedShadowDistance", "applyRendererFeedback writes it into the Sun tab's shadow Distance slider" },
        { "publishedFov", "applyRendererFeedback writes it into the Camera tab's Field of view slider" },
        { "publishedFlySpeed", "applyRendererFeedback writes it into the Camera tab's Fly speed slider" },
    };
    expect_every_member_has_a_gui_control("RendererFeedback",
      "Src/GraphicsEngineVulkan/renderer/GUIRendererSharedVars.ixx",
      "rendererFeedback.",
      5,
      kExempt);
}

// The renderer gets the app's settings by const reference; a GUI import or pointer would couple it to the editor again.
TEST(BuildIntegrity, RendererAndItsStagesDoNotReachTheEditor)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path engine_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(sourceExists(engine_root)) << "missing " << engine_root.string();

    static const std::regex kGuiImport(R"((^|\n)\s*(export\s+)?import\s+kataglyphis\.vulkan\.gui\s*;)");
    // The settings owner takes them mutably to spend the requests; everywhere else the renderer only reads them.
    const fs::path settings_owner = engine_root / "renderer" / "RenderSettings.ixx";
    const std::array<std::string, 3> kSettingsTypes = {
        "RenderSettings", "GUISceneSharedVars", "GUIRendererSharedVars"
    };

    std::vector<std::string> violations;
    std::size_t scanned = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(engine_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx" && path.extension() != ".hpp") { continue; }
        const std::string relative = fs::relative(path, engine_root).generic_string();
        if (relative.starts_with("gui/") || relative.starts_with("app/")) { continue; }
        const auto contents = readFileText(path);
        if (!contents) { continue; }
        ++scanned;

        if (std::regex_search(*contents, kGuiImport)) { violations.push_back(relative + ": imports the GUI module"); }
        if (contents->find("Frontend::GUI") != std::string::npos) {
            violations.push_back(relative + ": names Frontend::GUI");
        }
        if (path == settings_owner) { continue; }
        for (const std::string &type : kSettingsTypes) {
            const std::regex reference(R"((^|[^\w:])(const\s+)?(?:\w+::)*)" + type + R"(\s*&)");
            for (auto match = std::sregex_iterator(contents->begin(), contents->end(), reference);
              match != std::sregex_iterator();
              ++match) {
                if (!(*match)[2].matched) { violations.push_back(relative + ": takes a mutable " + type + " &"); }
            }
        }
    }

    // A wrong root or extension filter would scan nothing and pass.
    EXPECT_GE(scanned, 50U) << "scanned only " << scanned << " engine source(s) under " << engine_root.string();
    EXPECT_TRUE(violations.empty()) << "only app/ and gui/ may reach the editor, and the renderer reads settings "
                                       "by const reference (RenderSettings, applyRendererFeedback):"
                                    << joinViolations(violations);
}

// The key-bindings panel is a hand-written literal, so every bound key and button must map to prose in it.
TEST(BuildIntegrity, KeyBindingsPanelListsEveryBinding)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path camera_controller_path = repo_root / "Src" / "shared" / "frontend" / "CameraController.ixx";
    const fs::path window_input_callbacks_path = repo_root / "Src" / "shared" / "frontend" / "WindowInputCallbacks.ixx";
    const fs::path panel_path = repo_root / "Src" / "shared" / "frontend" / "CommonGuiPanels.ixx";

    const auto camera_controller_text = readFileText(camera_controller_path);
    ASSERT_TRUE(camera_controller_text.has_value()) << "could not open " << camera_controller_path.string();
    const auto window_input_callbacks_text = readFileText(window_input_callbacks_path);
    ASSERT_TRUE(window_input_callbacks_text.has_value()) << "could not open " << window_input_callbacks_path.string();
    const auto panel_text = readFileText(panel_path);
    ASSERT_TRUE(panel_text.has_value()) << "could not open " << panel_path.string();

    static const std::regex kKeyOrButtonPattern(R"(GLFW_(?:KEY|MOUSE_BUTTON)_[A-Z0-9_]+)");

    std::set<std::string> bindings;
    for (const auto *text : { &*camera_controller_text, &*window_input_callbacks_text }) {
        for (auto it = std::sregex_iterator(text->begin(), text->end(), kKeyOrButtonPattern);
          it != std::sregex_iterator();
          ++it) {
            bindings.insert(it->str());
        }
    }
    ASSERT_FALSE(bindings.empty()) << "found no GLFW_KEY_*/GLFW_MOUSE_BUTTON_* usages in "
                                   << camera_controller_path.string() << " or " << window_input_callbacks_path.string();

    // Symbol -> the exact substring the panel literal must contain for it.
    static const std::map<std::string, std::string> kSymbolToProse{
        { "GLFW_KEY_W", "W" },
        { "GLFW_KEY_A", "A" },
        { "GLFW_KEY_S", "S" },
        { "GLFW_KEY_D", "D" },
        { "GLFW_KEY_Q", "Q" },
        { "GLFW_KEY_E", "E" },
        { "GLFW_KEY_ESCAPE", "ESC" },
        { "GLFW_MOUSE_BUTTON_RIGHT", "Right mouse" },
    };

    // Bound inputs deliberately not user-facing, each with a reason; none today.
    static const std::set<std::string> kExemptSymbols{};

    std::vector<std::string> unmapped;
    std::vector<std::string> missing_from_panel;
    for (const auto &symbol : bindings) {
        if (kExemptSymbols.contains(symbol)) { continue; }
        const auto mapping = kSymbolToProse.find(symbol);
        if (mapping == kSymbolToProse.end()) {
            unmapped.push_back(symbol);
            continue;
        }
        if (panel_text->find(mapping->second) == std::string::npos) { missing_from_panel.push_back(symbol); }
    }

    EXPECT_TRUE(unmapped.empty()) << "found GLFW binding(s) with no prose mapping in this test's kSymbolToProse "
                                     "table - add one, or if the binding is deliberately not user-facing, add it "
                                     "to kExemptSymbols with a reason:"
                                  << joinViolations(unmapped);
    EXPECT_TRUE(missing_from_panel.empty())
      << panel_path.string()
      << " does not mention the following key binding(s) - add them to renderCommonKeyBindings():"
      << joinViolations(missing_from_panel);
}

// Compare-RendererTimings lines the passes up by name and nesting; both GUIs smooth over one window of frames.
TEST(BuildIntegrity, GpuTimingMatchesTheRustRenderersTaxonomy)
{
    namespace FrontendShared = Kataglyphis::VulkanRendererInternals::FrontendShared;
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path rust_path =
      repo_root / "third_party" / "OxidANT" / "crates" / "webgpu_renderer" / "src" / "render" / "gpu_timing.rs";
    if (!sourceExists(rust_path)) { GTEST_SKIP() << "the OxidANT submodule is not checked out here"; }
    const auto rust = readFileText(rust_path);
    ASSERT_TRUE(rust.has_value()) << "could not read " << rust_path.string();

    std::smatch name_fn;
    ASSERT_TRUE(std::regex_search(*rust, name_fn, std::regex(R"(fn name\(self\)[^{]*\{\s*match self \{([^}]*)\})")))
      << rust_path.string() << " has no TimedPass::name() match";
    std::set<std::string> rust_names;
    const std::string arms = name_fn[1].str();
    static const std::regex kArm(R"re(=>\s*"([A-Za-z]+)")re");
    for (std::sregex_iterator it(arms.begin(), arms.end(), kArm), end; it != end; ++it) {
        rust_names.insert((*it)[1].str());
    }
    ASSERT_GE(rust_names.size(), 10U) << "parsed only " << rust_names.size() << " Rust pass names - the scan broke";

    // The C++ renderer's own scopes; any other pass must carry the Rust renderer's name for the same work.
    static const std::set<std::string> kCppOnly = { "Post", "CloudUpsample" };
    for (const char *name : FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES) {
        if (kCppOnly.contains(name)) {
            EXPECT_FALSE(rust_names.contains(name)) << name << " is a Rust pass too: drop it from kCppOnly";
        } else {
            EXPECT_TRUE(rust_names.contains(name)) << "C++ times " << name << ", which no Rust TimedPass is named";
        }
    }
    for (const char *shared : { "Forward", "GBuffer", "DeferredLighting", "RayTrace", "PathTrace" }) {
        EXPECT_NE(std::ranges::find_if(FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES,
                    [shared](const char *name) { return std::string_view(name) == shared; }),
          FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES.end())
          << "the C++ renderer no longer times " << shared << " inside Main";
    }

    // A frame sums its top-level passes, so a shared scope must nest in the same parent in both renderers.
    std::smatch nested_fn;
    ASSERT_TRUE(
      std::regex_search(*rust, nested_fn, std::regex(R"(fn nested_in\(self\)[^{]*\{\s*match self \{([^}]*)\})")))
      << rust_path.string() << " has no TimedPass::nested_in() match";
    std::map<std::string, std::string> rust_parents;
    const std::string nested_arms = nested_fn[1].str();
    static const std::regex kNestedArm(R"re(((?:\|?\s*TimedPass::\w+\s*)+)=>\s*Some\(TimedPass::(\w+)\))re");
    static const std::regex kVariant(R"re(TimedPass::(\w+))re");
    for (std::sregex_iterator arm(nested_arms.begin(), nested_arms.end(), kNestedArm), end; arm != end; ++arm) {
        const std::string children = (*arm)[1].str();
        for (std::sregex_iterator child(children.begin(), children.end(), kVariant); child != end; ++child) {
            rust_parents[(*child)[1].str()] = (*arm)[2].str();
        }
    }
    ASSERT_FALSE(rust_parents.empty()) << rust_path.string() << " nests no pass - the scan broke";
    for (std::size_t pass = 0; pass < FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES.size(); ++pass) {
        const std::string name = FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES[pass];
        if (kCppOnly.contains(name)) { continue; }
        const FrontendShared::GpuTimedPass parent = FrontendShared::GPU_TIMED_PASS_PARENTS[pass];
        const std::string cpp_parent =
          parent == FrontendShared::GpuTimedPass::Count
            ? std::string()
            : FrontendShared::GPU_TIMED_PASS_EXPORT_NAMES[static_cast<std::size_t>(parent)];
        const auto found = rust_parents.find(name);
        EXPECT_EQ(found == rust_parents.end() ? std::string() : found->second, cpp_parent)
          << name << " nests in another parent in the Rust renderer";
    }

    std::smatch window;
    ASSERT_TRUE(std::regex_search(*rust, window, std::regex(R"(const AVERAGE_WINDOW: usize = (\d+);)")))
      << rust_path.string() << " has no AVERAGE_WINDOW";
    EXPECT_EQ(std::to_string(Kataglyphis::GpuTimingSubsystem::GpuPassAverage::WINDOW), window[1].str())
      << "the C++ GUI averages its pass times over another window than the Rust renderer's";
}

// The post settings take the Rust renderer's defaults and its Renderer tab's ranges; a change there must reach here.
TEST(BuildIntegrity, PostSettingsTakeTheRustRenderersDefaultsAndRanges)
{
    namespace FrontendShared = Kataglyphis::VulkanRendererInternals::FrontendShared;
    const fs::path renderer = repoRoot() / "third_party" / "OxidANT" / "crates" / "webgpu_renderer" / "src";
    if (!sourceExists(renderer)) { GTEST_SKIP() << "the OxidANT submodule is not checked out here"; }
    const auto host = readFileText(renderer / "ui" / "editor" / "host.rs");
    const auto inspector = readFileText(renderer / "ui" / "editor" / "inspector" / "renderer.rs");
    ASSERT_TRUE(host.has_value() && inspector.has_value()) << "could not read OxidANT's editor sources";

    // `field: value,` in RendererSettings::default(), the first rows after its impl.
    const std::size_t default_impl = host->find("impl Default for RendererSettings");
    ASSERT_NE(default_impl, std::string::npos) << "host.rs no longer implements Default for RendererSettings";
    const std::string defaults_text = host->substr(default_impl);
    const auto rust_default = [&](const std::string &field) -> std::string {
        std::smatch match;
        const std::regex row("\\n\\s*" + field + R"(:\s*([\w.]+),)");
        return std::regex_search(defaults_text, match, row) ? match[1].str() : std::string{};
    };
    // `FillSlider::new(&mut settings.field, lo..=hi)` in the Renderer tab.
    const auto rust_range = [&](const std::string &field) -> std::pair<double, double> {
        std::smatch match;
        const std::regex slider("settings\\." + field + R"(,\s*(-?[\d.]+)\.\.=(-?[\d.]+)\))");
        if (!std::regex_search(*inspector, match, slider)) { return { 0.0, -1.0 }; }
        return { std::stod(match[1].str()), std::stod(match[2].str()) };
    };

    const FrontendShared::GUIRendererSharedVars defaults{};
    EXPECT_EQ(rust_default("auto_exposure"), defaults.auto_exposure ? "true" : "false");
    // Compared as floats: the Rust literal is an f32, the C++ constant a float.
    const std::vector<std::tuple<std::string, float, float, float>> floats = {
        { "exposure_ev", defaults.exposure_ev, FrontendShared::kMinExposureEv, FrontendShared::kMaxExposureEv },
        { "auto_exposure_speed",
          defaults.auto_exposure_speed,
          FrontendShared::kMinAutoExposureSpeed,
          FrontendShared::kMaxAutoExposureSpeed },
        { "bloom_strength", defaults.bloom_strength, 0.0F, FrontendShared::kMaxBloomStrength },
        { "ssao_strength", defaults.ssao_strength, 0.0F, FrontendShared::kMaxSsaoStrength },
    };
    for (const auto &[field, value, lowest, highest] : floats) {
        const std::string rust = rust_default(field);
        ASSERT_FALSE(rust.empty()) << "RendererSettings::default() sets no " << field << " - the scan broke";
        EXPECT_FLOAT_EQ(std::stof(rust), value) << field << "'s default";
        const auto [rust_lowest, rust_highest] = rust_range(field);
        ASSERT_LE(rust_lowest, rust_highest) << "the Renderer tab has no " << field << " slider - the scan broke";
        EXPECT_FLOAT_EQ(static_cast<float>(rust_lowest), lowest) << field << "'s lowest value";
        EXPECT_FLOAT_EQ(static_cast<float>(rust_highest), highest) << field << "'s highest value";
    }

    // NaN and the infinities fall back to the default, the rest clamps into the range.
    EXPECT_EQ(FrontendShared::sanitizePostSetting(std::nanf(""), -4.0F, 4.0F, 0.5F), 0.5F);
    EXPECT_EQ(FrontendShared::sanitizePostSetting(-std::numeric_limits<float>::infinity(), -4.0F, 4.0F, 0.5F), 0.5F);
    EXPECT_EQ(FrontendShared::sanitizePostSetting(9.0F, -4.0F, 4.0F, 0.5F), 4.0F);
    EXPECT_EQ(FrontendShared::sanitizePostSetting(-1.5F, -4.0F, 4.0F, 0.5F), -1.5F);
}

// The path tracer takes the Rust tracer's dispatch budget, its editor's defaults and furnace, and its PATH TRACING rows.
TEST(BuildIntegrity, PathTracingTakesTheRustTracersPlanDefaultsAndRows)
{
    const fs::path renderer = repoRoot() / "third_party" / "OxidANT" / "crates" / "webgpu_renderer" / "src";
    if (!sourceExists(renderer)) { GTEST_SKIP() << "the OxidANT submodule is not checked out here"; }
    const fs::path modes = renderer / "render" / "tracer" / "modes";
    const auto dispatch = readFileText(modes / "dispatch.rs");
    const auto settings = readFileText(modes / "settings.rs");
    const auto inspector = readFileText(renderer / "ui" / "editor" / "inspector" / "renderer.rs");
    const fs::path gui_path = repoRoot() / "Src" / "GraphicsEngineVulkan" / "gui" / "GUI.cpp";
    const auto gui = readFileText(gui_path);
    ASSERT_TRUE(dispatch && settings && inspector) << "could not read OxidANT's tracer and editor sources";
    ASSERT_TRUE(gui.has_value()) << "could not open " << gui_path.string();

    std::smatch match;
    ASSERT_TRUE(std::regex_search(*dispatch, match, std::regex(R"(pub const TILE_SIZE: u32 = (\d+);)")))
      << "dispatch.rs has no TILE_SIZE - the scan broke";
    EXPECT_EQ(match[1].str(), std::to_string(Kataglyphis::kPathTracingTileSize)) << "the tile side";
    ASSERT_TRUE(std::regex_search(*dispatch, match, std::regex(R"(pub const DISPATCH_SEGMENTS: u64 = 1 << (\d+);)")))
      << "dispatch.rs has no DISPATCH_SEGMENTS - the scan broke";
    EXPECT_EQ(uint64_t{ 1 } << std::stoul(match[1].str()), Kataglyphis::kPathTracingDispatchSegments)
      << "the segment budget of one dispatch";

    // The ranges in settings.rs against the C++ sliders, the defaults against GUIRendererSharedVars'.
    const std::size_t trace_impl = settings->find("impl Default for TraceSettings");
    ASSERT_NE(trace_impl, std::string::npos) << "settings.rs no longer implements Default for TraceSettings";
    const std::string trace_defaults = settings->substr(trace_impl);
    const Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars defaults{};
    const std::vector<std::tuple<std::string, std::string, std::string, int>> quality = {
        { "SAMPLES_PER_PIXEL", "samples_per_pixel", "pathTracingSamplesPerPixel", defaults.pathTracingSamplesPerPixel },
        { "MAX_BOUNCES", "max_bounces", "pathTracingMaxBounces", defaults.pathTracingMaxBounces },
    };
    for (const auto &[range, field, member, cpp_default] : quality) {
        ASSERT_TRUE(
          std::regex_search(*settings, match, std::regex(range + R"(: RangeInclusive<u32> = (\d+)\.\.=(\d+);)")))
          << "settings.rs has no " << range << " - the scan broke";
        const std::string rust_range = match[1].str() + ", " + match[2].str();
        EXPECT_TRUE(
          std::regex_search(*gui, std::regex("&guiRendererSharedVars\\." + member + ",\\s*" + rust_range + "\\)")))
          << "the editor's " << member << " slider does not span the Rust range " << rust_range;
        ASSERT_TRUE(std::regex_search(trace_defaults, match, std::regex("\\n\\s*" + field + R"(:\s*(\d+),)")))
          << "TraceSettings::default() sets no " << field << " - the scan broke";
        EXPECT_EQ(match[1].str(), std::to_string(cpp_default)) << field << "'s default";
    }

    const std::size_t furnace_impl = settings->find("impl Default for FurnaceSettings");
    ASSERT_NE(furnace_impl, std::string::npos) << "settings.rs no longer implements Default for FurnaceSettings";
    const std::string furnace_text = settings->substr(furnace_impl);
    const Kataglyphis::VulkanRendererInternals::PathTracingFurnace furnace{};
    for (const auto &[field, value] : { std::pair{ "radiance", furnace.radiance },
           std::pair{ "metallic", furnace.metallic },
           std::pair{ "roughness", furnace.roughness } }) {
        ASSERT_TRUE(
          std::regex_search(furnace_text, match, std::regex(std::string(R"(\n\s*)") + field + R"(:\s*([\d.]+),)")))
          << "FurnaceSettings::default() sets no " << field << " - the scan broke";
        EXPECT_FLOAT_EQ(std::stof(match[1].str()), value) << "the White furnace's " << field;
    }
    EXPECT_FALSE(defaults.pathTracingFurnace) << "TraceSettings::default() starts without the furnace";

    // Every row of Rust's path_tracing(), its label and its hover text, and the restart button.
    const std::size_t section = inspector->find("fn path_tracing(");
    ASSERT_NE(section, std::string::npos) << "renderer.rs has no path_tracing() section - the scan broke";
    const std::string rows = inspector->substr(section, inspector->find("\nfn ", section + 1) - section);
    const std::regex row(R"re(property_row\(\s*ui,\s*"([^"]+)",\s*Some\("([^"]+)"\))re");
    const auto literal = [](const std::string &text) {
        return std::regex_replace(text, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)");
    };
    std::size_t rows_found = 0;
    for (auto it = std::sregex_iterator(rows.begin(), rows.end(), row); it != std::sregex_iterator(); ++it) {
        ++rows_found;
        const std::string label = (*it)[1].str();
        const std::regex cpp_row(
          "studioPropertyLabel\\(\\s*\"" + literal(label) + "\",\\s*\"" + literal((*it)[2].str()) + "\"\\)");
        EXPECT_TRUE(std::regex_search(*gui, cpp_row)) << "the editor's PATH TRACING has no '" << label
                                                      << "' row with the Rust hover text '" << (*it)[2].str() << "'";
    }
    EXPECT_EQ(rows_found, 4U) << "renderer.rs's path_tracing() changed its rows - the scan or this list needs updating";
    ASSERT_NE(rows.find("Button::new(\"Restart accumulation\")"), std::string::npos) << "the Rust restart button moved";
    EXPECT_NE(gui->find("ImGui::Button(\"Restart accumulation\""), std::string::npos)
      << "the editor has no Restart accumulation button";
    // The labels alone could front other settings: the toggle must set the furnace, the button the one-shot restart.
    EXPECT_TRUE(std::regex_search(*gui,
      std::regex(
        R"re("White furnace",[^;]*\);\s*studioToggle\("[^"]*",\s*&guiRendererSharedVars\.pathTracingFurnace\);)re")))
      << "the editor's White furnace row does not toggle pathTracingFurnace";
    EXPECT_TRUE(std::regex_search(*gui,
      std::regex(
        R"re(ImGui::Button\("Restart accumulation"[^{]*\{\s*guiRendererSharedVars\.pathTracingRestartRequested = true;)re")))
      << "the editor's Restart accumulation button does not request pathTracingRestartRequested";
}

// Rust's render_scale() rows, their hover texts, bindings and disabled states, and its two texts with values filled in.
TEST(BuildIntegrity, TheRenderScaleRowsAndTextsAreTheRustEditors)
{
    const fs::path editor =
      repoRoot() / "third_party" / "OxidANT" / "crates" / "webgpu_renderer" / "src" / "ui" / "editor";
    if (!sourceExists(editor)) { GTEST_SKIP() << "the OxidANT submodule is not checked out here"; }
    const auto inspector = readFileText(editor / "inspector" / "renderer.rs");
    const auto chip = readFileText(editor / "chip.rs");
    const auto gui = readFileText(repoRoot() / "Src" / "GraphicsEngineVulkan" / "gui" / "GUI.cpp");
    ASSERT_TRUE(inspector && chip && gui) << "could not read the editors' sources";
    const auto section_of = [](const std::string &text, const std::string &head) {
        const std::size_t start = text.find(head);
        return start == std::string::npos ? std::string{} : text.substr(start, text.find("\n}", start) - start);
    };
    const std::string rows = section_of(*inspector, "fn render_scale(");
    ASSERT_FALSE(rows.empty()) << "renderer.rs has no render_scale() section - the scan broke";

    const std::regex row(R"re(property_row\(\s*ui,\s*"([^"]+)",\s*Some\("([^"]+)"\))re");
    const auto literal = [](const std::string &text) {
        return std::regex_replace(text, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)");
    };
    std::vector<std::string> labels;
    for (auto it = std::sregex_iterator(rows.begin(), rows.end(), row); it != std::sregex_iterator(); ++it) {
        labels.push_back((*it)[1].str());
        const std::regex cpp_row(
          "studioPropertyLabel\\(\\s*\"" + literal(labels.back()) + "\",\\s*\"" + literal((*it)[2].str()) + "\"\\)");
        EXPECT_TRUE(std::regex_search(*gui, cpp_row)) << "the Renderer tab has no '" << labels.back()
                                                      << "' row with the Rust hover text '" << (*it)[2].str() << "'";
    }
    EXPECT_EQ(labels, (std::vector<std::string>{ "Render scale", "Auto scale", "Target" }))
      << "renderer.rs's render_scale() changed its rows - the scan or this list needs updating";

    // Auto greys the scale out and the target in, in both editors; each row writes its own setting at Rust's format.
    EXPECT_TRUE(std::regex_search(rows, std::regex(R"re(enabled_ui\(ui, !scale\.auto,[^}]*"Render scale")re")));
    EXPECT_TRUE(std::regex_search(rows, std::regex(R"re(enabled_ui\(ui, scale\.auto,[^}]*"Target")re")));
    EXPECT_TRUE(std::regex_search(rows, std::regex(R"re(RENDER_SCALE_RANGE\)\.decimals\(2\))re")));
    EXPECT_TRUE(
      std::regex_search(rows, std::regex(R"re(TARGET_FPS_RANGE\)\s*\.decimals\(0\)\s*\.suffix\(" FPS"\))re")));
    EXPECT_TRUE(std::regex_search(*gui,
      std::regex(
        R"re(studioBeginDisabled\(guiRendererSharedVars\.render_scale_auto\);\s*studioPropertyLabel\("Render scale",\s*"[^"]*"\);\s*studioSlider\("[^"]*",\s*&guiRendererSharedVars\.render_scale,\s*ranges\.minScale,\s*ranges\.maxScale,\s*"%\.2f"\);)re")))
      << "the Render scale row does not slide render_scale over Rust's range, greyed out under Auto";
    EXPECT_TRUE(std::regex_search(*gui,
      std::regex(
        R"re("Auto scale",\s*"[^"]*"\);\s*studioToggle\("[^"]*",\s*&guiRendererSharedVars\.render_scale_auto\);)re")))
      << "the Auto scale row does not toggle render_scale_auto";
    EXPECT_TRUE(std::regex_search(*gui,
      std::regex(
        R"re(studioBeginDisabled\(!guiRendererSharedVars\.render_scale_auto\);\s*studioPropertyLabel\("Target",\s*"[^"]*"\);\s*studioSlider\("[^"]*",\s*&guiRendererSharedVars\.render_scale_target_fps,\s*ranges\.minTargetFps,\s*ranges\.maxTargetFps,\s*"%\.0f FPS"\);)re")))
      << "the Target row does not slide render_scale_target_fps over Rust's range, live only under Auto";

    // Rust's format strings with the C++ helpers' values: a reworded or reordered text fails here.
    const auto filled = [](std::string format, const std::map<std::string, std::string> &values) {
        for (const auto &[name, value] : values) {
            for (std::size_t at = format.find("{" + name + "}"); at != std::string::npos;
              at = format.find("{" + name + "}")) {
                format.replace(at, name.size() + 2U, value);
            }
        }
        return format;
    };
    const std::map<std::string, std::string> values = {
        { "inner_w", "1200" }, { "inner_h", "675" }, { "width", "1600" }, { "height", "900" }, { "percent", "75" }
    };
    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars scaled{};
    scaled.render_scale = 0.75F;
    const Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars full{};
    const std::regex format(R"re(format!\(\s*"([^"]+)")re");
    std::smatch match;
    ASSERT_TRUE(std::regex_search(rows, match, format)) << "render_scale() formats no Renders line - the scan broke";
    EXPECT_EQ(filled(match[1].str(), values), Kataglyphis::renderScaleSummary(scaled, 1600, 900));
    const std::string extent_text = section_of(*chip, "fn extent_text(");
    std::vector<std::string> chip_formats;
    for (auto it = std::sregex_iterator(extent_text.begin(), extent_text.end(), format); it != std::sregex_iterator();
      ++it) {
        chip_formats.push_back((*it)[1].str());
    }
    ASSERT_EQ(chip_formats.size(), 2U) << "chip.rs's extent_text() changed its formats - the scan broke";
    EXPECT_EQ(filled(chip_formats[0], values), Kataglyphis::renderScaleChipText(full, 1600, 900));
    EXPECT_EQ(filled(chip_formats[1], values), Kataglyphis::renderScaleChipText(scaled, 1600, 900));
}

// The fly speed is OxidANT's (render_core's fly_speed.rs, which both renderers' framing fits to a scene).
TEST(BuildIntegrity, TheFlySpeedTakesOxidantsDefaultAndRange)
{
    const fs::path source = repoRoot() / "third_party" / "OxidANT" / "crates" / "render_core" / "src" / "fly_speed.rs";
    if (!sourceExists(source)) { GTEST_SKIP() << "the OxidANT submodule is not checked out here"; }
    const auto rust = readFileText(source);
    ASSERT_TRUE(rust.has_value()) << "could not read " << source.string();
    const auto number = [](std::string text) {
        text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
        return std::stof(text);
    };
    std::smatch match;
    ASSERT_TRUE(std::regex_search(*rust, match, std::regex(R"(pub const DEFAULT_SPEED: f32 = ([\d_.]+);)")))
      << source.string() << " has no DEFAULT_SPEED";
    EXPECT_FLOAT_EQ(number(match[1].str()), kDefaultCameraFlySpeed);
    ASSERT_TRUE(std::regex_search(
      *rust, match, std::regex(R"(pub const SPEED_RANGE: RangeInclusive<f32> = ([\d_.]+)\.\.=([\d_.]+);)")))
      << source.string() << " has no SPEED_RANGE";
    EXPECT_FLOAT_EQ(number(match[1].str()), kMinCameraFlySpeed);
    EXPECT_FLOAT_EQ(number(match[2].str()), kMaxCameraFlySpeed);
}

// The Rust editor's shortcut keys, F12 among them, are the C++ editor's: handled, and listed with Rust's words.
TEST(BuildIntegrity, TheEditorsShortcutKeysAreTheRustEditors)
{
    const fs::path keys_path =
      repoRoot() / "third_party" / "OxidANT" / "crates" / "webgpu_renderer" / "src" / "ui" / "editor" / "keys.rs";
    if (!sourceExists(keys_path)) { GTEST_SKIP() << "the OxidANT submodule is not checked out here"; }
    const auto rust = readFileText(keys_path);
    const auto gui = readFileText(repoRoot() / "Src" / "GraphicsEngineVulkan" / "gui" / "GUI.cpp");
    const auto panel = readFileText(repoRoot() / "Src" / "shared" / "frontend" / "CommonGuiPanels.ixx");
    ASSERT_TRUE(rust.has_value() && gui.has_value() && panel.has_value()) << "could not read the key sources";

    // `keys: &["F12"], action: "...", shortcut: Some((Key::F12, ...))`; ESC's words differ by the hosts' focus.
    static const std::regex kRow(R"rx(keys: &\["(F\d+)"\],\s*action: "([^"]+)",\s*shortcut: Some\(\(Key::F\d+)rx");
    std::size_t rows = 0;
    for (auto it = std::sregex_iterator(rust->begin(), rust->end(), kRow); it != std::sregex_iterator(); ++it) {
        ++rows;
        const std::string key = (*it)[1].str();
        const std::string action = (*it)[2].str();
        EXPECT_NE(gui->find("ImGuiKey_" + key + ", false"), std::string::npos) << "GUI.cpp does not handle " << key;
        EXPECT_NE(panel->find("{ \"" + key + "\" }, \"" + action + "\""), std::string::npos)
          << "the CONTROLS list does not say \"" << action << "\" for " << key;
    }
    EXPECT_GE(rows, 3U) << "found only " << rows << " function-key shortcut(s) in keys.rs - the scan broke";
}

// getting_started.md lists the keys the CONTROLS list shows, no more and no fewer.
TEST(BuildIntegrity, GettingStartedListsTheEditorsKeys)
{
    const auto panel = readFileText(repoRoot() / "Src" / "shared" / "frontend" / "CommonGuiPanels.ixx");
    const auto guide = readFileText(repoRoot() / "docs" / "source" / "getting_started.md");
    ASSERT_TRUE(panel.has_value() && guide.has_value()) << "could not read the key list or the guide";

    // `Binding{ { "W", "A", "S", "D" }, "..." }` reads "W A S D" in the guide's first column.
    std::set<std::string> listed;
    static const std::regex kBinding(R"rx(Binding\{ \{ ((?:"[^"]+"(?:, )?)+) \}, ")rx");
    static const std::regex kKey(R"rx("([^"]+)")rx");
    for (auto it = std::sregex_iterator(panel->begin(), panel->end(), kBinding); it != std::sregex_iterator(); ++it) {
        const std::string keys = (*it)[1].str();
        std::vector<std::string> names;
        for (auto key = std::sregex_iterator(keys.begin(), keys.end(), kKey); key != std::sregex_iterator(); ++key) {
            names.push_back((*key)[1].str());
        }
        listed.insert(joinViolations(names, " ").substr(1));
    }
    ASSERT_GE(listed.size(), 5U) << "found only " << listed.size()
                                 << " bindings in CommonGuiPanels.ixx - the scan broke";

    std::set<std::string> documented;
    std::istringstream lines(*guide);
    std::string line;
    bool in_table = false;
    while (std::getline(lines, line)) {
        if (line.starts_with("| Key | Does |")) {
            in_table = true;
        } else if (in_table && line.starts_with("| ") && !line.starts_with("| ---")) {
            documented.insert(line.substr(2, line.find(" |", 2) - 2));
        } else if (in_table && !line.starts_with("| ---")) {
            break;
        }
    }
    EXPECT_EQ(documented, listed) << "getting_started.md's key table and renderCommonKeyBindings() disagree";
}

// --help lists every flag: abseil shows only those of files the usage filter accepts.
TEST(BuildIntegrity, EveryFlagIsInAFileHelpLists)
{
    static const std::regex kFlag(R"(ABSL_FLAG\(\s*[^,]+,\s*(\w+))");
    std::size_t flags = 0;
    std::error_code error;
    const fs::path src_root = repoRoot() / "Src";
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error)) { continue; }
        const std::string extension = it->path().extension().string();
        if (extension != ".cpp" && extension != ".ixx" && extension != ".hpp") { continue; }
        const auto text = readFileText(it->path());
        if (!text.has_value()) { continue; }
        for (auto flag = std::sregex_iterator(text->begin(), text->end(), kFlag); flag != std::sregex_iterator();
          ++flag) {
            ++flags;
            EXPECT_TRUE(Kataglyphis::helpListsFlagsOf(it->path().generic_string()))
              << "--" << (*flag)[1].str() << " is defined in " << it->path().generic_string() << ", which --help skips";
        }
    }
    ASSERT_FALSE(error) << error.message();
    EXPECT_GE(flags, 2U) << "found only " << flags << " ABSL_FLAG definitions under Src/ - the scan broke";
}
