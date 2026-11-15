module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include <imgui.h>
#include <imgui_internal.h>

export module kataglyphis.shared.imgui.widgets;

import kataglyphis.shared.imgui.fonts;
import kataglyphis.shared.imgui.style;

export namespace Kataglyphis::Frontend {

enum class StudioIcon : std::uint8_t {
    Cube,
    Camera,
    Sun,
    Mountain,
    Cloud,
    Eye,
    EyeOff,
    Refresh,
    Gauge,
    Keyboard,
    Layers
};

// Stroke icons on a 24-unit grid; drawn rather than loaded, so they need no icon font and no licence row.
inline void drawStudioIcon(ImDrawList *drawList, StudioIcon icon, ImVec2 origin, float size, ImU32 color)
{
    const float unit = size / 24.0F;
    // About the stem weight of Roboto Medium at the same size.
    const float thickness = std::max(1.0F, 2.4F * unit);
    const auto at = [origin, unit](float x, float y) { return ImVec2(origin.x + x * unit, origin.y + y * unit); };

    switch (icon) {
    case StudioIcon::Cube: {
        const std::array hexagon = {
            at(12.0F, 2.0F), at(21.0F, 7.0F), at(21.0F, 17.0F), at(12.0F, 22.0F), at(3.0F, 17.0F), at(3.0F, 7.0F)
        };
        drawList->AddPolyline(hexagon.data(), static_cast<int>(hexagon.size()), color, ImDrawFlags_Closed, thickness);
        drawList->AddLine(at(3.0F, 7.0F), at(12.0F, 12.0F), color, thickness);
        drawList->AddLine(at(21.0F, 7.0F), at(12.0F, 12.0F), color, thickness);
        drawList->AddLine(at(12.0F, 12.0F), at(12.0F, 22.0F), color, thickness);
        break;
    }
    case StudioIcon::Camera: {
        const std::array body = { at(3.0F, 7.0F),
            at(7.0F, 7.0F),
            at(9.0F, 4.0F),
            at(15.0F, 4.0F),
            at(17.0F, 7.0F),
            at(21.0F, 7.0F),
            at(21.0F, 19.0F),
            at(3.0F, 19.0F) };
        drawList->AddPolyline(body.data(), static_cast<int>(body.size()), color, ImDrawFlags_Closed, thickness);
        drawList->AddCircle(at(12.0F, 13.0F), 4.0F * unit, color, 0, thickness);
        break;
    }
    case StudioIcon::Sun: {
        drawList->AddCircle(at(12.0F, 12.0F), 4.0F * unit, color, 0, thickness);
        for (int ray = 0; ray < 8; ++ray) {
            const float angle = static_cast<float>(ray) * (IM_PI / 4.0F);
            const float cx = std::cos(angle);
            const float cy = std::sin(angle);
            drawList->AddLine(
              at(12.0F + cx * 7.5F, 12.0F + cy * 7.5F), at(12.0F + cx * 11.0F, 12.0F + cy * 11.0F), color, thickness);
        }
        break;
    }
    case StudioIcon::Mountain: {
        const std::array ridge = {
            at(2.0F, 19.0F), at(8.5F, 6.0F), at(13.0F, 13.0F), at(16.0F, 9.5F), at(22.0F, 19.0F)
        };
        drawList->AddPolyline(ridge.data(), static_cast<int>(ridge.size()), color, ImDrawFlags_Closed, thickness);
        break;
    }
    case StudioIcon::Cloud: {
        drawList->PathArcTo(at(7.0F, 15.0F), 4.0F * unit, IM_PI * 0.5F, IM_PI * 1.5F);
        drawList->PathArcTo(at(12.0F, 11.0F), 5.0F * unit, IM_PI, IM_PI * 2.0F);
        drawList->PathArcTo(at(17.0F, 15.0F), 4.0F * unit, IM_PI * 1.5F, IM_PI * 2.5F);
        drawList->PathStroke(color, ImDrawFlags_Closed, thickness);
        break;
    }
    case StudioIcon::Eye:
    case StudioIcon::EyeOff: {
        drawList->PathLineTo(at(2.0F, 12.0F));
        drawList->PathBezierQuadraticCurveTo(at(12.0F, 2.0F), at(22.0F, 12.0F));
        drawList->PathBezierQuadraticCurveTo(at(12.0F, 22.0F), at(2.0F, 12.0F));
        drawList->PathStroke(color, ImDrawFlags_Closed, thickness);
        // The slash would merge with the pupil into a blob at text size.
        if (icon == StudioIcon::EyeOff) {
            drawList->AddLine(at(4.0F, 4.0F), at(20.0F, 20.0F), color, thickness);
        } else {
            drawList->AddCircle(at(12.0F, 12.0F), 3.0F * unit, color, 0, thickness);
        }
        break;
    }
    case StudioIcon::Refresh: {
        drawList->PathArcTo(at(12.0F, 12.0F), 8.0F * unit, -IM_PI * 0.25F, IM_PI * 1.35F);
        drawList->PathStroke(color, ImDrawFlags_None, thickness);
        const ImVec2 tip = at(12.0F + 8.0F * 0.7071F, 12.0F - 8.0F * 0.7071F);
        drawList->AddLine(tip, ImVec2(tip.x, tip.y - 5.0F * unit), color, thickness);
        drawList->AddLine(tip, ImVec2(tip.x - 5.0F * unit, tip.y), color, thickness);
        break;
    }
    case StudioIcon::Gauge: {
        drawList->PathArcTo(at(12.0F, 14.0F), 9.0F * unit, IM_PI * 0.8F, IM_PI * 2.2F);
        drawList->PathStroke(color, ImDrawFlags_None, thickness);
        drawList->AddLine(at(12.0F, 14.0F), at(16.0F, 9.0F), color, thickness);
        break;
    }
    case StudioIcon::Keyboard: {
        drawList->AddRect(at(2.0F, 6.0F), at(22.0F, 18.0F), color, 2.0F * unit, ImDrawFlags_None, thickness);
        for (int key = 0; key < 4; ++key) {
            const float x = 6.0F + 4.0F * static_cast<float>(key);
            drawList->AddLine(at(x, 10.0F), at(x + 0.5F, 10.0F), color, thickness);
        }
        drawList->AddLine(at(7.0F, 14.0F), at(17.0F, 14.0F), color, thickness);
        break;
    }
    case StudioIcon::Layers: {
        const std::array top = { at(12.0F, 4.5F), at(21.0F, 9.5F), at(12.0F, 14.5F), at(3.0F, 9.5F) };
        drawList->AddPolyline(top.data(), static_cast<int>(top.size()), color, ImDrawFlags_Closed, thickness);
        const std::array bottom = { at(3.0F, 14.5F), at(12.0F, 19.5F), at(21.0F, 14.5F) };
        drawList->AddPolyline(bottom.data(), static_cast<int>(bottom.size()), color, ImDrawFlags_None, thickness);
        break;
    }
    }
}

// An icon the size of the current font, laid out like a glyph.
inline void studioIconText(StudioIcon icon, ImU32 color)
{
    const float size = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    drawStudioIcon(ImGui::GetWindowDrawList(), icon, origin, size, color);
}

// A frame-height button with a text-size icon centred in it.
inline auto studioIconButton(const char *strId, StudioIcon icon, const char *tooltip) -> bool
{
    const float side = ImGui::GetFrameHeight();
    const float iconSize = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button(strId, ImVec2(side, side));
    const float inset = std::round((side - iconSize) * 0.5F);
    const ImU32 color = ImGui::GetColorU32(ImGui::IsItemHovered() ? studio_palette::kText : studio_palette::kTextMuted);
    drawStudioIcon(ImGui::GetWindowDrawList(), icon, ImVec2(origin.x + inset, origin.y + inset), iconSize, color);
    if (tooltip != nullptr) { ImGui::SetItemTooltip("%s", tooltip); }
    return pressed;
}

// The width studioToggle() takes, so a caller can right-align it.
inline auto studioToggleWidth() -> float { return std::round(std::round(ImGui::GetFrameHeight() * 0.72F) * 1.8F); }

// A pill switch: an ImGui item, so keyboard navigation and BeginDisabled() work as for a checkbox.
inline auto studioToggle(const char *strId, bool *value) -> bool
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems) { return false; }

    const ImGuiID id = window->GetID(strId);
    const float height = ImGui::GetFrameHeight();
    const float trackHeight = std::round(height * 0.72F);
    const float trackWidth = studioToggleWidth();
    const ImVec2 origin = window->DC.CursorPos;
    const ImRect bb(origin, ImVec2(origin.x + trackWidth, origin.y + height));
    ImGui::ItemSize(bb, ImGui::GetStyle().FramePadding.y);
    if (!ImGui::ItemAdd(bb, id)) { return false; }

    bool hovered = false;
    bool held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        ImGui::MarkItemEdited(id);
    }

    // A 120 ms ease; the position lives in the window's storage under the item id.
    float *knob = window->StateStorage.GetFloatRef(id, *value ? 1.0F : 0.0F);
    const float target = *value ? 1.0F : 0.0F;
    const float step = ImGui::GetIO().DeltaTime / 0.12F;
    *knob = (*knob < target) ? std::min(target, *knob + step) : std::max(target, *knob - step);
    const float t = *knob;

    // Own colours rather than the slider's: mint is the on-state, and the off track must stand off the panel.
    const ImVec2 trackMin(bb.Min.x, bb.Min.y + (height - trackHeight) * 0.5F);
    const ImVec2 trackMax(trackMin.x + trackWidth, trackMin.y + trackHeight);
    const ImVec4 offColor = hovered ? studioRgb(60, 68, 82) : studio_palette::kRaisedActive;
    const ImVec4 onColor = hovered ? studio_palette::kAccentHover : studio_palette::kAccent;
    window->DrawList->AddRectFilled(
      trackMin, trackMax, ImGui::GetColorU32(ImLerp(offColor, onColor, t)), trackHeight * 0.5F);

    const float radius = trackHeight * 0.5F - std::max(2.0F, std::round(trackHeight * 0.12F));
    const float knobX = ImLerp(trackMin.x + trackHeight * 0.5F, trackMax.x - trackHeight * 0.5F, t);
    const ImVec4 knobColor =
      ImLerp(hovered ? studio_palette::kText : studio_palette::kTextMuted, studio_palette::kPanel, t);
    window->DrawList->AddCircleFilled(
      ImVec2(knobX, (trackMin.y + trackMax.y) * 0.5F), radius, ImGui::GetColorU32(knobColor));
    ImGui::RenderNavCursor(bb, id);
    return pressed;
}

