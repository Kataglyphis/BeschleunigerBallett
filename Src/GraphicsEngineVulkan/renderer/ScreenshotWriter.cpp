module;

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "spdlog/spdlog.h"
#include <stb_image_write.h>

module kataglyphis.vulkan.screenshot_writer;

namespace {
void appendToStream(void *context, void *data, int size)
{ static_cast<std::ofstream *>(context)->write(static_cast<const char *>(data), static_cast<std::streamsize>(size)); }
}// namespace

namespace Kataglyphis {

auto nextScreenshotPath(const std::filesystem::path &directory) -> std::filesystem::path
{
    for (unsigned int index = 0;; ++index) {
        std::array<char, 32> name{};
        std::snprintf(name.data(), name.size(), "screenshot-%03u.png", index);
        std::filesystem::path candidate = directory / name.data();
        std::error_code error;
        // An unreadable folder answers with an error; the write then fails and says why.
        if (!std::filesystem::exists(candidate, error) || error) { return candidate; }
    }
}

auto saveScreenshotPng(const std::filesystem::path &directory,
  std::vector<std::uint8_t> rgba,
  std::uint32_t width,
  std::uint32_t height) -> std::optional<std::filesystem::path>
{
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4U;
    if (width == 0 || height == 0 || rgba.size() != row_bytes * height) {
        spdlog::error("Failed to save screenshot: {} bytes are no {}x{} RGBA8 image", rgba.size(), width, height);
        return std::nullopt;
    }
    // The swapchain's alpha is no coverage, so a viewer must not show the shot see-through.
    for (std::size_t alpha = 3; alpha < rgba.size(); alpha += 4U) { rgba[alpha] = 255U; }

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        spdlog::error("Failed to save screenshot: cannot create {}: {}", directory.string(), error.message());
        return std::nullopt;
    }
    const std::filesystem::path path = nextScreenshotPath(directory);
    // A stream, not stbi_write_png's fopen, which takes the path in the Windows code page.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const bool encoded = out
                         && stbi_write_png_to_func(&appendToStream,
                              &out,
                              static_cast<int>(width),
                              static_cast<int>(height),
                              4,
                              rgba.data(),
                              static_cast<int>(row_bytes))
                              != 0;
    out.close();
    if (!encoded || !out) {
        spdlog::error("Failed to save screenshot: cannot write {}", path.string());
        std::ignore = std::filesystem::remove(path, error);
        return std::nullopt;
    }
    spdlog::info("Saved {}", path.string());
    return path;
}

ScreenshotWriter::~ScreenshotWriter()
{
    {
        const std::scoped_lock lock(mutex);
        stopping = true;
    }
    wake.notify_one();
    // The worker writes what is queued before it sees the stop.
    if (worker.joinable()) { worker.join(); }
}

void ScreenshotWriter::write(std::filesystem::path directory,
  std::vector<std::uint8_t> rgba,
  std::uint32_t width,
  std::uint32_t height)
{
    {
        const std::scoped_lock lock(mutex);
        queue.push_back(Shot{ std::move(directory), std::move(rgba), width, height });
        if (!worker.joinable()) {
            worker = std::thread([this] { drain(); });
        }
    }
    wake.notify_one();
}

void ScreenshotWriter::wait()
{
    std::unique_lock lock(mutex);
    idle.wait(lock, [this] { return queue.empty() && !busy; });
}

void ScreenshotWriter::drain()
{
    std::unique_lock lock(mutex);
    while (true) {
        wake.wait(lock, [this] { return stopping || !queue.empty(); });
        if (queue.empty()) { return; }
        Shot shot = std::move(queue.front());
        queue.pop_front();
        busy = true;
        lock.unlock();
        std::ignore = saveScreenshotPng(shot.directory, std::move(shot.rgba), shot.width, shot.height);
        lock.lock();
        busy = false;
        if (queue.empty()) { idle.notify_all(); }
    }
}

}// namespace Kataglyphis
