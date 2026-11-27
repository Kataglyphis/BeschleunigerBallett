#include <gtest/gtest.h>

#include "renderer/BreadcrumbTrail.hpp"
#include "vulkan_base/PhysicalDeviceChoices.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Kataglyphis::BreadcrumbBackend;
using Kataglyphis::VulkanRendererInternals::analyzeBreadcrumbs;
using Kataglyphis::VulkanRendererInternals::BreadcrumbFrameReport;
using Kataglyphis::VulkanRendererInternals::BreadcrumbPassed;
using Kataglyphis::VulkanRendererInternals::BreadcrumbProgress;
using Kataglyphis::VulkanRendererInternals::BreadcrumbRing;
using Kataglyphis::VulkanRendererInternals::BreadcrumbState;
using Kataglyphis::VulkanRendererInternals::BreadcrumbTrail;
using Kataglyphis::VulkanRendererInternals::CheckpointSample;
using Kataglyphis::VulkanRendererInternals::DeviceFaultAddress;
using Kataglyphis::VulkanRendererInternals::DeviceFaultInfo;
using Kataglyphis::VulkanRendererInternals::DeviceFaultQuery;
using Kataglyphis::VulkanRendererInternals::DeviceFaultVendor;
using Kataglyphis::VulkanRendererInternals::formatDeviceLossReport;
using Kataglyphis::VulkanRendererInternals::lastWrittenMarker;
using Kataglyphis::VulkanRendererInternals::passedFromCheckpoints;

// A path-traced frame with bloom off: main with the tracer inside it, the exposure reduce, post with the editor inside.
void recordPathTracedFrame(BreadcrumbRing &ring, std::uint32_t slot)
{
    ring.beginRecording(slot);
    ring.beginScope("main");
    ring.beginScope("pathtracing");
    ring.endScope();
    ring.endScope();
    ring.beginScope("exposure_reduce");
    ring.endScope();
    ring.beginScope("post");
    ring.beginScope("gui");
    ring.endScope();
    ring.endScope();
    ring.endRecording();
}

// The index of the scope with this path, or nullopt.
std::optional<std::uint32_t> scopeAt(const BreadcrumbTrail &trail, std::string_view path)
{
    for (std::uint32_t scope = 0; scope < trail.scopes().size(); ++scope) {
        if (trail.path(scope) == path) { return scope; }
    }
    return std::nullopt;
}

std::string pathOf(const BreadcrumbTrail &trail, std::optional<std::uint32_t> scope)
{ return scope ? trail.path(*scope) : std::string("<none>"); }

// recordPathTracedFrame's events from the trail's first: main 0 and 3, tracer 1-2, reduce 4-5, post 6 and 9, gui 7-8.
BreadcrumbPassed passedAt(const BreadcrumbTrail &trail,
  std::optional<std::uint32_t> topEvent,
  std::optional<std::uint32_t> bottomEvent)
{
    const auto marker = [&trail](std::optional<std::uint32_t> event) -> std::optional<std::uint32_t> {
        if (!event) { return std::nullopt; }
        return trail.firstMarker() + *event;
    };
    return BreadcrumbPassed{ .top = marker(topEvent), .bottom = marker(bottomEvent) };
}

BreadcrumbProgress progressOf(BreadcrumbState state)
{
    BreadcrumbProgress progress{};
    progress.state = state;
    return progress;
}

DeviceFaultInfo faultOf(DeviceFaultQuery query, vk::Result result = vk::Result::eSuccess)
{
    DeviceFaultInfo fault{};
    fault.query = query;
    fault.result = result;
    return fault;
}

void expectEveryLineInTheBlock(const std::vector<std::string> &lines)
{
    ASSERT_FALSE(lines.empty());
    for (const std::string &line : lines) {
        EXPECT_TRUE(line.starts_with("Device loss") || line.starts_with("  "))
          << "a line one grep for the block would miss: " << line;
    }
}

}// namespace

TEST(GpuBreadcrumbsUnit, NestedScopesNameTheStageTheyRecordInside)
{
    BreadcrumbRing ring;
    ring.resize(2);
    recordPathTracedFrame(ring, 1);

    const BreadcrumbTrail &trail = ring.trail(1);
    ASSERT_EQ(trail.scopes().size(), 5U);
    EXPECT_TRUE(scopeAt(trail, "exposure_reduce").has_value());
    EXPECT_TRUE(scopeAt(trail, "main > pathtracing").has_value()) << "the path tracer lost the stage it records in";
    EXPECT_TRUE(scopeAt(trail, "post > gui").has_value());
    EXPECT_FALSE(scopeAt(trail, "pathtracing").has_value()) << "a nested scope was recorded as a top-level one";
    EXPECT_EQ(trail.eventCount(), 10U);
    EXPECT_FALSE(trail.truncated());
}

