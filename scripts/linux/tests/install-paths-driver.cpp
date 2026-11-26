// test-install-paths.sh's driver: one scenario per argument against Src/shared/util/InstallPaths.hpp, exit 0 when it holds.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "util/InstallPaths.hpp"

namespace fs = std::filesystem;
using namespace Kataglyphis::Shared;

namespace {

fs::path scratch;

void make(const fs::path &relative)
{
    std::error_code error;
    fs::create_directories((scratch / relative).parent_path(), error);
    std::ofstream(scratch / relative) << "x";
}

auto rootOf(const fs::path &executableDir) -> std::optional<fs::path>
{
    const auto roots = installRootsFromExecutable(scratch / executableDir);
    return roots.empty() ? std::nullopt : std::optional<fs::path>(roots.front());
}

auto expect(bool holds, const char *what) -> int
{
    if (!holds) { std::fprintf(stderr, "FAIL: %s\n", what); }
    return holds ? 0 : 1;
}

auto installed(const std::string &layout) -> int
{
    if (layout == "flat") {
        make("app/Resources/ShadersSlang/build/spirv/a.spv");
        return expect(rootOf("app") == scratch / "app", "a flat copy is its own root");
    }
    make("Resources/ShadersSlang/build/spirv/a.spv");
    if (layout == "bin") { return expect(rootOf("bin") == scratch, "bin/.. is the install root"); }
    if (layout == "bundle") {
        return expect(rootOf("lib/beschleunigerballett") == scratch, "lib/<name>/../.. is the root");
    }
    make("bin/Resources/x");
    return expect(rootOf("bin") == scratch, "bin/.. comes before bin/");
}

auto order() -> int
{
    make("installed/Resources/Models/m.obj");
    make("source/Resources/Models/m.obj");
    const auto roots =
      orderedSearchRoots(installRootsFromExecutable(scratch / "installed/bin"), { scratch / "source" });
    const auto found = resolveUnderRoots(roots, "Resources/Models/m.obj");
    const auto fallback = resolveUnderRoots(orderedSearchRoots({}, { scratch / "source" }), "Resources/Models/m.obj");
    return expect(found == scratch / "installed/Resources/Models/m.obj", "the executable's tree wins over the cwd")
           + expect(fallback == scratch / "source/Resources/Models/m.obj", "the cwd finds a source tree")
           + expect(!resolveUnderRoots(roots, "../installed/Resources/Models/m.obj").has_value(), ".. is refused");
}

auto userDirs() -> int
{
    UserDirectoryEnvironment environment;
    environment.localAppData = fs::path("/appdata");
    environment.home = fs::path("/home/u");
    environment.xdgCacheHome = fs::path("relative");
    environment.xdgStateHome = fs::path("/state");
    return expect(perUserDirectory(UserDirectoryKind::Cache, environment, true)
                    == fs::path("/appdata/BeschleunigerBallett/cache"),
             "Windows cache")
           + expect(perUserDirectory(UserDirectoryKind::Logs, environment, true)
                      == fs::path("/appdata/BeschleunigerBallett/logs"),
             "Windows logs")
           + expect(perUserDirectory(UserDirectoryKind::Cache, environment, false)
                      == fs::path("/home/u/.cache/BeschleunigerBallett"),
             "a relative XDG_CACHE_HOME means ~/.cache")
           + expect(
             perUserDirectory(UserDirectoryKind::Logs, environment, false) == fs::path("/state/BeschleunigerBallett"),
             "XDG_STATE_HOME")
           + expect(perUserDirectory(
                      UserDirectoryKind::Logs, UserDirectoryEnvironment{ {}, {}, {}, fs::path("/home/u") }, false)
                      == fs::path("/home/u/.local/state/BeschleunigerBallett"),
             "~/.local/state")
           + expect(!perUserDirectory(UserDirectoryKind::Cache, UserDirectoryEnvironment{}, false).has_value(),
             "no HOME, no directory");
}

}// namespace

auto main(int argc, char **argv) -> int
{
    if (argc != 3) { return 2; }
    scratch = fs::path(argv[2]);
    const std::string scenario = argv[1];
    if (scenario == "order") { return order(); }
    if (scenario == "user-dirs") { return userDirs(); }
    return installed(scenario);
}
