import kataglyphis.vulkan.app;

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#if USE_RUST
#include "oxidant_bridge/native_only.h"
#endif

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "spdlog/common.h"
#include "spdlog/logger.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#include <iostream>
#include <string>

// abseil owns argument parsing, `--help` and unknown-argument rejection; add no parser of your own.
ABSL_FLAG(std::string,
          gpu,
          "",
          "GPU selection mode (auto, dedicated, integrated)");

namespace {
auto normalize_gpu_mode(std::string value) -> std::string
{
    for (auto &character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }

    if (value == "auto" || value == "dedicated" || value == "integrated") { return value; }
    return "";
}

void initialize_logging()
{
    // In Release builds disable logging entirely to avoid runtime overhead and log files
#ifdef NDEBUG
    auto null_sink_ptr = std::make_shared<spdlog::sinks::null_sink_mt>();
    std::vector<spdlog::sink_ptr> sinks{ null_sink_ptr };
    auto logger = std::make_shared<spdlog::logger>("GraphicsEngineVulkan", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::off);
    logger->flush_on(spdlog::level::off);
    spdlog::set_default_logger(logger);
#else
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::info);

    std::vector<spdlog::sink_ptr> sinks{ console_sink };

    std::error_code fs_error;
    const auto current_path = std::filesystem::current_path(fs_error);
    if (!fs_error) {
        const auto log_dir = current_path / "logs" / "GraphicsEngineVulkan";
        std::filesystem::create_directories(log_dir, fs_error);

        if (!fs_error) {
            const auto log_file = log_dir / "graphics_engine_vulkan.log";
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file.string(), true);
            file_sink->set_level(spdlog::level::trace);
            sinks.push_back(file_sink);
        }
    }

    auto logger = std::make_shared<spdlog::logger>("GraphicsEngineVulkan", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::trace);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    spdlog::set_default_logger(logger);
    spdlog::info("Logger initialized.");
#endif
}

}// namespace

auto main(int argc, char **argv) -> int
{
    initialize_logging();

    absl::SetProgramUsageMessage("Kataglyphis BeschleunigerBallett Graphics Engine");
    absl::ParseCommandLine(argc, argv);

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

#if USE_RUST
    if (USE_RUST) {
        const auto value = rusty_cxxbridge_integer();
        std::cout << "A value given by the Rust bridge function " << value << "\n";
        spdlog::default_logger_raw()->log(
          spdlog::level::info, std::string("Rust extern value: ") + std::to_string(value));
    }
#endif

    // No try/catch: exceptions are disabled project-wide and ASSERT_VULKAN aborts on fatal failures.
    const int result = Kataglyphis::App::run();
    spdlog::shutdown();
    return result;
}