// Equal-width segments in one frame. `enabled` may be empty (all enabled); `width` 0 fits the labels, < 0 fills.
inline auto studioSegmented(const char *strId,
  int *current,
  std::span<const char *const> labels,
  std::span<const bool> enabled = {},
  float width = 0.0F) -> bool
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems || labels.empty()) { return false; }

    const ImGuiStyle &style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float inset = std::max(2.0F, std::round(style.FramePadding.y * 0.5F));
    const auto count = static_cast<float>(labels.size());
    float total = width;
    if (width < 0.0F) {
        total = ImGui::GetContentRegionAvail().x;
    } else if (width == 0.0F) {
        float widest = 0.0F;
        for (const char *label : labels) { widest = std::max(widest, ImGui::CalcTextSize(label, nullptr, true).x); }
        total = (widest + style.FramePadding.x * 2.0F) * count + inset * 2.0F;
    }

    const ImVec2 origin = window->DC.CursorPos;
    const ImRect frame(origin, ImVec2(origin.x + total, origin.y + height));
    ImGui::ItemSize(frame, style.FramePadding.y);
    window->DrawList->AddRectFilled(frame.Min, frame.Max, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);

    ImGui::PushID(strId);
    const float segmentWidth = (total - inset * 2.0F) / count;
    const float rounding = std::max(0.0F, style.FrameRounding - inset);
    const float underline = std::max(1.0F, style.TabBarOverlineSize);
    bool changed = false;
    for (std::size_t index = 0; index < labels.size(); ++index) {
        const bool isEnabled = enabled.empty() || (index < enabled.size() && enabled[index]);
        const float left = frame.Min.x + inset + segmentWidth * static_cast<float>(index);
        const ImRect segment(ImVec2(left, frame.Min.y + inset), ImVec2(left + segmentWidth, frame.Max.y - inset));
        const ImGuiID id = window->GetID(static_cast<int>(index));
        const bool selected = (*current == static_cast<int>(index));

        ImGui::BeginDisabled(!isEnabled);
        bool hovered = false;
        bool held = false;
        bool pressed = false;
        if (ImGui::ItemAdd(segment, id)) { pressed = ImGui::ButtonBehavior(segment, id, &hovered, &held); }
        if (pressed && !selected) {
            *current = static_cast<int>(index);
            changed = true;
        }
        if (selected || hovered) {
            window->DrawList->AddRectFilled(segment.Min,
              segment.Max,
              ImGui::GetColorU32(selected ? ImGuiCol_TabSelected : ImGuiCol_FrameBgHovered),
              rounding);
        }
        if (selected) {
            const float margin = segmentWidth * 0.22F;
            window->DrawList->AddRectFilled(ImVec2(segment.Min.x + margin, segment.Max.y - underline),
              ImVec2(segment.Max.x - margin, segment.Max.y),
              ImGui::GetColorU32(ImGuiCol_TabSelectedOverline),
              underline * 0.5F);
        }
        const ImVec2 textSize = ImGui::CalcTextSize(labels[index], nullptr, true);
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? studio_palette::kText : studio_palette::kTextMuted);
        ImGui::RenderTextClipped(segment.Min, segment.Max, labels[index], nullptr, &textSize, ImVec2(0.5F, 0.5F));
        ImGui::PopStyleColor();
        ImGui::RenderNavCursor(segment, id);
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    return changed;
}

