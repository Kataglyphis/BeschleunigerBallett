module;

#include <array>
#include <filesystem>
#include <system_error>

#include <imgui.h>

export module kataglyphis.shared.imgui.fonts;

import kataglyphis.shared.util.resource_paths;

export namespace Kataglyphis::Frontend {
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
  -> bool
{
    std::error_code ec;
    if (!std::filesystem::exists(fontPath, ec) || ec) { return false; }

    return fonts->AddFontFromFileTTF(fontPath.string().c_str(), sizePixels) != nullptr;
}

inline void configureKataglyphisImGuiFonts(ImGuiIO &imguiIo, float sizePixels)
{
    std::error_code current_path_ec;
    const std::filesystem::path cwd = std::filesystem::current_path(current_path_ec);
    const std::filesystem::path fontDir =
      resolveKataglyphisImGuiFontDirectory(current_path_ec ? std::filesystem::path(".") : cwd);

    bool hasCustomFont = false;
    if (!fontDir.empty()) {
        const std::array<std::filesystem::path, 6> fontFiles = { fontDir / "Roboto-Medium.ttf",
            fontDir / "Cousine-Regular.ttf",
            fontDir / "DroidSans.ttf",
            fontDir / "Karla-Regular.ttf",
            fontDir / "ProggyClean.ttf",
            fontDir / "ProggyTiny.ttf" };

        for (const auto &fontPath : fontFiles) {
            hasCustomFont = addKataglyphisFontIfAvailable(imguiIo.Fonts, fontPath, sizePixels) || hasCustomFont;
        }
    }

    if (!hasCustomFont) {
        ImFontConfig fontConfig{};
        fontConfig.SizePixels = sizePixels;
        imguiIo.Fonts->AddFontDefault(&fontConfig);
    }
}

inline void configureKataglyphisImGuiFonts(ImGuiIO &imguiIo)
{
    constexpr float defaultFontSizePixels = 30.0F;
    configureKataglyphisImGuiFonts(imguiIo, defaultFontSizePixels);
}
}// namespace Kataglyphis::Frontend