TEST(GpuBreadcrumbsUnit, MarkersRunWithoutAGapAcrossFramesAndNeverUseZero)
{
    BreadcrumbRing ring;
    ring.resize(2);
    recordPathTracedFrame(ring, 0);
    recordPathTracedFrame(ring, 1);

    const BreadcrumbTrail &first = ring.trail(0);
    const BreadcrumbTrail &second = ring.trail(1);
    EXPECT_GT(first.firstMarker(), 0U) << "0 is what an unwritten marker slot holds";
    ASSERT_TRUE(first.lastMarker().has_value());
    EXPECT_EQ(second.firstMarker(), *first.lastMarker() + 1U);
    EXPECT_EQ(second.serial(), first.serial() + 1U);

    const BreadcrumbTrail &trail = ring.trail(0);
    const std::uint32_t pathtracing = *scopeAt(trail, "main > pathtracing");
    const std::uint32_t main = *scopeAt(trail, "main");
    EXPECT_LT(trail.scopes()[main].beginMarker, trail.scopes()[pathtracing].beginMarker);
    EXPECT_LT(trail.scopes()[pathtracing].endMarker, trail.scopes()[main].endMarker);
}

TEST(GpuBreadcrumbsUnit, EventsOutsideARecordingOrAnUnmatchedEndMarkNothing)
{
    BreadcrumbRing ring;
    ring.resize(1);
    EXPECT_FALSE(ring.beginScope("before").has_value()) << "a scope outside any recording got a marker";

    ring.beginRecording(0);
    EXPECT_FALSE(ring.endScope().has_value()) << "an end with no open scope got a marker";
    const auto begun = ring.beginScope("main");
    ASSERT_TRUE(begun.has_value());
    EXPECT_EQ(begun->index, 0U);
    ring.endRecording();
    EXPECT_FALSE(ring.endScope().has_value());

    // A slot past the ring records nothing rather than another slot's trail.
    ring.beginRecording(5);
    EXPECT_FALSE(ring.recordingSlot().has_value());
    EXPECT_FALSE(ring.beginScope("lost").has_value());
}

TEST(GpuBreadcrumbsUnit, AFullTrailStopsMarkingAndSaysSo)
{
    BreadcrumbRing ring(3);
    ring.resize(1);
    ring.beginRecording(0);
    ASSERT_TRUE(ring.beginScope("main").has_value());
    ASSERT_TRUE(ring.beginScope("forward").has_value());
    ASSERT_TRUE(ring.endScope().has_value());
    // The fourth event has no marker slot: refused, and so is the begin after it and its end.
    EXPECT_FALSE(ring.endScope().has_value());
    EXPECT_FALSE(ring.beginScope("post").has_value());
    EXPECT_FALSE(ring.endScope().has_value());
    ring.endRecording();

    const BreadcrumbTrail &trail = ring.trail(0);
    EXPECT_TRUE(trail.truncated());
    EXPECT_EQ(trail.eventCount(), 3U);
    ASSERT_EQ(trail.scopes().size(), 2U);
    EXPECT_EQ(trail.scopes()[*scopeAt(trail, "main")].endMarker, 0U) << "main closed on an event it had no slot for";
    EXPECT_NE(trail.scopes()[*scopeAt(trail, "main > forward")].endMarker, 0U);
}

TEST(GpuBreadcrumbsUnit, OnlySubmittedTrailsAreReportedOldestFirst)
{
    BreadcrumbRing ring;
    ring.resize(3);
    recordPathTracedFrame(ring, 1);
    ring.markSubmitted(1);
    recordPathTracedFrame(ring, 2);
    recordPathTracedFrame(ring, 0);
    ring.markSubmitted(0);
    EXPECT_EQ(ring.submittedSlots(), (std::vector<std::uint32_t>{ 1U, 0U }));

    // Recording a slot again starts a trail nothing submitted yet.
    ring.beginRecording(1);
    ring.markSubmitted(1);
    EXPECT_EQ(ring.submittedSlots(), (std::vector<std::uint32_t>{ 0U }));
    ring.endRecording();
}