// BeginTabItem with the segmented control's marks: muted idle labels, the selected one bright and underlined.
inline auto studioTabItem(const char *label, ImGuiTabItemFlags flags = ImGuiTabItemFlags_None) -> bool
{
    const ImGuiTabBar *bar = ImGui::GetCurrentTabBar();
    const ImGuiID shown =
      (bar == nullptr) ? 0 : (bar->NextSelectedTabId != 0 ? bar->NextSelectedTabId : bar->SelectedTabId);
    const bool selected = shown != 0 && shown == ImGui::GetID(label);
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? studio_palette::kText : studio_palette::kTextMuted);
    const bool open = ImGui::BeginTabItem(label, nullptr, flags);
    ImGui::PopStyleColor();
    if (open) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float underline = std::max(1.0F, ImGui::GetStyle().TabBarOverlineSize);
        const float inset = std::round((max.x - min.x) * 0.22F);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x + inset, max.y - underline),
          ImVec2(max.x - inset, max.y),
          ImGui::GetColorU32(ImGuiCol_TabSelectedOverline),
          underline * 0.5F);
    }
    return open;
}

// BeginDisabled scales style.Alpha; this FrameBg composites back to kFrame over kPanel, so fields keep their shape.
inline void studioBeginDisabled(bool disabled)
{
    ImGui::BeginDisabled(disabled);
    const float alpha = std::max(ImGui::GetStyle().Alpha, 0.01F);
    const ImVec4 frame = ImLerp(studio_palette::kPanel, studio_palette::kFrame, 1.0F / alpha);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, frame);
    ImGui::PushStyleColor(ImGuiCol_Button, frame);
}

