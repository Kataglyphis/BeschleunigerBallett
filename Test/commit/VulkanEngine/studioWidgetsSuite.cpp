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

}// namespace

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