TEST(GpuBreadcrumbsUnit, ABottomMarkerAtOrPastTheTrailsLastCompletesIt)
{
    BreadcrumbRing ring;
    ring.resize(1);
    recordPathTracedFrame(ring, 0);
    const BreadcrumbTrail &trail = ring.trail(0);

    EXPECT_EQ(analyzeBreadcrumbs(trail, passedAt(trail, 9U, 9U)).state, BreadcrumbState::Completed);
    // A queue-wide checkpoint in a later frame covers this one too.
    EXPECT_EQ(analyzeBreadcrumbs(trail, passedAt(trail, 40U, 30U)).state, BreadcrumbState::Completed);
    EXPECT_NE(analyzeBreadcrumbs(trail, passedAt(trail, 9U, 8U)).state, BreadcrumbState::Completed);
}

TEST(GpuBreadcrumbsUnit, MarkersOfAnEarlierTrailMeanThisOneNeverStarted)
{
    BreadcrumbRing ring;
    ring.resize(1);
    recordPathTracedFrame(ring, 0);
    recordPathTracedFrame(ring, 0);
    const BreadcrumbTrail &trail = ring.trail(0);

    const BreadcrumbPassed earlier{ .top = trail.firstMarker() - 1U, .bottom = trail.firstMarker() - 2U };
    EXPECT_EQ(analyzeBreadcrumbs(trail, earlier).state, BreadcrumbState::NotStarted);
    EXPECT_EQ(analyzeBreadcrumbs(trail, BreadcrumbPassed{}).state, BreadcrumbState::NotStarted);
}

TEST(GpuBreadcrumbsUnit, AHangInThePathTracerNamesItInsideMain)
{
    BreadcrumbRing ring;
    ring.resize(1);
    recordPathTracedFrame(ring, 0);
    const BreadcrumbTrail &trail = ring.trail(0);

    // Everything before the tracer finished, the tracer did not, and the front end ran on into the reduce.
    const BreadcrumbProgress in_tracer = analyzeBreadcrumbs(trail, passedAt(trail, 5U, 1U));
    EXPECT_EQ(in_tracer.state, BreadcrumbState::InProgress);
    EXPECT_FALSE(in_tracer.lastCompleted.has_value()) << pathOf(trail, in_tracer.lastCompleted);
    EXPECT_EQ(pathOf(trail, in_tracer.firstUnfinished), "main > pathtracing");
    EXPECT_TRUE(in_tracer.firstUnfinishedStarted);

    // The front end reached the tracer, but main's own commands before it are the ones not finished.
    const BreadcrumbProgress before_tracer = analyzeBreadcrumbs(trail, passedAt(trail, 1U, 0U));
    EXPECT_EQ(pathOf(trail, before_tracer.firstUnfinished), "main");
    EXPECT_TRUE(before_tracer.firstUnfinishedStarted);

    // Main done and the reduce not yet reached.
    const BreadcrumbProgress between = analyzeBreadcrumbs(trail, passedAt(trail, 3U, 3U));
    EXPECT_EQ(pathOf(trail, between.lastCompleted), "main");
    EXPECT_EQ(pathOf(trail, between.firstUnfinished), "exposure_reduce");
    EXPECT_FALSE(between.firstUnfinishedStarted);

    // The tracer done, main's tail not, while the front end ran on into post's editor.
    const BreadcrumbProgress after_tracer = analyzeBreadcrumbs(trail, passedAt(trail, 7U, 2U));
    EXPECT_EQ(pathOf(trail, after_tracer.lastCompleted), "main > pathtracing");
    EXPECT_EQ(pathOf(trail, after_tracer.firstUnfinished), "main");

    // Nothing finished at all.
    const BreadcrumbProgress first_scope = analyzeBreadcrumbs(trail, passedAt(trail, 0U, std::nullopt));
    EXPECT_FALSE(first_scope.lastCompleted.has_value());
    EXPECT_EQ(pathOf(trail, first_scope.firstUnfinished), "main");
}

TEST(GpuBreadcrumbsUnit, AHangInPostsDrawNamesPostNotTheEditorRecordedAfterIt)
{
    BreadcrumbRing ring;
    ring.resize(1);
    recordPathTracedFrame(ring, 0);
    const BreadcrumbTrail &trail = ring.trail(0);

    // Post's fullscreen draw hung, and the front end wrote the editor's top markers behind it anyway.
    const BreadcrumbProgress in_post = analyzeBreadcrumbs(trail, passedAt(trail, 8U, 6U));
    EXPECT_EQ(pathOf(trail, in_post.lastCompleted), "exposure_reduce");
    EXPECT_EQ(pathOf(trail, in_post.firstUnfinished), "post");
    EXPECT_TRUE(in_post.firstUnfinishedStarted);

    // Once that draw finished, the editor's own commands are the ones left.
    const BreadcrumbProgress in_editor = analyzeBreadcrumbs(trail, passedAt(trail, 9U, 7U));
    EXPECT_EQ(pathOf(trail, in_editor.firstUnfinished), "post > gui");
    EXPECT_TRUE(in_editor.firstUnfinishedStarted);
}