inline void studioEndDisabled()
{
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
}

// A small tracked caps caption, vertically centred in `lineHeight` (at least its own height).
inline void studioCaption(const char *text, float lineHeight = 0.0F)
{
    ImGui::PushFont(nullptr, kKataglyphisUiFontPixels * kStudioCaptionScale);
    const float size = ImGui::GetFontSize();
    const float tracking = std::max(1.0F, std::round(size * 0.08F));
    const float height = std::max(lineHeight, size);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float y = origin.y + std::round((height - size) * 0.5F);
    const ImU32 color = ImGui::GetColorU32(studio_palette::kTextMuted);
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    float x = origin.x;
    for (const char *glyph = text; *glyph != '\0'; ++glyph) {
        drawList->AddText(ImVec2(x, y), color, glyph, glyph + 1);
        x += ImGui::CalcTextSize(glyph, glyph + 1).x + tracking;
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(std::max(0.0F, x - origin.x - tracking), height));
}

// A slider drawn as a fill up to the value, so no grab ever covers a digit. Logarithmic needs 0 < min < max.
template<typename T>
    requires std::is_same_v<T, float> || std::is_same_v<T, int>
inline auto studioSlider(const char *strId,
  T *value,
  std::type_identity_t<T> min,
  std::type_identity_t<T> max,
  const char *format = nullptr,
  ImGuiSliderFlags flags = ImGuiSliderFlags_None) -> bool
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems) { return false; }
    const bool logarithmic = (flags & ImGuiSliderFlags_Logarithmic) != 0;
    IM_ASSERT(!logarithmic || (min > T{ 0 } && max > min));

    // Drawn first, from this frame's value; ImGui adds the hover tint, the text and the nav cursor on top.
    const ImGuiStyle &style = ImGui::GetStyle();
    const ImVec2 origin = window->DC.CursorPos;
    const ImRect frame(origin, ImVec2(origin.x + ImGui::CalcItemWidth(), origin.y + ImGui::GetFrameHeight()));
    const auto lo = static_cast<float>(min);
    const auto hi = static_cast<float>(max);
    const float current = std::clamp(static_cast<float>(*value), std::min(lo, hi), std::max(lo, hi));
    float t = 0.0F;
    if (hi != lo) { t = logarithmic ? std::log(current / lo) / std::log(hi / lo) : (current - lo) / (hi - lo); }
    // ImGui gives an int slider a one-unit grab after a fixed 2 px pad, so its minimum shows one unit filled.
    constexpr float kGrabPadding = 2.0F;
    const float lead = std::is_same_v<T, int>
                         ? kGrabPadding + (frame.GetWidth() - 2.0F * kGrabPadding) / (std::abs(hi - lo) + 1.0F)
                         : 0.0F;
    const bool active = ImGui::GetActiveID() == window->GetID(strId);
    window->DrawList->AddRectFilled(frame.Min, frame.Max, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
    ImGui::RenderRectFilledInRangeH(window->DrawList,
      frame,
      ImGui::GetColorU32(studioWithAlpha(studio_palette::kAccent, active ? 0.32F : 0.2F)),
      frame.Min.x,
      ImLerp(frame.Min.x + lead, frame.Max.x, t),
      style.FrameRounding);

    const ImVec4 clear(0.0F, 0.0F, 0.0F, 0.0F);
    const ImVec4 tint = studioWithAlpha(studio_palette::kText, 0.04F);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, tint);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, tint);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, clear);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, clear);
    // A 1 px grab maps the mouse across the whole frame, as the fill suggests.
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 1.0F);
    bool changed = false;
    if constexpr (std::is_same_v<T, float>) {
        changed = ImGui::SliderFloat(strId, value, min, max, format != nullptr ? format : "%.3f", flags);
    } else {
        changed = ImGui::SliderInt(strId, value, min, max, format != nullptr ? format : "%d", flags);
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    return changed;
}

