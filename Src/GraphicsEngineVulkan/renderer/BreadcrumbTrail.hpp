#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "vulkan_base/PhysicalDeviceChoices.hpp"

namespace Kataglyphis::VulkanRendererInternals {

inline constexpr std::uint32_t kNoBreadcrumbScope = std::numeric_limits<std::uint32_t>::max();
// Scope events one command buffer may mark; a frame records about thirty.
inline constexpr std::uint32_t kBreadcrumbEventCapacity = 512;

struct BreadcrumbScope
{
    std::string name;
    std::uint32_t parent = kNoBreadcrumbScope;
    std::uint32_t beginMarker = 0;
    // 0 while open, and for good when the trail ran out of events before the scope closed.
    std::uint32_t endMarker = 0;
};

// The marker the GPU writes at a scope's begin or end, and the event's index: its slot in the marker buffer.
struct BreadcrumbEvent
{
    std::uint32_t marker = 0;
    std::uint32_t index = 0;
};

// One command buffer's label scopes in begin order; its markers run from firstMarker without a gap.
class BreadcrumbTrail
{
  public:
    void restart(std::uint64_t serial, std::uint32_t firstMarker, std::uint32_t capacity)
    {
        serial_ = serial;
        first_marker_ = firstMarker;
        capacity_ = capacity;
        event_count_ = 0;
        scope_count_ = 0;
        refused_open_ = 0;
        truncated_ = false;
        submitted_ = false;
        open_.clear();
    }

    std::optional<BreadcrumbEvent> begin(std::string_view name)
    {
        if (event_count_ >= capacity_) {
            truncated_ = true;
            ++refused_open_;
            return std::nullopt;
        }
        // Reused across restarts, so a steady frame allocates no scope names.
        if (scope_count_ == scopes_.size()) { scopes_.emplace_back(); }
        BreadcrumbScope &scope = scopes_[scope_count_];
        scope.name.assign(name);
        scope.parent = open_.empty() ? kNoBreadcrumbScope : open_.back();
        scope.beginMarker = first_marker_ + event_count_;
        scope.endMarker = 0;
        open_.push_back(scope_count_);
        ++scope_count_;
        return BreadcrumbEvent{ .marker = scope.beginMarker, .index = event_count_++ };
    }

    std::optional<BreadcrumbEvent> end()
    {
        // The end of a refused begin, so it must not close the scope around it.
        if (refused_open_ > 0) {
            --refused_open_;
            return std::nullopt;
        }
        if (open_.empty()) { return std::nullopt; }
        BreadcrumbScope &scope = scopes_[open_.back()];
        open_.pop_back();
        if (event_count_ >= capacity_) {
            truncated_ = true;
            return std::nullopt;
        }
        scope.endMarker = first_marker_ + event_count_;
        return BreadcrumbEvent{ .marker = scope.endMarker, .index = event_count_++ };
    }

    void markSubmitted() { submitted_ = true; }

    [[nodiscard]] std::uint64_t serial() const { return serial_; }
    [[nodiscard]] std::uint32_t firstMarker() const { return first_marker_; }
    [[nodiscard]] std::uint32_t eventCount() const { return event_count_; }
    [[nodiscard]] std::uint32_t capacity() const { return capacity_; }
    [[nodiscard]] std::optional<std::uint32_t> lastMarker() const
    {
        if (event_count_ == 0) { return std::nullopt; }
        return first_marker_ + event_count_ - 1;
    }
    [[nodiscard]] std::span<const BreadcrumbScope> scopes() const { return { scopes_.data(), scope_count_ }; }
    [[nodiscard]] bool truncated() const { return truncated_; }
    [[nodiscard]] bool submitted() const { return submitted_; }

    // "main > pathtracing": the scope behind the ones it records inside.
    [[nodiscard]] std::string path(std::uint32_t scope) const
    {
        if (scope >= scope_count_) { return {}; }
        std::vector<std::uint32_t> chain;
        for (std::uint32_t at = scope; at != kNoBreadcrumbScope; at = scopes_[at].parent) { chain.push_back(at); }
        std::string joined;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            if (!joined.empty()) { joined += " > "; }
            joined += scopes_[*it].name;
        }
        return joined;
    }

  private:
    std::uint64_t serial_ = 0;
    std::uint32_t first_marker_ = 0;
    std::uint32_t capacity_ = 0;
    std::uint32_t event_count_ = 0;
    std::uint32_t scope_count_ = 0;
    std::uint32_t refused_open_ = 0;
    bool truncated_ = false;
    bool submitted_ = false;
    std::vector<BreadcrumbScope> scopes_;
    std::vector<std::uint32_t> open_;
};

