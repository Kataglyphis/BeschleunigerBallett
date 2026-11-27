module;

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

export module kataglyphis.vulkan.screenshot_writer;

export namespace Kataglyphis {

/// `screenshot-000.png` in `directory`, else the first number not taken: the Rust viewer's names.
std::filesystem::path nextScreenshotPath(const std::filesystem::path &directory);

/// Writes RGBA8 as an opaque PNG at nextScreenshotPath(), making `directory`; nothing when a step failed, as logged.
std::optional<std::filesystem::path> saveScreenshotPng(const std::filesystem::path &directory,
  std::vector<std::uint8_t> rgba,
  std::uint32_t width,
  std::uint32_t height);

/// Encodes and writes on one worker, in order, so the frame pays only the readback and never an earlier encode.
class ScreenshotWriter
{
  public:
    ScreenshotWriter() = default;
    ScreenshotWriter(const ScreenshotWriter &) = delete;
    ScreenshotWriter &operator=(const ScreenshotWriter &) = delete;
    ScreenshotWriter(ScreenshotWriter &&) = delete;
    ScreenshotWriter &operator=(ScreenshotWriter &&) = delete;
    // Joins, never detaches: a shot taken just before quitting still reaches the disk.
    ~ScreenshotWriter();

    // Queues the shot and returns; the worker starts with the first.
    void
      write(std::filesystem::path directory, std::vector<std::uint8_t> rgba, std::uint32_t width, std::uint32_t height);

    // Until every queued shot is on disk, or failed as logged.
    void wait();

  private:
    struct Shot
    {
        std::filesystem::path directory;
        std::vector<std::uint8_t> rgba;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    void drain();

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Shot> queue;
    bool busy = false;
    bool stopping = false;
    std::thread worker;
};

}// namespace Kataglyphis