TEST(GpuBreadcrumbsUnit, ATopBehindTheBottomIsStale)
{
    BreadcrumbRing ring;
    ring.resize(1);
    recordPathTracedFrame(ring, 0);
    const BreadcrumbTrail &trail = ring.trail(0);

    const BreadcrumbProgress progress = analyzeBreadcrumbs(trail, passedAt(trail, std::nullopt, 2U));
    EXPECT_EQ(progress.state, BreadcrumbState::InProgress);
    EXPECT_EQ(pathOf(trail, progress.lastCompleted), "main > pathtracing");
    EXPECT_EQ(pathOf(trail, progress.firstUnfinished), "main");
    EXPECT_TRUE(progress.firstUnfinishedStarted);
}

TEST(GpuBreadcrumbsUnit, OnlyMarkerSlotsHoldingThisTrailsMarkersCountAsReached)
{
    const std::array<std::uint32_t, 5> partly{ 101U, 102U, 103U, 7U, 0U };
    EXPECT_EQ(lastWrittenMarker(partly, 101U), 103U);
    // Slots an older trail wrote match none of this trail's markers.
    const std::array<std::uint32_t, 3> stale{ 7U, 8U, 9U };
    EXPECT_FALSE(lastWrittenMarker(stale, 101U).has_value());
    EXPECT_FALSE(lastWrittenMarker(std::span<const std::uint32_t>{}, 101U).has_value());
}

TEST(GpuBreadcrumbsUnit, CheckpointsReadTheTopAndTheBottomOfThePipe)
{
    const std::array<CheckpointSample, 2> both{ { { vk::PipelineStageFlagBits2::eTopOfPipe, 9U },
      { vk::PipelineStageFlagBits2::eBottomOfPipe, 7U } } };
    const BreadcrumbPassed passed = passedFromCheckpoints(both);
    EXPECT_EQ(passed.top, 9U);
    EXPECT_EQ(passed.bottom, 7U);

    // A null marker at the bottom: nothing finished.
    const std::array<CheckpointSample, 2> none_done{ { { vk::PipelineStageFlagBits2::eTopOfPipe, 3U },
      { vk::PipelineStageFlagBits2::eBottomOfPipe, 0U } } };
    EXPECT_FALSE(passedFromCheckpoints(none_done).bottom.has_value());

    const std::array<CheckpointSample, 2> other_stages{ { { vk::PipelineStageFlagBits2::eFragmentShader, 8U },
      { vk::PipelineStageFlagBits2::eComputeShader, 5U } } };
    const BreadcrumbPassed bounded = passedFromCheckpoints(other_stages);
    EXPECT_EQ(bounded.top, 8U);
    EXPECT_EQ(bounded.bottom, 5U);
}

TEST(GpuBreadcrumbsUnit, WithoutMarkersTheReportListsTheScopesOfEachImagesLastFrame)
{
    BreadcrumbRing ring;
    ring.resize(2);
    recordPathTracedFrame(ring, 1);
    recordPathTracedFrame(ring, 0);
    const std::array<BreadcrumbFrameReport, 2> frames{ {
      { .slot = 1, .trail = &ring.trail(1), .progress = std::nullopt },
      { .slot = 0, .trail = &ring.trail(0), .progress = std::nullopt },
    } };

    const std::vector<std::string> lines =
      formatDeviceLossReport(BreadcrumbBackend::None, false, frames, DeviceFaultInfo{});
    expectEveryLineInTheBlock(lines);
    ASSERT_EQ(lines.size(), 4U);
    EXPECT_NE(lines[0].find("no marker extension"), std::string::npos) << lines[0];
    EXPECT_EQ(lines[1],
      std::string("  frame ") + std::to_string(ring.trail(1).serial())
        + " (image 1): main, main > pathtracing, exposure_reduce, post, post > gui");
    EXPECT_TRUE(lines[2].starts_with(std::string("  frame ") + std::to_string(ring.trail(0).serial()) + " (image 0)"));
    EXPECT_EQ(lines[3], "Device loss fault info: VK_EXT_device_fault is not enabled");
}