// DragFloat3 with the palette's axis markers instead of ImGui's pure RGB ones; `strId` is an id, never drawn.
inline auto studioDragAxes(const char *strId,
  std::span<float, 3> values,
  float speed,
  float min,
  float max,
  const char *format,
  ImGuiSliderFlags flags = ImGuiSliderFlags_None) -> bool
{
    if (ImGui::GetCurrentWindow()->SkipItems) { return false; }
    static constexpr std::array kAxes = { studio_palette::kAxisX, studio_palette::kAxisY, studio_palette::kAxisZ };
    bool changed = false;
    ImGui::BeginGroup();
    ImGui::PushID(strId);
    ImGui::PushMultiItemsWidths(static_cast<int>(kAxes.size()), ImGui::CalcItemWidth());
    for (std::size_t axis = 0; axis < kAxes.size(); ++axis) {
        ImGui::PushID(static_cast<int>(axis));
        if (axis > 0) { ImGui::SameLine(0.0F, ImGui::GetStyle().ItemInnerSpacing.x); }
        // Raw colour: DragScalar applies style.Alpha itself, so GetColorU32 here would fade disabled markers twice.
        ImGui::SetNextItemColorMarker(ImGui::ColorConvertFloat4ToU32(kAxes[axis]));
        changed |= ImGui::DragFloat("", &values[axis], speed, min, max, format, flags);
        ImGui::PopID();
        ImGui::PopItemWidth();
    }
    ImGui::PopID();
    ImGui::EndGroup();
    return changed;
}

