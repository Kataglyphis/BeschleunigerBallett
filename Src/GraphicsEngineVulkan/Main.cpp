#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <system_error>
#include <tuple>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/flags/usage_config.h"
#include "absl/strings/string_view.h"
#include "shared/util/InstallPaths.hpp"
#include "spdlog/common.h"
#include "spdlog/logger.h"
#include "spdlog/sinks/base_sink.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#include <string>

// After the includes: GCC 16 redefines the standard library when a textual include follows an import that has it.
import kataglyphis.vulkan.app;
import kataglyphis.vulkan.command_line;
import kataglyphis.vulkan.config;
import kataglyphis.vulkan.installed_resources;
import kataglyphis.vulkan.oxidant_diagnostics;

// abseil owns argument parsing, `--help` and unknown-argument rejection; add no parser of your own.
#if defined(__clang__)
// clang 22 (the riscv64 cross build's) blames this line for flag.h's __COUNTER__, which 23 leaves to the system header.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
ABSL_FLAG(std::string, gpu, "", "GPU selection mode (auto, dedicated, integrated)");
ABSL_FLAG(bool,
  check_resources,
  false,
  "Find and read every file a start needs, print where each was found, and exit (0 when all are there)");
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace {
auto normalize_gpu_mode(std::string value) -> std::string
{
    for (auto &character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }

    if (value == "auto" || value == "dedicated" || value == "integrated") { return value; }
    return "";
}

// --version calls into Rust, so the packages' --version smoke tests also prove the bridge links and runs.
auto version_string() -> std::string
{
    namespace config = Kataglyphis::RendererConfig;
    return "BeschleunigerBallett " PROJECT_VERSION " (Vulkan " + std::string(config::vulkanVersionMajor) + "."
           + std::string(config::vulkanVersionMinor) + ", OxidANT " + Kataglyphis::oxidantVersion() + ")\n";
}

// Keeps the last error a fatal path logged, for the message that replaces a silent abort.
class LastErrorSink final : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    // try_lock: an abort raised inside a sink still holds the mutex.
    auto last() -> std::string
    {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        return lock.owns_lock() ? last_ : std::string("(the error is in the log)");
    }

  protected:
    void sink_it_(const spdlog::details::log_msg &msg) override
    {
        if (msg.level >= spdlog::level::err) { last_.assign(msg.payload.data(), msg.payload.size()); }
    }
    void flush_() override {}

  private:
    std::string last_;
};

std::shared_ptr<LastErrorSink> last_error_sink;
std::filesystem::path log_file_path;

// A missing or read-only log folder costs the file, never the start: spdlog would abort on a file it cannot open.
auto openLogFile(std::string &problem) -> std::filesystem::path
{
    const auto directory = Kataglyphis::Shared::userDirectory(Kataglyphis::Shared::UserDirectoryKind::Logs);
    if (!directory.has_value()) {
        problem = "no per-user log directory (HOME/LOCALAPPDATA unset)";
        return {};
    }
    std::error_code error;
    std::filesystem::create_directories(*directory, error);
    const std::filesystem::path file =
      *directory / (std::string(Kataglyphis::Shared::kApplicationDirectoryName) + ".log");
    if (error || !std::ofstream(file, std::ios::app)) {
        problem = "cannot write " + file.string();
        return {};
    }
    return file;
}

void initialize_logging()
{
    // Release keeps warnings and errors in the file, so a failed start leaves a trace; Debug keeps everything.
#ifdef NDEBUG
    constexpr auto kKeptLevel = spdlog::level::warn;
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
#else
    constexpr auto kKeptLevel = spdlog::level::trace;
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
#endif
    console_sink->set_level(std::max(kKeptLevel, spdlog::level::info));
    last_error_sink = std::make_shared<LastErrorSink>();
    std::vector<spdlog::sink_ptr> sinks{ console_sink, last_error_sink };

    std::string problem;
    log_file_path = openLogFile(problem);
    if (!log_file_path.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file_path.string(), true));
    }

    auto logger = std::make_shared<spdlog::logger>("GraphicsEngineVulkan", sinks.begin(), sinks.end());
    logger->set_level(kKeptLevel);
    logger->flush_on(kKeptLevel);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_default_logger(logger);
    if (!problem.empty()) { spdlog::warn("Logging to the console only: {}", problem); }
    spdlog::info("Logger initialized ({}).", log_file_path.empty() ? std::string("no file") : log_file_path.string());
}

#if defined(_WIN32)
auto widen(const std::string &utf8) -> std::wstring
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) { return {}; }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