TEST(GpuBreadcrumbsUnit, WithMarkersTheReportSaysWhereEachFrameStopped)
{
    BreadcrumbRing ring;
    ring.resize(3);
    recordPathTracedFrame(ring, 0);
    recordPathTracedFrame(ring, 1);
    recordPathTracedFrame(ring, 2);
    const BreadcrumbTrail &hung = ring.trail(1);
    const std::array<BreadcrumbFrameReport, 3> frames{ {
      { .slot = 0, .trail = &ring.trail(0), .progress = progressOf(BreadcrumbState::Completed) },
      { .slot = 1, .trail = &hung, .progress = analyzeBreadcrumbs(hung, passedAt(hung, 8U, 6U)) },
      { .slot = 2, .trail = &ring.trail(2), .progress = progressOf(BreadcrumbState::NotStarted) },
    } };

    const std::vector<std::string> lines =
      formatDeviceLossReport(BreadcrumbBackend::BufferMarker, true, frames, DeviceFaultInfo{});
    expectEveryLineInTheBlock(lines);
    ASSERT_EQ(lines.size(), 5U);
    EXPECT_NE(lines[0].find("VK_AMD_buffer_marker"), std::string::npos) << lines[0];
    EXPECT_TRUE(lines[1].ends_with("(image 0): completed")) << lines[1];
    EXPECT_TRUE(lines[2].ends_with("(image 1): last completed 'exposure_reduce', first unfinished 'post' (started)"))
      << lines[2];
    EXPECT_TRUE(lines[3].ends_with("(image 2): not started")) << lines[3];

    const std::vector<std::string> unread =
      formatDeviceLossReport(BreadcrumbBackend::Checkpoints, false, std::span<const BreadcrumbFrameReport>{}, {});
    EXPECT_NE(unread[0].find("VK_NV_device_diagnostic_checkpoints, unread"), std::string::npos) << unread[0];
    EXPECT_EQ(unread[1], "  no frame was submitted");
}

TEST(GpuBreadcrumbsUnit, TheReportCarriesTheFaultDescriptionAddressesAndVendorInfo)
{
    DeviceFaultInfo fault = faultOf(DeviceFaultQuery::Reported);
    fault.description = "page fault in a ray-query shader";
    fault.addresses.push_back(DeviceFaultAddress{
      .type = vk::DeviceFaultAddressTypeEXT::eWriteInvalid, .address = 0x1234567U, .precision = 0x1000U });
    fault.addresses.push_back(DeviceFaultAddress{
      .type = vk::DeviceFaultAddressTypeEXT::eInstructionPointerFault, .address = 0xABCDU, .precision = 1U });
    fault.vendors.push_back(DeviceFaultVendor{ .description = "ring hang", .code = 0x2AU, .data = 0xBEEFU });

    const std::vector<std::string> lines =
      formatDeviceLossReport(BreadcrumbBackend::None, false, std::span<const BreadcrumbFrameReport>{}, fault);
    expectEveryLineInTheBlock(lines);
    ASSERT_EQ(lines.size(), 6U);
    EXPECT_EQ(lines[2], "Device loss fault info: page fault in a ray-query shader");
    EXPECT_EQ(lines[3], "  invalid write at 0x0000000001234000..0x0000000001234fff");
    EXPECT_EQ(lines[4], "  instruction pointer, fault at 0x000000000000abcd..0x000000000000abcd");
    EXPECT_EQ(lines[5], "  vendor fault 0x2a, data 0xbeef: ring hang");

    fault = faultOf(DeviceFaultQuery::Failed, vk::Result::eErrorOutOfHostMemory);
    EXPECT_EQ(formatDeviceLossReport(BreadcrumbBackend::None, false, {}, fault).back(),
      "Device loss fault info: vkGetDeviceFaultInfoEXT returned -1");
    fault = faultOf(DeviceFaultQuery::NotQueriedSimulated);
    EXPECT_EQ(formatDeviceLossReport(BreadcrumbBackend::None, false, {}, fault).back(),
      "Device loss fault info: not queried, the loss is simulated");
}

TEST(GpuBreadcrumbsUnit, ACutTrailSaysWhereItStops)
{
    BreadcrumbRing ring(2);
    ring.resize(1);
    recordPathTracedFrame(ring, 0);
    const std::array<BreadcrumbFrameReport, 1> frames{ {
      { .slot = 0, .trail = &ring.trail(0), .progress = std::nullopt },
    } };
    const std::vector<std::string> lines = formatDeviceLossReport(BreadcrumbBackend::None, false, frames, {});
    EXPECT_TRUE(lines[1].ends_with("(image 0): main, main > pathtracing; the trail stops at 2 events")) << lines[1];
}