// Each swapchain image's last trail, one per command buffer, behind one marker counter.
class BreadcrumbRing
{
  public:
    explicit BreadcrumbRing(std::uint32_t capacity = kBreadcrumbEventCapacity) : capacity_(capacity) {}

    void resize(std::uint32_t slots)
    {
        recording_.reset();
        trails_.assign(slots, BreadcrumbTrail{});
    }

    [[nodiscard]] std::uint32_t slotCount() const { return static_cast<std::uint32_t>(trails_.size()); }
    [[nodiscard]] std::uint32_t capacity() const { return capacity_; }
    [[nodiscard]] std::optional<std::uint32_t> recordingSlot() const { return recording_; }

    // The slot's last trail is done with: its command buffer is recorded again only after its fence signaled.
    void beginRecording(std::uint32_t slot)
    {
        endRecording();
        if (slot >= trails_.size()) { return; }
        // Marker 0 means "never written", and a trail's markers must not wrap past it.
        if (next_marker_ > std::numeric_limits<std::uint32_t>::max() - capacity_) { next_marker_ = 1; }
        trails_[slot].restart(++next_serial_, next_marker_, capacity_);
        recording_ = slot;
    }

    std::optional<BreadcrumbEvent> beginScope(std::string_view name)
    {
        if (!recording_) { return std::nullopt; }
        return trails_[*recording_].begin(name);
    }

    std::optional<BreadcrumbEvent> endScope()
    {
        if (!recording_) { return std::nullopt; }
        return trails_[*recording_].end();
    }

    void endRecording()
    {
        if (!recording_) { return; }
        next_marker_ += trails_[*recording_].eventCount();
        recording_.reset();
    }

    void markSubmitted(std::uint32_t slot)
    {
        if (slot < trails_.size() && recording_ != slot) { trails_[slot].markSubmitted(); }
    }

    // slot < slotCount().
    [[nodiscard]] const BreadcrumbTrail &trail(std::uint32_t slot) const { return trails_[slot]; }

    // The slots whose last trail reached the queue, oldest first.
    [[nodiscard]] std::vector<std::uint32_t> submittedSlots() const
    {
        std::vector<std::uint32_t> slots;
        for (std::uint32_t slot = 0; slot < trails_.size(); ++slot) {
            if (trails_[slot].submitted()) { slots.push_back(slot); }
        }
        std::ranges::sort(slots, {}, [this](std::uint32_t slot) { return trails_[slot].serial(); });
        return slots;
    }

  private:
    std::vector<BreadcrumbTrail> trails_;
    std::uint32_t capacity_;
    std::uint32_t next_marker_ = 1;
    std::uint64_t next_serial_ = 0;
    std::optional<std::uint32_t> recording_;
};

// The last markers the GPU passed at the top and at the bottom of the pipe; nullopt where it passed none.
struct BreadcrumbPassed
{
    std::optional<std::uint32_t> top;
    std::optional<std::uint32_t> bottom;
};

enum class BreadcrumbState : std::uint8_t { NotStarted, InProgress, Completed };

struct BreadcrumbProgress
{
    BreadcrumbState state = BreadcrumbState::NotStarted;
    std::optional<std::uint32_t> lastCompleted;
    std::optional<std::uint32_t> firstUnfinished;
    bool firstUnfinishedStarted = false;
};

// A bottom marker covers every command before it; a top marker only says the GPU's front end got that far.
inline BreadcrumbProgress analyzeBreadcrumbs(const BreadcrumbTrail &trail, const BreadcrumbPassed &passed)
{
    BreadcrumbProgress progress{};
    const std::optional<std::uint32_t> last = trail.lastMarker();
    if (!last || (passed.bottom && *passed.bottom >= *last)) {
        progress.state = BreadcrumbState::Completed;
        return progress;
    }
    const std::uint32_t first = trail.firstMarker();
    // Queue-wide markers may lie in another trail: an earlier one says nothing here, a later one covers it all.
    const auto within = [first, last](std::optional<std::uint32_t> marker) -> std::optional<std::uint32_t> {
        if (!marker || *marker < first) { return std::nullopt; }
        return std::min(*marker, *last);
    };
    const std::optional<std::uint32_t> bottom = within(passed.bottom);
    std::optional<std::uint32_t> top = within(passed.top);
    // Nothing completes before it starts, so a top behind the bottom is stale.
    if (bottom && (!top || *top < *bottom)) { top = bottom; }
    if (!top) { return progress; }
    progress.state = BreadcrumbState::InProgress;

    // The front end runs ahead of unfinished work, so only the bottom marker says whose commands are stuck.
    constexpr std::uint32_t kNeverClosed = std::numeric_limits<std::uint32_t>::max();
    const std::span<const BreadcrumbScope> scopes = trail.scopes();
    std::optional<std::uint32_t> open_at_bottom;
    std::optional<std::uint32_t> next_after_bottom;
    for (std::uint32_t index = 0; index < scopes.size(); ++index) {
        const BreadcrumbScope &scope = scopes[index];
        const std::uint32_t end = scope.endMarker == 0 ? kNeverClosed : scope.endMarker;
        if (bottom && end <= *bottom) {
            if (!progress.lastCompleted || end > scopes[*progress.lastCompleted].endMarker) {
                progress.lastCompleted = index;
            }
        } else if (bottom && scope.beginMarker <= *bottom) {
            // Scopes come in begin order and nest, so the last one open at the bottom marker is the innermost.
            open_at_bottom = index;
        } else if (!next_after_bottom) {
            next_after_bottom = index;
        }
    }
    if (open_at_bottom) {
        progress.firstUnfinished = open_at_bottom;
        progress.firstUnfinishedStarted = true;
    } else if (next_after_bottom) {
        progress.firstUnfinished = next_after_bottom;
        progress.firstUnfinishedStarted = scopes[*next_after_bottom].beginMarker <= *top;
    }
    return progress;
}

