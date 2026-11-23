module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <span>

#include <imgui.h>

export module kataglyphis.shared.frontend.common_gui_panels;

import kataglyphis.shared.imgui.style;
import kataglyphis.shared.imgui.widgets;

export namespace Kataglyphis::Frontend {

// Every GLFW binding needs its prose here: BuildIntegrity.KeyBindingsPanelListsEveryBinding reads this file.
inline void renderCommonKeyBindings()
{
    struct Binding
    {
        std::array<const char *, 4> keys;
        const char *action;
    };
    static constexpr std::array kBindings = {
        Binding{ { "W", "A", "S", "D" }, "Move forward, back and sideways" },
        Binding{ { "Q", "E" }, "Turn left and right" },
        Binding{ { "Right mouse" }, "Hold to look around" },
        Binding{ { "ESC" }, "Leave the focused panel; pressed again, quit" },
        Binding{ { "F1" }, "Show or hide the editor" },
        Binding{ { "F5" }, "Reload all shaders" },
    };

    if (!ImGui::BeginTable("##key_bindings", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch);
    const float gap = std::max(2.0F, std::round(ImGui::GetStyle().ItemInnerSpacing.x * 0.75F));
    ImGui::PushStyleColor(ImGuiCol_Text, studio_palette::kTextMuted);
    for (const Binding &binding : kBindings) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        for (std::size_t index = 0; index < binding.keys.size() && binding.keys[index] != nullptr; ++index) {
            if (index > 0) { ImGui::SameLine(0.0F, gap); }
            studioKeycap(binding.keys[index]);
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(binding.action);
        ImGui::PopTextWrapPos();
    }
    ImGui::PopStyleColor();
    ImGui::EndTable();
}

// `pass_ms` < 0 reads "n/a" (pass off or no sample); unequal spans use the shorter, so a mismatch never overreads.
inline void renderGpuPassTimings(bool supported,
  std::span<const float> pass_ms,
  std::span<const char *const> pass_names,
  float frame_ms)
{
    if (!supported) {
        ImGui::PushStyleColor(ImGuiCol_Text, studio_palette::kTextMuted);
        ImGui::TextUnformatted("unavailable (no timestamp support on this queue family)");
        ImGui::PopStyleColor();
        return;
    }

    const std::size_t count = std::min(pass_ms.size(), pass_names.size());
    float longest = 0.0F;
    for (std::size_t i = 0; i < count; ++i) { longest = std::max(longest, pass_ms[i]); }

    if (!ImGui::BeginTable("##gpu_passes", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TableSetupColumn("pass", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("bar", ImGuiTableColumnFlags_WidthStretch);
    // Sized for the widest value, so the column does not jump with the readings.
    ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000.00 ms").x);
    std::array<char, 32> value{};
    for (std::size_t i = 0; i < count; ++i) {
        const bool active = pass_ms[i] >= 0.0F;
        if (active) {
            std::snprintf(value.data(), value.size(), "%.2f ms", static_cast<double>(pass_ms[i]));
        } else {
            std::snprintf(value.data(), value.size(), "n/a");
        }
        // Each bar is the pass's share of the frame; without a frame time, of the longest pass.
        float fraction = 0.0F;
        if (frame_ms > 0.0F) {
            fraction = std::clamp(pass_ms[i] / frame_ms, 0.0F, 1.0F);
        } else if (longest > 0.0F) {
            fraction = pass_ms[i] / longest;
        }
        studioMetricRow(pass_names[i], fraction, value.data(), active);
    }
    ImGui::EndTable();
}

}// namespace Kataglyphis::Frontend
