module;

#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

module kataglyphis.vulkan.installed_resources;

import kataglyphis.shared.imgui.fonts;
import kataglyphis.shared.util.file_reader;
import kataglyphis.shared.util.resource_paths;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.shader_helper;

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSpirvDirectory = "Resources/ShadersSlang/build/spirv";
// The first pipeline a start builds, below kSpirvDirectory, which the renderer asks for by the same relative path.
constexpr std::string_view kFirstShader = "rasterizer/rasterizer.vs_main.spv";

auto describe(const std::optional<fs::path> &path) -> std::string
{ return path.has_value() ? path->string() : "<none>"; }

auto readable(const fs::path &file) -> bool { return !Kataglyphis::Shared::readBinaryFile(file.string()).empty(); }

// Every compiled shader is read and must be SPIR-V: a start loads most of them before the first frame.
auto checkShaders(std::ostream &out) -> int
{
    const auto directory = Kataglyphis::Shared::resolveInstalledPath(kSpirvDirectory);
    if (!directory.has_value()) {
        out << "MISSING shaders: no " << kSpirvDirectory << " beside the executable or above the working directory\n";
        return 1;
    }
    std::size_t valid = 0;
    std::vector<std::string> invalid;
    std::error_code error;
    for (fs::recursive_directory_iterator it(*directory, error), end; !error && it != end; it.increment(error)) {
        if (it->path().extension() != ".spv") { continue; }
        if (Kataglyphis::validateSpirvBlob(Kataglyphis::Shared::readBinaryFile(it->path().string()))) {
            ++valid;
        } else {
            invalid.push_back(it->path().string());
        }
    }
    for (const std::string &file : invalid) { out << "INVALID shader: " << file << '\n'; }
    const std::string first_shader = (fs::path(kSpirvDirectory) / kFirstShader).generic_string();
    const auto first = Kataglyphis::Shared::resolveInstalledPath(first_shader);
    if (!first.has_value()) { out << "MISSING shader: " << first_shader << '\n'; }
    out << "shaders: " << valid << " SPIR-V files under " << directory->string() << '\n';
    return (error || valid == 0 || !invalid.empty() || !first.has_value()) ? 1 : 0;
}

auto checkFile(std::ostream &out, std::string_view what, const fs::path &file) -> int
{
    if (readable(file)) {
        out << what << ": " << file.string() << '\n';
        return 0;
    }
    out << "MISSING " << what << ": " << file.string() << '\n';
    return 1;
}

}// namespace

auto Kataglyphis::checkInstalledResources(std::ostream &out) -> int
{
    const auto &executable_dir = Shared::cachedExecutableDirectory();
    const auto roots = Shared::executableInstallRoots();
    out << "executable: " << describe(executable_dir) << '\n';
    out << "install root: " << (roots.empty() ? std::string("<none beside the executable>") : roots.front().string())
        << '\n';

    int missing = checkShaders(out);
    missing += checkFile(out,
      "default scene",
      fs::path(sceneConfig::resolveModelPath(std::string(sceneConfig::defaultModelRelativePath()))));
    if (const fs::path fonts = Frontend::kataglyphisImGuiFontDirectory(); fonts.empty()) {
        out << "MISSING UI font: no third_party/IMGUI/misc/fonts\n";
        ++missing;
    } else {
        missing += checkFile(out, "UI font", fonts / "Roboto-Medium.ttf");
    }

    // No skybox is a supported tree (the analytic sky stands in), so it is reported, never failed.
    out << "skybox: " << describe(Shared::resolveResourceRelativePath("Textures/Skybox/kloofendal_43d_clear_puresky"))
        << '\n';
    out << "cache directory: " << describe(Shared::userCacheDirectory()) << '\n';
    out << "log directory: " << describe(Shared::userLogDirectory()) << '\n';

    out << (missing == 0 ? "resources: OK\n" : "resources: " + std::to_string(missing) + " check(s) failed\n");
    return missing == 0 ? 0 : 1;
}
