module;

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.render_stage_registry;

import kataglyphis.vulkan.gui_renderer_shared_vars;

export namespace Kataglyphis {

// What a stage sees of the frame it records into; anything else it needs, its closures capture.
struct RenderStageFrame
{
    vk::CommandBuffer commandBuffer{};
    uint32_t imageIndex = 0;
    // The scene passes draw at the render-scaled scene extent; post, FXAA and the swapchain image at the output's.
    vk::Extent2D sceneExtent{};
    vk::Extent2D outputExtent{};
    vk::Image swapchainImage{};
    vk::ImageView swapchainImageView{};
    vk::Format swapchainFormat = vk::Format::eUndefined;
    // Ray or path tracing draws this frame, so the raster and shadow stages stand down.
    bool tracingOwnsFrame = false;
};

struct RenderStageResize
{
    vk::Extent2D sceneExtent{};
    vk::Extent2D outputExtent{};
    uint32_t imageCount = 0;
};

// The built-in stages' slots; a new stage takes a free value between them.
namespace RenderStageOrder {
    inline constexpr int ShadowCascades = 100;
    // Only while the scene has a shadowed point or spot light.
    inline constexpr int PunctualShadows = 150;
    inline constexpr int Main = 200;
    inline constexpr int Clouds = 300;
    // Only while the clouds march at half resolution.
    inline constexpr int CloudUpsample = 350;
    // Only with auto exposure on.
    inline constexpr int Histogram = 400;
    inline constexpr int ExposureReduce = 410;
    // Only above zero bloom strength.
    inline constexpr int Bloom = 450;
    inline constexpr int Post = 500;
    // Only while FXAA filters the frame; post then renders into its LDR intermediate and this stage presents.
    inline constexpr int Fxaa = 520;
}// namespace RenderStageOrder

// Creation is the registrant's, before add(); its resources go with the closures that capture them.
struct RenderStage
{
    std::string name;
    // Recorded in ascending order, never in the order the stages were added.
    int order = 0;
    std::optional<VulkanRendererInternals::FrontendShared::GpuTimedPass> timedPass;
    // Wraps the stage in a debug label named after it; none for a stage that labels its own sub-passes.
    std::optional<std::array<float, 4>> labelColor;
    // Unset records every frame.
    std::function<bool(const RenderStageFrame &)> enabled;
    std::function<void(const RenderStageFrame &)> record;
    // Runs instead of record on a frame enabled() turned down.
    std::function<void(const RenderStageFrame &)> recordSkipped;
    // After a swapchain recreation or a render-scale change, for the extent-sized resources.
    std::function<void(const RenderStageResize &)> resize;
    std::function<void()> shaderHotReload;
};

class RenderStageRegistry
{
  public:
    enum class AddResult : std::uint8_t {
        Added,
        EmptyName,
        NoRecordHook,
        DuplicateName,
        DuplicateOrder,
        DuplicateTimedPass,
    };

    // Called with true before a recorded stage and false after it, e.g. for its label and timestamps.
    using Bracket = std::function<void(const RenderStage &, bool begin)>;

    AddResult add(RenderStage stage)
    {
        if (stage.name.empty()) { return AddResult::EmptyName; }
        if (!stage.record) { return AddResult::NoRecordHook; }
        for (const RenderStage &existing : stages) {
            if (existing.name == stage.name) { return AddResult::DuplicateName; }
            if (existing.order == stage.order) { return AddResult::DuplicateOrder; }
            if (stage.timedPass.has_value() && existing.timedPass == stage.timedPass) {
                return AddResult::DuplicateTimedPass;
            }
        }
        const auto position = std::ranges::upper_bound(stages, stage.order, {}, &RenderStage::order);
        stages.insert(position, std::move(stage));
        return AddResult::Added;
    }

    bool remove(std::string_view name)
    {
        return std::erase_if(stages, [name](const RenderStage &stage) { return stage.name == name; }) > 0;
    }

    // Not re-entrant: a hook must not add or remove stages.
    void recordFrame(const RenderStageFrame &frame, const Bracket &bracket)
    {
        last_recorded.clear();
        for (const RenderStage &stage : stages) {
            if (stage.enabled && !stage.enabled(frame)) {
                if (stage.recordSkipped) { stage.recordSkipped(frame); }
                continue;
            }
            if (bracket) { bracket(stage, true); }
            stage.record(frame);
            if (bracket) { bracket(stage, false); }
            last_recorded.push_back(stage.name);
        }
    }

    void resize(const RenderStageResize &resize_info) const
    {
        for (const RenderStage &stage : stages) {
            if (stage.resize) { stage.resize(resize_info); }
        }
    }

    void shaderHotReload() const
    {
        for (const RenderStage &stage : stages) {
            if (stage.shaderHotReload) { stage.shaderHotReload(); }
        }
    }

    [[nodiscard]] std::vector<std::string> names() const
    {
        std::vector<std::string> result;
        result.reserve(stages.size());
        for (const RenderStage &stage : stages) { result.push_back(stage.name); }
        return result;
    }

    // The stages the last recordFrame recorded, in order; a disabled one is absent.
    [[nodiscard]] const std::vector<std::string> &lastRecorded() const { return last_recorded; }

  private:
    std::vector<RenderStage> stages;
    std::vector<std::string> last_recorded;
};

}// namespace Kataglyphis