// One slot per event, so a slot holding this trail's marker is an event the GPU reached; an older trail's never does.
inline std::optional<std::uint32_t> lastWrittenMarker(std::span<const std::uint32_t> written, std::uint32_t firstMarker)
{
    std::optional<std::uint32_t> last;
    for (std::uint32_t index = 0; index < written.size(); ++index) {
        if (written[index] == firstMarker + index) { last = firstMarker + index; }
    }
    return last;
}

struct CheckpointSample
{
    vk::PipelineStageFlags2 stage;
    // 0 for a stage no checkpoint reached.
    std::uint32_t marker = 0;
};

// The top and the bottom of the pipe; with neither reported, the latest and the earliest stage bound them.
inline BreadcrumbPassed passedFromCheckpoints(std::span<const CheckpointSample> samples)
{
    BreadcrumbPassed passed{};
    std::optional<std::uint32_t> latest;
    std::optional<std::uint32_t> earliest;
    bool top_reported = false;
    bool bottom_reported = false;
    for (const CheckpointSample &sample : samples) {
        const std::optional<std::uint32_t> marker =
          sample.marker == 0 ? std::nullopt : std::optional<std::uint32_t>(sample.marker);
        if (sample.stage == vk::PipelineStageFlagBits2::eTopOfPipe) {
            top_reported = true;
            passed.top = marker;
        } else if (sample.stage == vk::PipelineStageFlagBits2::eBottomOfPipe) {
            bottom_reported = true;
            passed.bottom = marker;
        }
        if (marker) {
            latest = latest ? std::max(*latest, *marker) : *marker;
            earliest = earliest ? std::min(*earliest, *marker) : *marker;
        }
    }
    if (!top_reported) { passed.top = latest; }
    if (!bottom_reported) { passed.bottom = earliest; }
    return passed;
}

enum class DeviceFaultQuery : std::uint8_t { NotEnabled, NotQueriedSimulated, Failed, Reported };

struct DeviceFaultAddress
{
    vk::DeviceFaultAddressTypeEXT type = vk::DeviceFaultAddressTypeEXT::eNone;
    vk::DeviceAddress address = 0;
    vk::DeviceSize precision = 0;
};

struct DeviceFaultVendor
{
    std::string description;
    std::uint64_t code = 0;
    std::uint64_t data = 0;
};

struct DeviceFaultInfo
{
    DeviceFaultQuery query = DeviceFaultQuery::NotEnabled;
    vk::Result result = vk::Result::eSuccess;
    std::string description;
    std::vector<DeviceFaultAddress> addresses;
    std::vector<DeviceFaultVendor> vendors;
};

constexpr const char *deviceFaultAddressTypeName(vk::DeviceFaultAddressTypeEXT type)
{
    switch (type) {
    case vk::DeviceFaultAddressTypeEXT::eReadInvalid:
        return "invalid read";
    case vk::DeviceFaultAddressTypeEXT::eWriteInvalid:
        return "invalid write";
    case vk::DeviceFaultAddressTypeEXT::eExecuteInvalid:
        return "invalid execute";
    case vk::DeviceFaultAddressTypeEXT::eInstructionPointerUnknown:
        return "instruction pointer, unknown fault";
    case vk::DeviceFaultAddressTypeEXT::eInstructionPointerInvalid:
        return "instruction pointer, invalid instruction";
    case vk::DeviceFaultAddressTypeEXT::eInstructionPointerFault:
        return "instruction pointer, fault";
    case vk::DeviceFaultAddressTypeEXT::eNone:
    default:
        return "no fault";
    }
}