auto usableStdHandle(DWORD which) -> bool
{
    HANDLE const handle = GetStdHandle(which);
    return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
}

// The Release program has no console of its own: --version and --help reach a calling terminal, a pipe keeps its handle.
void attachParentConsole()
{
    const bool out_usable = usableStdHandle(STD_OUTPUT_HANDLE);
    const bool err_usable = usableStdHandle(STD_ERROR_HANDLE);
    if ((out_usable && err_usable) || AttachConsole(ATTACH_PARENT_PROCESS) == 0) { return; }
    FILE *stream = nullptr;
    if (!out_usable) { std::ignore = freopen_s(&stream, "CONOUT$", "w", stdout); }
    if (!err_usable) { std::ignore = freopen_s(&stream, "CONOUT$", "w", stderr); }
    std::cout.clear();
    std::cerr.clear();
}

// Without a console nobody reads stderr, so a fatal error gets a dialog instead of a silent exit.
void showFatalMessage()
{
    if (usableStdHandle(STD_ERROR_HANDLE)) { return; }
    std::wstring text = L"BeschleunigerBallett stopped.\n\n";
    text += widen(last_error_sink ? last_error_sink->last() : std::string());
    if (!log_file_path.empty()) { text += L"\n\nLog: " + log_file_path.wstring(); }
    MessageBoxW(nullptr, text.c_str(), L"BeschleunigerBallett", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

void onAbort(int /*signal*/)
{
    spdlog::default_logger_raw()->flush();
    showFatalMessage();
}
#else
void showFatalMessage() {}
#endif

}// namespace

auto main(int argc, char **argv) -> int
{
#if defined(_WIN32)
    attachParentConsole();
#endif
    initialize_logging();
#if defined(_WIN32)
    // Every fatal path logs critical and aborts; the handler turns that into a message box with the log's path.
    std::ignore = std::signal(SIGABRT, onAbort);
#endif

    absl::SetProgramUsageMessage("Kataglyphis BeschleunigerBallett Graphics Engine.\n\n" + std::string(Kataglyphis::kUsage)
                                 + "\n  model  a .gltf, .glb or .obj file to start with, before "
                                   "KATAGLYPHIS_MODEL_OVERRIDE and the default scene");
    absl::FlagsUsageConfig usage_config;
    usage_config.version_string = version_string;
    const auto in_main = [](absl::string_view file) { return Kataglyphis::helpListsFlagsOf(file); };
    usage_config.contains_helpshort_flags = in_main;
    usage_config.contains_help_flags = in_main;
    absl::SetFlagsUsageConfig(usage_config);
    const std::vector<char *> positional = absl::ParseCommandLine(argc, argv);

    if (absl::GetFlag(FLAGS_check_resources)) { return Kataglyphis::checkInstalledResources(std::cout); }

    // absl leaves the program's name first.
    const Kataglyphis::StartupModelArgument model =
      Kataglyphis::startupModelArgument(std::span<char *const>(positional).subspan(positional.empty() ? 0U : 1U));
    if (model.tooMany) {
        spdlog::error("{}: {}", Kataglyphis::kUsage, model.note);
        showFatalMessage();
        spdlog::shutdown();
        return EXIT_FAILURE;
    }
    if (!model.note.empty()) { spdlog::warn("Starting without the command line's model: {}", model.note); }

    const std::string gpu_mode = absl::GetFlag(FLAGS_gpu);
    if (!gpu_mode.empty()) {
        const std::string normalized_mode = normalize_gpu_mode(gpu_mode);
        if (normalized_mode.empty()) {
            spdlog::warn("Invalid value for --gpu. Valid values are: auto, dedicated, integrated.");
        } else {
#if defined(_WIN32)
            _putenv_s("KATAGLYPHIS_VK_GPU", normalized_mode.c_str());
#else
            setenv("KATAGLYPHIS_VK_GPU", normalized_mode.c_str(), 1);
#endif
            spdlog::default_logger_raw()->log(
              spdlog::level::info, std::string("GPU selection mode set via CLI: ") + normalized_mode);
        }
    }

    spdlog::info("OxidANT {}", Kataglyphis::oxidantVersion());

    // No try/catch: exceptions are disabled project-wide and vkCheck aborts on fatal failures.
    const int result = Kataglyphis::App::run(Kataglyphis::AppOptions{ .startupModel = model.model });
    if (result != EXIT_SUCCESS) { showFatalMessage(); }
    spdlog::shutdown();
    return result;
}