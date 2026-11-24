module;
#include <atomic>
#include <memory>
#include <string>
#include <thread>

export module kataglyphis.vulkan.async_model_parse;

import kataglyphis.vulkan.gltf_loader;

export namespace Kataglyphis {

/// Runs GltfLoader::parseCpu (glTF, GLB or OBJ) on a worker thread.
///
/// Model loading is ~2.8 s of device-free CPU work against ~15 ms of GPU
/// upload (measured on the bundled 27 MB model), and today all of it happens
/// inline, so the window freezes. This moves the CPU half off the render
/// thread; the caller polls and performs the upload itself, because that half
/// must run on the thread owning the device.
///
/// Deliberately one parse at a time. A queue would need cancellation
/// semantics for the common case - the user picking a third model while the
/// second is still loading - and there is no evidence yet that anything needs
/// more than "the newest request wins".
class AsyncModelParse
{
  public:
    AsyncModelParse() = default;
    AsyncModelParse(const AsyncModelParse &) = delete;
    AsyncModelParse &operator=(const AsyncModelParse &) = delete;

    /// Joins any running parse. The destructor must never detach: a worker
    /// writing into `loader` after this object dies is a use-after-free that
    /// would surface as corrupted geometry rather than a crash.
    ~AsyncModelParse() { waitForCompletion(); }

    /// Starts parsing `modelFile`. If a parse is already running this blocks
    /// until it finishes and discards its result - see the note above about
    /// newest-wins.
    void start(const std::string &modelFile, const GltfLoadOptions &options = {})
    {
        waitForCompletion();

        finished.store(false, std::memory_order_release);
        succeeded.store(false, std::memory_order_release);
        path = modelFile;
        loader = std::make_unique<GltfLoader>();// device-free
        worker = std::thread([this, options] {
            const bool ok = loader->parseCpu(path, options);
            succeeded.store(ok, std::memory_order_release);
            // Release LAST: a reader that sees finished == true must also see
            // every write the parse made, including `succeeded`.
            finished.store(true, std::memory_order_release);
        });
    }

    /// True between start() and the result being taken.
    [[nodiscard]] bool isRunning() const { return worker.joinable() && !isFinished(); }

    /// True once the worker has stored its result. Cheap enough to call every
    /// frame.
    [[nodiscard]] bool isFinished() const { return finished.load(std::memory_order_acquire); }

    [[nodiscard]] bool wasSuccessful() const { return succeeded.load(std::memory_order_acquire); }

    /// Joins the worker and hands over the parsed loader, leaving this object
    /// idle. Returns nullptr if nothing was started or the parse failed.
    ///
    /// Joining here rather than in isFinished() keeps the polling path free of
    /// thread bookkeeping, and means the caller cannot accidentally read the
    /// loader while the worker still holds it.
    std::unique_ptr<GltfLoader> takeResult()
    {
        waitForCompletion();
        if (!succeeded.load(std::memory_order_acquire)) {
            loader.reset();
            return nullptr;
        }
        return std::move(loader);
    }

    void waitForCompletion()
    {
        if (worker.joinable()) { worker.join(); }
    }

  private:
    std::thread worker;
    std::atomic<bool> finished{ false };
    std::atomic<bool> succeeded{ false };
    std::unique_ptr<GltfLoader> loader;
    std::string path;
};

}// namespace Kataglyphis