// One submitted frame; progress stays empty where no marker was read.
struct BreadcrumbFrameReport
{
    std::uint32_t slot = 0;
    const BreadcrumbTrail *trail = nullptr;
    std::optional<BreadcrumbProgress> progress;
};

inline constexpr std::string_view kDeviceLossReportPrefix = "Device loss";

namespace BreadcrumbReportDetail {
    inline std::string quoted(const BreadcrumbTrail &trail, std::optional<std::uint32_t> scope)
    { return scope ? std::format("'{}'", trail.path(*scope)) : std::string("none"); }

    inline std::string frameBody(const BreadcrumbFrameReport &frame)
    {
        const BreadcrumbTrail &trail = *frame.trail;
        std::string body;
        if (!frame.progress) {
            for (std::uint32_t scope = 0; scope < trail.scopes().size(); ++scope) {
                if (!body.empty()) { body += ", "; }
                body += trail.path(scope);
            }
            if (body.empty()) { body = "no scope recorded"; }
        } else if (frame.progress->state == BreadcrumbState::Completed) {
            body = "completed";
        } else if (frame.progress->state == BreadcrumbState::NotStarted) {
            body = "not started";
        } else {
            body = std::format("last completed {}, first unfinished {} ({})",
              quoted(trail, frame.progress->lastCompleted),
              quoted(trail, frame.progress->firstUnfinished),
              frame.progress->firstUnfinishedStarted ? "started" : "not started");
        }
        if (trail.truncated()) { body += std::format("; the trail stops at {} events", trail.capacity()); }
        return body;
    }
}// namespace BreadcrumbReportDetail

// Every header line starts with kDeviceLossReportPrefix and every detail line with two spaces.
inline std::vector<std::string> formatDeviceLossReport(BreadcrumbBackend backend,
  bool markersRead,
  std::span<const BreadcrumbFrameReport> frames,
  const DeviceFaultInfo &fault)
{
    std::vector<std::string> lines;
    constexpr std::string_view kScopeList = "the scopes of each image's last submitted frame, oldest first:";
    if (backend == BreadcrumbBackend::None) {
        lines.push_back(std::format("{} breadcrumbs: no marker extension, so {}", kDeviceLossReportPrefix, kScopeList));
    } else if (!markersRead) {
        lines.push_back(std::format("{} breadcrumbs from {}, unread, so {}",
          kDeviceLossReportPrefix,
          breadcrumbBackendName(backend),
          kScopeList));
    } else {
        lines.push_back(std::format("{} breadcrumbs from {}, each image's last submitted frame, oldest first:",
          kDeviceLossReportPrefix,
          breadcrumbBackendName(backend)));
    }
    for (const BreadcrumbFrameReport &frame : frames) {
        if (frame.trail == nullptr) { continue; }
        lines.push_back(std::format(
          "  frame {} (image {}): {}", frame.trail->serial(), frame.slot, BreadcrumbReportDetail::frameBody(frame)));
    }
    if (frames.empty()) { lines.emplace_back("  no frame was submitted"); }

    switch (fault.query) {
    case DeviceFaultQuery::NotEnabled:
        lines.push_back(std::format("{} fault info: VK_EXT_device_fault is not enabled", kDeviceLossReportPrefix));
        break;
    case DeviceFaultQuery::NotQueriedSimulated:
        lines.push_back(std::format("{} fault info: not queried, the loss is simulated", kDeviceLossReportPrefix));
        break;
    case DeviceFaultQuery::Failed:
        lines.push_back(std::format("{} fault info: vkGetDeviceFaultInfoEXT returned {}",
          kDeviceLossReportPrefix,
          static_cast<int>(fault.result)));
        break;
    case DeviceFaultQuery::Reported:
    default:
        lines.push_back(std::format("{} fault info: {}",
          kDeviceLossReportPrefix,
          fault.description.empty() ? std::string("no description") : fault.description));
        for (const DeviceFaultAddress &address : fault.addresses) {
            // A power of two: the fault lies anywhere in the aligned range of that size around the reported address.
            const vk::DeviceSize range_bytes = std::max<vk::DeviceSize>(address.precision, 1);
            const vk::DeviceAddress lowest = address.address & ~(range_bytes - 1);
            lines.push_back(std::format("  {} at 0x{:016x}..0x{:016x}",
              deviceFaultAddressTypeName(address.type),
              lowest,
              lowest + (range_bytes - 1)));
        }
        for (const DeviceFaultVendor &vendor : fault.vendors) {
            lines.push_back(std::format("  vendor fault 0x{:x}, data 0x{:x}: {}",
              vendor.code,
              vendor.data,
              vendor.description.empty() ? std::string("no description") : vendor.description));
        }
        break;
    }
    return lines;
}

}// namespace Kataglyphis::VulkanRendererInternals
