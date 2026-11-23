// The studio widgets' press contracts, driven through a bare ImGui context and ImGui's own item activation.

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>

import kataglyphis.shared.imgui.widgets;

namespace {

// Presses segment `index` of a three-way control holding `current`, the way a click on it lands.
bool press_segment(int index, int &current, bool reselectable)
{
    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGuiContext *ctx = ImGui::CreateContext();
    // CreateContext keeps a context that is already current, such as a GUI's.
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0F, 480.0F);
    // Lets a frame run without a renderer backend uploading the font atlas.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    const std::array<const char *, 3> labels = { "Raster", "Ray traced", "Path traced" };

    bool reported = false;
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame();
        ImGui::Begin("panel");
        const ImGuiID segment = ImGui::GetIDWithSeed(index, ImGui::GetID("##segments"));
        reported =
          Kataglyphis::Frontend::studioSegmented("##segments", &current, labels, {}, 300.0F, reselectable) || reported;
        ImGui::End();
        ImGui::Render();
        if (frame == 0) { ImGui::ActivateItemByID(segment); }
    }
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(previous);
    return reported;
}

// Types `text` into a [0, 2] slider's text entry, which Ctrl+click or Enter on a focused slider opens, and returns it.
float type_into_slider(const char *text, float value)
{
    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0F, 480.0F);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    for (int frame = 0; frame < 5; ++frame) {
        if (frame == 2) { io.AddInputCharactersUTF8(text); }
        if (frame == 3) { io.AddKeyEvent(ImGuiKey_Enter, true); }
        if (frame == 4) { io.AddKeyEvent(ImGuiKey_Enter, false); }
        ImGui::NewFrame();
        ImGui::Begin("panel");
        const ImGuiID slider = ImGui::GetID("##ambient");
        Kataglyphis::Frontend::studioSlider("##ambient", &value, 0.0F, 2.0F, "%.2f");
        ImGui::End();
        ImGui::Render();
        if (frame == 0) {
            ImGui::ActivateItemByID(slider);
            // The flag Enter's activation carries: the slider opens its text entry instead of a drag.
            ctx->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
        }
    }
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(previous);
    return value;
}

// Opens the Sun tab's colour picker, types `text` into its `edit` row's `field` ("##rgb"/"##X" is red) and returns it.
std::array<float, 3>
  type_into_color_picker(const char *edit, const char *field, const char *text, std::array<float, 3> rgb)
{
    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0F, 480.0F);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    for (int frame = 0; frame < 6; ++frame) {
        if (frame == 3) { io.AddInputCharactersUTF8(text); }
        if (frame == 4) { io.AddKeyEvent(ImGuiKey_Enter, true); }
        if (frame == 5) { io.AddKeyEvent(ImGuiKey_Enter, false); }
        ImGui::NewFrame();
        ImGui::Begin("panel");
        const ImGuiID swatch = ImGui::GetIDWithSeed("##ColorButton", nullptr, ImGui::GetID("##color"));
        Kataglyphis::Frontend::studioColorEdit3(
          "##color", rgb, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayHex);
        ImGui::End();
        ImGui::Render();
        if (frame == 0) { ImGui::ActivateItemByID(swatch); }
        if (frame == 1 && !ctx->OpenPopupStack.empty() && ctx->OpenPopupStack.back().Window != nullptr) {
            // The popup's ColorPicker4 pushes "##picker", its rows "##rgb" and "##hsv", their fields "##X".."##Z".
            const ImGuiID picker = ImGui::GetIDWithSeed("##picker", nullptr, ctx->OpenPopupStack.back().Window->ID);
            ImGui::ActivateItemByID(ImGui::GetIDWithSeed(field, nullptr, ImGui::GetIDWithSeed(edit, nullptr, picker)));
            ctx->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
        }
    }
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(previous);
    return rgb;
}

}// namespace

// The Sun's colour keeps [0, 1] for a value typed into its picker, whose float fields ImGui leaves unclamped.
TEST(StudioWidgetsUnit, AColourPickerHoldsItsRangeForATypedValue)
{
    const std::array<float, 3> sun = { 0.8F, 0.4F, 0.2F };
    EXPECT_FLOAT_EQ(type_into_color_picker("##rgb", "##X", "0.25", sun)[0], 0.25F)
      << "typing does not reach the picker's red field: the harness is broken";
    EXPECT_EQ(type_into_color_picker("##rgb", "##X", "-1", sun)[0], 0.0F) << "a typed red below 0 passed unclamped";
    EXPECT_EQ(type_into_color_picker("##rgb", "##X", "7", sun)[0], 1.0F) << "a typed red above 1 passed unclamped";
    // A saturation of 5 turns this orange into green -1.87 and blue -3.2 before any clamp.
    const std::array<float, 3> saturated = type_into_color_picker("##hsv", "##Y", "5", sun);
    EXPECT_NE(saturated, sun) << "typing does not reach the picker's saturation field: the harness is broken";
    for (const float channel : saturated) {
        EXPECT_TRUE(channel >= 0.0F && channel <= 1.0F) << "a typed saturation of 5 left a channel at " << channel;
    }
}

// The Sun's Ambient and Radiance, and every other editor slider, keep their range for a typed value too.
TEST(StudioWidgetsUnit, ASliderHoldsItsRangeForATypedValue)
{
    EXPECT_FLOAT_EQ(type_into_slider("1.5", 0.35F), 1.5F) << "typing does not reach the slider: the harness is broken";
    EXPECT_EQ(type_into_slider("-1", 0.35F), 0.0F) << "a typed value below the range passed unclamped";
    EXPECT_EQ(type_into_slider("7", 0.35F), 2.0F) << "a typed value above the range passed unclamped";
}

// The editor's render-mode switch shows the drawn mode, so it can only cancel a waiting pick through its shown segment.
TEST(StudioWidgetsUnit, OnlyAReselectableSegmentedControlReportsAPressOnItsSelectedSegment)
{
    int current = 0;
    EXPECT_FALSE(press_segment(0, current, false)) << "a plain control reports nothing for its selected segment";
    EXPECT_TRUE(press_segment(0, current, true))
      << "a reselectable control must report the press, or it cannot act on what it does not show";
    EXPECT_EQ(current, 0);

    EXPECT_TRUE(press_segment(2, current, false)) << "pressing another segment is a change either way";
    EXPECT_EQ(current, 2);
}
