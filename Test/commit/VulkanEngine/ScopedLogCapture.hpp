#pragma once

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

namespace Kataglyphis::TestSupport {

struct CapturedRecord
{
    spdlog::level::level_enum level;
    std::string text;
};

// What the default logger receives while it lives, from any thread.
class ScopedLogCapture
{
  public:
    ScopedLogCapture()
      : sink_(std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg &msg) {
            const std::lock_guard<std::mutex> lock(mutex_);
            records_.push_back({ msg.level, std::string(msg.payload.data(), msg.payload.size()) });
        }))
    { spdlog::default_logger()->sinks().push_back(sink_); }
    ScopedLogCapture(const ScopedLogCapture &) = delete;
    ScopedLogCapture &operator=(const ScopedLogCapture &) = delete;
    ScopedLogCapture(ScopedLogCapture &&) = delete;
    ScopedLogCapture &operator=(ScopedLogCapture &&) = delete;
    ~ScopedLogCapture()
    {
        auto &sinks = spdlog::default_logger()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
    }

    std::vector<CapturedRecord> records() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

  private:
    std::shared_ptr<spdlog::sinks::callback_sink_mt> sink_;
    mutable std::mutex mutex_;
    std::vector<CapturedRecord> records_;
};

}// namespace Kataglyphis::TestSupport
