#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

import kataglyphis.vulkan.gui_renderer_shared_vars;
import kataglyphis.vulkan.render_stage_registry;

namespace {

using Kataglyphis::RenderStage;
using Kataglyphis::RenderStageFrame;
using Kataglyphis::RenderStageRegistry;
using Kataglyphis::RenderStageResize;
using Kataglyphis::VulkanRendererInternals::FrontendShared::GpuTimedPass;
using AddResult = Kataglyphis::RenderStageRegistry::AddResult;

// A stage that logs "<hook> <name>" from each of its record, resize and reload hooks.
RenderStage loggingStage(const std::string &name, int order, std::vector<std::string> &log)
{
    RenderStage stage;
    stage.name = name;
    stage.order = order;
    stage.record = [name, &log](const RenderStageFrame &) { log.push_back("record " + name); };
    stage.resize = [name, &log](const RenderStageResize &) { log.push_back("resize " + name); };
    stage.shaderHotReload = [name, &log] { log.push_back("reload " + name); };
    return stage;
}

}// namespace

TEST(RenderStageRegistry, BuiltInSlotsKeepTheFrameOrder)
{
    namespace Order = Kataglyphis::RenderStageOrder;
    EXPECT_LT(Order::ShadowCascades, Order::PunctualShadows);
    EXPECT_LT(Order::PunctualShadows, Order::Main);
    EXPECT_LT(Order::Main, Order::Clouds);
    EXPECT_LT(Order::Clouds, Order::CloudUpsample);
    EXPECT_LT(Order::CloudUpsample, Order::Post);
    EXPECT_LT(Order::Post, Order::Fxaa) << "FXAA filters what post wrote";
}

TEST(RenderStageRegistry, RecordsByOrderNotByRegistration)
{
    std::vector<std::string> log;
    RenderStageRegistry registry;
    ASSERT_EQ(registry.add(loggingStage("post", 500, log)), AddResult::Added);
    ASSERT_EQ(registry.add(loggingStage("shadow", 100, log)), AddResult::Added);
    ASSERT_EQ(registry.add(loggingStage("between", 450, log)), AddResult::Added);
    ASSERT_EQ(registry.add(loggingStage("main", 200, log)), AddResult::Added);

    registry.recordFrame(RenderStageFrame{}, {});

    const std::vector<std::string> expected = { "record shadow", "record main", "record between", "record post" };
    EXPECT_EQ(log, expected);
    EXPECT_EQ(registry.lastRecorded(), (std::vector<std::string>{ "shadow", "main", "between", "post" }));
    EXPECT_EQ(registry.names(), registry.lastRecorded());
}

TEST(RenderStageRegistry, RefusesAmbiguousOrIncompleteStages)
{
    std::vector<std::string> log;
    RenderStageRegistry registry;
    RenderStage timed = loggingStage("main", 200, log);
    timed.timedPass = GpuTimedPass::Main;
    ASSERT_EQ(registry.add(std::move(timed)), AddResult::Added);

    EXPECT_EQ(registry.add(loggingStage("", 300, log)), AddResult::EmptyName);
    EXPECT_EQ(registry.add(loggingStage("main", 300, log)), AddResult::DuplicateName);
    EXPECT_EQ(registry.add(loggingStage("other", 200, log)), AddResult::DuplicateOrder);

    RenderStage second_main_timer = loggingStage("other", 300, log);
    second_main_timer.timedPass = GpuTimedPass::Main;
    EXPECT_EQ(registry.add(std::move(second_main_timer)), AddResult::DuplicateTimedPass);

    RenderStage silent = loggingStage("silent", 400, log);
    silent.record = nullptr;
    EXPECT_EQ(registry.add(std::move(silent)), AddResult::NoRecordHook);

    EXPECT_EQ(registry.names(), (std::vector<std::string>{ "main" }));
}

TEST(RenderStageRegistry, BracketWrapsEachRecordedStageOnly)
{
    std::vector<std::string> log;
    RenderStageRegistry registry;
    ASSERT_EQ(registry.add(loggingStage("a", 1, log)), AddResult::Added);
    RenderStage off = loggingStage("off", 2, log);
    off.enabled = [](const RenderStageFrame &frame) { return !frame.tracingOwnsFrame; };
    off.recordSkipped = [&log](const RenderStageFrame &) { log.emplace_back("skipped off"); };
    ASSERT_EQ(registry.add(std::move(off)), AddResult::Added);

    RenderStageFrame frame{};
    frame.tracingOwnsFrame = true;
    registry.recordFrame(
      frame, [&log](const RenderStage &stage, bool begin) { log.push_back((begin ? "begin " : "end ") + stage.name); });

    const std::vector<std::string> expected = { "begin a", "record a", "end a", "skipped off" };
    EXPECT_EQ(log, expected);
    EXPECT_EQ(registry.lastRecorded(), (std::vector<std::string>{ "a" }));

    log.clear();
    frame.tracingOwnsFrame = false;
    registry.recordFrame(frame, {});
    EXPECT_EQ(log, (std::vector<std::string>{ "record a", "record off" }));
    EXPECT_EQ(registry.lastRecorded(), (std::vector<std::string>{ "a", "off" }));
}

TEST(RenderStageRegistry, ResizeAndReloadReachEveryStageThatHasTheHook)
{
    std::vector<std::string> log;
    RenderStageRegistry registry;
    ASSERT_EQ(registry.add(loggingStage("late", 20, log)), AddResult::Added);
    RenderStage record_only = loggingStage("record_only", 15, log);
    record_only.resize = nullptr;
    record_only.shaderHotReload = nullptr;
    ASSERT_EQ(registry.add(std::move(record_only)), AddResult::Added);
    ASSERT_EQ(registry.add(loggingStage("early", 10, log)), AddResult::Added);

    registry.resize(RenderStageResize{ .extent = vk::Extent2D{ 64, 32 }, .imageCount = 3 });
    registry.shaderHotReload();

    const std::vector<std::string> expected = { "resize early", "resize late", "reload early", "reload late" };
    EXPECT_EQ(log, expected);
}

TEST(RenderStageRegistry, ResizeHandsTheNewExtentToTheStage)
{
    RenderStageRegistry registry;
    RenderStageResize seen{};
    RenderStage stage;
    stage.name = "sized";
    stage.record = [](const RenderStageFrame &) {};
    stage.resize = [&seen](const RenderStageResize &resize) { seen = resize; };
    ASSERT_EQ(registry.add(std::move(stage)), AddResult::Added);

    registry.resize(RenderStageResize{ .extent = vk::Extent2D{ 640, 360 }, .imageCount = 4 });
    EXPECT_EQ(seen.extent, (vk::Extent2D{ 640, 360 }));
    EXPECT_EQ(seen.imageCount, 4U);
}

TEST(RenderStageRegistry, RemovedStageNoLongerRecordsAndFreesItsName)
{
    std::vector<std::string> log;
    RenderStageRegistry registry;
    ASSERT_EQ(registry.add(loggingStage("kept", 1, log)), AddResult::Added);
    ASSERT_EQ(registry.add(loggingStage("gone", 2, log)), AddResult::Added);

    EXPECT_TRUE(registry.remove("gone"));
    EXPECT_FALSE(registry.remove("gone"));
    registry.recordFrame(RenderStageFrame{}, {});
    EXPECT_EQ(log, (std::vector<std::string>{ "record kept" }));
    EXPECT_EQ(registry.add(loggingStage("gone", 2, log)), AddResult::Added);
}
