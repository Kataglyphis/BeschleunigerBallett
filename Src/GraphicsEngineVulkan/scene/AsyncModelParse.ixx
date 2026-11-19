module;
#include <atomic>
#include <memory>
#include <string>
#include <thread>

export module kataglyphis.vulkan.async_model_parse;

import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.obj_loader;
import kataglyphis.vulkan.model_file_kind;

export namespace Kataglyphis {

/// Runs ObjLoader::parseCpu (or GltfLoader::parseCpu, by extension) on a worker
/// thread.
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
    void start(const std::string &modelFile, const GltfLoadOptions &gltfOptions = {})
    {
        waitForCompletion();

        finished.store(false, std::memory_order_release);
        succeeded.store(false, std::memory_order_release);
        loader.reset();
        gltfLoader.reset();
        path = modelFile;

        // Dispatch by extension. The OBJ path is unchanged; glTF gets a parallel
        // one so both load off the render thread.
        if (isGltfModelPath(modelFile)) {
            gltfLoader = std::make_unique<GltfLoader>();// device-free
            worker = std::thread([this, gltfOptions] {
                const bool ok = gltfLoader->parseCpu(path, gltfOptions);
                succeeded.store(ok, std::memory_order_release);
                finished.store(true, std::memory_order_release);
            });
        } else {
            loader = std::make_unique<ObjLoader>();// device-free
            worker = std::thread([this] {
                const bool ok = loader->parseCpu(path);
                succeeded.store(ok, std::memory_order_release);
                // Release LAST: a reader that sees finished == true must also see
                // every write the parse made, including `succeeded`.
                finished.store(true, std::memory_order_release);
            });
        }
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
    std::unique_ptr<ObjLoader> takeResult()
    {
        waitForCompletion();
        if (!succeeded.load(std::memory_order_acquire)) {
            loader.reset();
            return nullptr;
        }
        return std::move(loader);
    }

    /// True when the pending/just-finished parse was a glTF document, so the
    /// caller knows to take the glTF result rather than the OBJ one. Valid until
    /// a result is taken or a new parse starts.
    [[nodiscard]] bool parsedGltf() const { return gltfLoader != nullptr; }

    /// The glTF analogue of takeResult(): joins and hands over the parsed
    /// GltfLoader (nullptr if the last parse was OBJ, failed, or nothing ran).
    std::unique_ptr<GltfLoader> takeGltfResult()
    {
        waitForCompletion();
        if (!succeeded.load(std::memory_order_acquire)) {
            gltfLoader.reset();
            return nullptr;
        }
        return std::move(gltfLoader);
    }

    void waitForCompletion()
    {
        if (worker.joinable()) { worker.join(); }
    }

  private:
    std::thread worker;
    std::atomic<bool> finished{ false };
    std::atomic<bool> succeeded{ false };
    std::unique_ptr<ObjLoader> loader;
    std::unique_ptr<GltfLoader> gltfLoader;
    std::string path;
};

}// namespace Kataglyphis
