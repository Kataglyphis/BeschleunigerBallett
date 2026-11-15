module;
#include <filesystem>
#include <system_error>

#include <imgui.h>

export module kataglyphis.shared.imgui.fonts;

import kataglyphis.shared.util.resource_paths;

export namespace Kataglyphis::Frontend {

// Unscaled UI size; style.FontScaleDpi carries the monitor's content scale on top.
inline constexpr float kKataglyphisUiFontPixels = 15.0F;

// The type scale over the body size: captions, object titles and the profiler's headline figure.
inline constexpr float kStudioCaptionScale = 0.8F;
inline constexpr float kStudioTitleScale = 1.2F;
inline constexpr float kStudioFigureScale = 1.8F;

// Roboto is the brand face; Cousine, already shipped with Dear ImGui, gives numbers fixed widths.
struct KataglyphisFonts
{
    ImFont *ui = nullptr;
    ImFont *mono = nullptr;
};

inline auto resolveKataglyphisImGuiFontDirectory(const std::filesystem::path &cwd) -> std::filesystem::path
{
    std::error_code ec;
#ifdef RELATIVE_IMGUI_FONTS_PATH
    auto fromMacro = (cwd / std::filesystem::path(RELATIVE_IMGUI_FONTS_PATH)).lexically_normal();
    if (std::filesystem::exists(fromMacro, ec) && !ec) { return fromMacro; }
#endif
    if (const auto found = Kataglyphis::Shared::searchAncestorsForRelative(
          cwd, "third_party/IMGUI/misc/fonts", Kataglyphis::Shared::kResourceSearchDepth);
      found.has_value()) {
        return *found;
    }
    return {};
}

inline auto addKataglyphisFontIfAvailable(ImFontAtlas *fonts, const std::filesystem::path &fontPath, float sizePixels)
  -> ImFont *
{
    std::error_code ec;
    if (!std::filesystem::exists(fontPath, ec) || ec) { return nullptr; }
    return fonts->AddFontFromFileTTF(fontPath.string().c_str(), sizePixels);
}

// The vector default stands in for a missing TTF: the bitmap one is drawn for 13 px and blurs at any scale.
inline auto configureKataglyphisImGuiFonts(ImGuiIO &imguiIo, float sizePixels) -> KataglyphisFonts
{
    std::error_code current_path_ec;
    const std::filesystem::path cwd = std::filesystem::current_path(current_path_ec);
    const std::filesystem::path fontDir =
      resolveKataglyphisImGuiFontDirectory(current_path_ec ? std::filesystem::path(".") : cwd);

    KataglyphisFonts fonts{};
    if (!fontDir.empty()) {
        fonts.ui = addKataglyphisFontIfAvailable(imguiIo.Fonts, fontDir / "Roboto-Medium.ttf", sizePixels);
        fonts.mono = addKataglyphisFontIfAvailable(imguiIo.Fonts, fontDir / "Cousine-Regular.ttf", sizePixels);
    }
    if (fonts.ui == nullptr) {
        ImFontConfig fontConfig{};
        fontConfig.SizePixels = sizePixels;
        fonts.ui = imguiIo.Fonts->AddFontDefaultVector(&fontConfig);
    }
    if (fonts.mono == nullptr) { fonts.mono = fonts.ui; }
    imguiIo.FontDefault = fonts.ui;
    return fonts;
}

inline auto configureKataglyphisImGuiFonts(ImGuiIO &imguiIo) -> KataglyphisFonts
{ return configureKataglyphisImGuiFonts(imguiIo, kKataglyphisUiFontPixels); }
}// namespace Kataglyphis::Frontend