// A key as a small raised cap; text after it on the same row, table rows included, centres on it.
inline void studioKeycap(const char *key)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems) { return; }
    const float lineHeight = ImGui::GetTextLineHeight();
    const float height = lineHeight + 2.0F * std::max(1.0F, std::round(lineHeight * 0.14F));
    const float lip = std::max(1.0F, std::round(height * 0.06F));
    ImGui::PushFont(nullptr, kKataglyphisUiFontPixels * kStudioCaptionScale);
    const ImVec2 textSize = ImGui::CalcTextSize(key);
    const float width = std::max(height, textSize.x + 2.0F * std::round(height * 0.3F));
    const ImRect bb(window->DC.CursorPos, ImVec2(window->DC.CursorPos.x + width, window->DC.CursorPos.y + height));
    ImGui::ItemSize(bb, std::round((height - lineHeight) * 0.5F));
    if (ImGui::ItemAdd(bb, 0)) {
        // A darker lip under the face reads as a key rather than a tag.
        const float rounding = std::round(height * 0.22F);
        window->DrawList->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(studio_palette::kBase), rounding);
        window->DrawList->AddRectFilled(
          bb.Min, ImVec2(bb.Max.x, bb.Max.y - lip), ImGui::GetColorU32(studio_palette::kRaised), rounding);
        const ImVec2 textPos(std::round(bb.Min.x + (width - textSize.x) * 0.5F),
          std::round(bb.Min.y + (height - lip - textSize.y) * 0.5F));
        window->DrawList->AddText(textPos, ImGui::GetColorU32(studio_palette::kText), key);
    }
    ImGui::PopFont();
}

// A two-column property grid: muted labels left, full-width controls right.
inline auto beginStudioProperties(const char *strId, float labelWidth) -> bool
{
    if (!ImGui::BeginTable(strId, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
        return false;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

inline void studioPropertyLabel(const char *label, const char *tooltip = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, studio_palette::kTextMuted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (tooltip != nullptr) { ImGui::SetItemTooltip("%s", tooltip); }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

inline void endStudioProperties() { ImGui::EndTable(); }

// Sits on the text line like a glyph, menu and status bars included, whose text is offset by the frame padding.
inline void studioStatusDot(ImVec4 color)
{
    const float size = ImGui::GetTextLineHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float centerY = origin.y + ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset + size * 0.5F;
    ImGui::Dummy(ImVec2(size * 0.6F, size));
    ImGui::GetWindowDrawList()->AddCircleFilled(
      ImVec2(origin.x + size * 0.3F, centerY), size * 0.22F, ImGui::GetColorU32(color));
}

// One row of a 3-column metric table: name, a bar filled to `fraction`, and the value right-aligned.
inline void studioMetricRow(const char *name, float fraction, const char *value, bool active)
{
    ImGui::TableNextRow();
    ImGui::PushStyleColor(ImGuiCol_Text, active ? studio_palette::kText : studio_palette::kTextDisabled);

    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(name);

    ImGui::TableSetColumnIndex(1);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float barWidth = ImGui::GetContentRegionAvail().x;
    const float lineHeight = ImGui::GetTextLineHeight();
    const float barHeight = std::max(3.0F, std::round(lineHeight * 0.32F));
    const float top = origin.y + (lineHeight - barHeight) * 0.5F;
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(ImVec2(origin.x, top),
      ImVec2(origin.x + barWidth, top + barHeight),
      ImGui::GetColorU32(ImGuiCol_FrameBg),
      barHeight * 0.5F);
    const float filled = std::clamp(fraction, 0.0F, 1.0F);
    if (active && filled > 0.0F) {
        drawList->AddRectFilled(ImVec2(origin.x, top),
          ImVec2(origin.x + std::max(barHeight, barWidth * filled), top + barHeight),
          ImGui::GetColorU32(ImGuiCol_PlotHistogram),
          barHeight * 0.5F);
    }
    ImGui::Dummy(ImVec2(barWidth, lineHeight));

    ImGui::TableSetColumnIndex(2);
    const float valueWidth = ImGui::CalcTextSize(value).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, ImGui::GetContentRegionAvail().x - valueWidth));
    ImGui::TextUnformatted(value);

    ImGui::PopStyleColor();
}
}// namespace Kataglyphis::Frontend
