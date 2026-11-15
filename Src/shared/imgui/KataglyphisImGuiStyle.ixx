module;
#include <algorithm>
#include <cmath>

#include <imgui.h>

export module kataglyphis.shared.imgui.style;

export namespace Kataglyphis::Frontend {

// sRGB bytes as ImGui's ImVec4; the UI draws after tonemapping, so these land on screen verbatim.
constexpr auto studioRgb(int red, int green, int blue, float alpha = 1.0F) -> ImVec4
{
    return {
        static_cast<float>(red) / 255.0F, static_cast<float>(green) / 255.0F, static_cast<float>(blue) / 255.0F, alpha
    };
}

constexpr auto studioWithAlpha(ImVec4 color, float alpha) -> ImVec4 { return { color.x, color.y, color.z, alpha }; }

// Studio Pro: graphite neutrals; the accent (brand.tokens.json colors.accent) marks selection and live values only.
namespace studio_palette {
    inline constexpr ImVec4 kBase = studioRgb(14, 16, 19);
    inline constexpr ImVec4 kPanel = studioRgb(21, 24, 29);
    inline constexpr ImVec4 kPopup = studioRgb(26, 30, 36);
    inline constexpr ImVec4 kFrame = studioRgb(28, 32, 39);
    inline constexpr ImVec4 kFrameHover = studioRgb(35, 40, 51);
    inline constexpr ImVec4 kFrameActive = studioRgb(42, 48, 59);
    inline constexpr ImVec4 kRaised = studioRgb(37, 42, 50);
    inline constexpr ImVec4 kRaisedHover = studioRgb(44, 50, 60);
    inline constexpr ImVec4 kRaisedActive = studioRgb(51, 58, 70);
    inline constexpr ImVec4 kBorder = studioRgb(42, 47, 56);
    inline constexpr ImVec4 kBorderLight = studioRgb(34, 38, 45);
    inline constexpr ImVec4 kGrab = studioRgb(96, 104, 118);
    inline constexpr ImVec4 kText = studioRgb(230, 232, 235);
    inline constexpr ImVec4 kTextMuted = studioRgb(155, 163, 174);
    inline constexpr ImVec4 kTextDisabled = studioRgb(107, 116, 128);
    inline constexpr ImVec4 kAccent = studioRgb(106, 240, 173);
    inline constexpr ImVec4 kAccentHover = studioRgb(138, 245, 193);
    inline constexpr ImVec4 kAccentActive = studioRgb(42, 212, 136);
    inline constexpr ImVec4 kWarning = studioRgb(245, 180, 85);
    inline constexpr ImVec4 kDanger = studioRgb(242, 119, 122);
    inline constexpr ImVec4 kInfo = studioRgb(122, 167, 255);
    inline constexpr ImVec4 kLink = studioRgb(34, 211, 238);
    // X, Y and Z markers on vector fields: the usual red, green and blue, toned to sit with the graphite.
    inline constexpr ImVec4 kAxisX = studioRgb(232, 92, 100);
    inline constexpr ImVec4 kAxisY = studioRgb(138, 196, 88);
    inline constexpr ImVec4 kAxisZ = studioRgb(90, 148, 236);
}// namespace studio_palette

// `scale` is the window's content scale times the user's UI scale; metrics are authored at 1.0.
inline void applyKataglyphisImGuiDarkTheme(float scale = 1.0F)
{
    namespace pal = studio_palette;
    ImGuiStyle &style = ImGui::GetStyle();

    // Start from a fresh style, so a DPI change rescales the authored metrics instead of compounding them.
    const float font_size_base = style.FontSizeBase;
    const float font_scale_main = style.FontScaleMain;
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);
    style.FontSizeBase = font_size_base;
    style.FontScaleMain = font_scale_main;

    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_Text] = pal::kText;
    colors[ImGuiCol_TextDisabled] = pal::kTextDisabled;
    colors[ImGuiCol_WindowBg] = pal::kPanel;
    colors[ImGuiCol_ChildBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_PopupBg] = pal::kPopup;
    colors[ImGuiCol_Border] = pal::kBorder;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_FrameBg] = pal::kFrame;
    colors[ImGuiCol_FrameBgHovered] = pal::kFrameHover;
    colors[ImGuiCol_FrameBgActive] = pal::kFrameActive;
    colors[ImGuiCol_TitleBg] = pal::kBase;
    colors[ImGuiCol_TitleBgActive] = pal::kPanel;
    colors[ImGuiCol_TitleBgCollapsed] = pal::kBase;
    colors[ImGuiCol_MenuBarBg] = pal::kPanel;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_ScrollbarGrab] = studioRgb(46, 52, 64);
    colors[ImGuiCol_ScrollbarGrabHovered] = studioRgb(58, 65, 80);
    colors[ImGuiCol_ScrollbarGrabActive] = studioRgb(74, 82, 98);
    colors[ImGuiCol_CheckMark] = pal::kAccent;
    colors[ImGuiCol_CheckboxSelectedBg] = pal::kRaised;
    // Neutral at rest so the value text over it stays legible; mint only marks the value being edited.
    colors[ImGuiCol_SliderGrab] = pal::kGrab;
    colors[ImGuiCol_SliderGrabActive] = studioWithAlpha(pal::kAccent, 0.32F);
    colors[ImGuiCol_Button] = pal::kFrame;
    colors[ImGuiCol_ButtonHovered] = pal::kFrameHover;
    colors[ImGuiCol_ButtonActive] = pal::kFrameActive;
    colors[ImGuiCol_Header] = pal::kRaised;
    colors[ImGuiCol_HeaderHovered] = pal::kRaisedHover;
    colors[ImGuiCol_HeaderActive] = pal::kRaisedActive;
    colors[ImGuiCol_Separator] = pal::kBorder;
    colors[ImGuiCol_SeparatorHovered] = studioWithAlpha(pal::kAccent, 0.6F);
    colors[ImGuiCol_SeparatorActive] = pal::kAccent;
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_ResizeGripHovered] = studioWithAlpha(pal::kAccent, 0.4F);
    colors[ImGuiCol_ResizeGripActive] = studioWithAlpha(pal::kAccent, 0.7F);
    colors[ImGuiCol_InputTextCursor] = pal::kText;
    colors[ImGuiCol_TabHovered] = pal::kFrameHover;
    colors[ImGuiCol_Tab] = pal::kPanel;
    colors[ImGuiCol_TabSelected] = pal::kRaised;
    colors[ImGuiCol_TabSelectedOverline] = pal::kAccent;
    colors[ImGuiCol_TabDimmed] = pal::kPanel;
    colors[ImGuiCol_TabDimmedSelected] = pal::kRaised;
    colors[ImGuiCol_TabDimmedSelectedOverline] = studioWithAlpha(pal::kAccent, 0.5F);
    colors[ImGuiCol_PlotLines] = pal::kAccent;
    colors[ImGuiCol_PlotLinesHovered] = pal::kAccentHover;
    colors[ImGuiCol_PlotHistogram] = pal::kAccent;
    colors[ImGuiCol_PlotHistogramHovered] = pal::kAccentHover;
    colors[ImGuiCol_TableHeaderBg] = pal::kFrame;
    colors[ImGuiCol_TableBorderStrong] = pal::kBorder;
    colors[ImGuiCol_TableBorderLight] = pal::kBorderLight;
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0F, 1.0F, 1.0F, 0.02F);
    colors[ImGuiCol_TextLink] = pal::kLink;
    colors[ImGuiCol_TextSelectedBg] = studioWithAlpha(pal::kAccent, 0.35F);
    colors[ImGuiCol_TreeLines] = pal::kBorder;
    colors[ImGuiCol_DragDropTarget] = pal::kAccent;
    colors[ImGuiCol_NavCursor] = pal::kAccent;
    colors[ImGuiCol_NavWindowingHighlight] = studioWithAlpha(pal::kAccent, 0.7F);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.5F);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.55F);

    style.WindowPadding = ImVec2(12.0F, 10.0F);
    style.FramePadding = ImVec2(8.0F, 4.0F);
    style.CellPadding = ImVec2(6.0F, 4.0F);
    style.ItemSpacing = ImVec2(8.0F, 6.0F);
    style.ItemInnerSpacing = ImVec2(6.0F, 4.0F);
    style.IndentSpacing = 14.0F;
    style.ScrollbarSize = 10.0F;
    style.GrabMinSize = 10.0F;
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.TabBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F;
    style.TabBarOverlineSize = 2.0F;
    style.WindowRounding = 6.0F;
    style.ChildRounding = 6.0F;
    style.FrameRounding = 5.0F;
    style.PopupRounding = 6.0F;
    style.ScrollbarRounding = 6.0F;
    style.GrabRounding = 5.0F;
    style.TabRounding = 4.0F;
    style.SeparatorTextBorderSize = 1.0F;
    style.SeparatorTextPadding = ImVec2(0.0F, 6.0F);
    style.WindowTitleAlign = ImVec2(0.0F, 0.5F);
    style.DisabledAlpha = 0.45F;

    style.ScaleAllSizes(scale);
    // ScaleAllSizes truncates, so any scale below 1 would zero every 1 px line, the text caret included.
    const float hairline = std::max(1.0F, std::round(scale));
    style.WindowBorderSize = hairline;
    style.ChildBorderSize = hairline;
    style.PopupBorderSize = hairline;
    style.TabBarBorderSize = hairline;
    style.SeparatorTextBorderSize = hairline;
    style.SeparatorSize = hairline;
    style.TreeLinesSize = hairline;
    style.InputTextCursorSize = hairline;
    // ImGui draws the close-button X, link underlines and swatch borders (int)_MainScale wide, 0 below scale 1.
    style._MainScale = std::max(style._MainScale, 1.0F);
    style.TabBarOverlineSize = std::max(2.0F, std::round(2.0F * scale));
    // Tooltips sit this many cursor sizes away from the mouse; truncated to 0 they would open under it.
    style.MouseCursorScale = std::max(1.0F, scale);
    style.FontScaleDpi = scale;
}
}// namespace Kataglyphis::Frontend
