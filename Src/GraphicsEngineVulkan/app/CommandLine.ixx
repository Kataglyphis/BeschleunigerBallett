module;

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

export module kataglyphis.vulkan.command_line;

import kataglyphis.vulkan.model_file_kind;
import kataglyphis.vulkan.scene_config;

export namespace Kataglyphis {

/// What the command line's positional arguments, after the program name, ask the start to load.
struct StartupModelArgument
{
    /// The model to start with, as the scene loads it; empty for KATAGLYPHIS_MODEL_OVERRIDE or the build's default.
    std::string model;
    /// Why the given path is not loaded, for the log; the start then falls back as without one.
    std::string note;
    /// More than one positional argument: a usage error, as an unknown flag is.
    bool tooMany = false;
};

/// The usage line --help and a usage error print.
inline constexpr std::string_view kUsage = "Usage: BeschleunigerBallett [model] [flags]";

/// Whether --help lists the flags `file` defines: abseil's default looks for a file named after the program, and
/// main's flags live in Main.cpp.
inline bool helpListsFlagsOf(std::string_view file)
{
    const std::size_t separator = file.find_last_of("/\\");
    return file.substr(separator == std::string_view::npos ? 0U : separator + 1U) == "Main.cpp";
}

/// Rust's viewer takes `[model]` the same way: a file that is not there is reported and the startup chain goes on.
inline StartupModelArgument startupModelArgument(std::span<char *const> positional)
{
    StartupModelArgument argument;
    if (positional.empty()) { return argument; }
    if (positional.size() > 1U) {
        argument.tooMany = true;
        argument.note = "expected at most one model path, got " + std::to_string(positional.size());
        return argument;
    }

    // The argument as given, already in the code page a narrow path takes.
    const std::string given(positional.front());
    const std::filesystem::path path(given);
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        argument.note = given + " does not exist";
    } else if (!std::filesystem::is_regular_file(path, error) || error) {
        argument.note = given + " is not a file";
    } else if (!isSupportedModelPath(given)) {
        argument.note = given + " is not a .gltf, .glb or .obj file";
    } else if (std::optional<std::string> model = sceneConfig::libraryPathFor(path); model.has_value()) {
        argument.model = std::move(*model);
    } else {
        argument.note = given + " has a full path the system code page cannot name";
    }
    return argument;
}

}// namespace Kataglyphis
