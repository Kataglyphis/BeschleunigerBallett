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

}// namespace

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
