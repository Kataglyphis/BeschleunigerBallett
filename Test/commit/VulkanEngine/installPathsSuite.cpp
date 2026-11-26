// The 1.7.0 MSI started the program in bin\ and it found no shader: the resolver must go from the executable, not the cwd.

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "shared/util/InstallPaths.hpp"

import kataglyphis.shared.util.resource_paths;

namespace fs = std::filesystem;
using Kataglyphis::Shared::installRootsFromExecutable;
using Kataglyphis::Shared::perUserDirectory;
using Kataglyphis::Shared::resolveResourceRelativePathFrom;
using Kataglyphis::Shared::UserDirectoryEnvironment;
using Kataglyphis::Shared::UserDirectoryKind;

namespace {

// A scratch tree, removed again; the name keeps parallel suites apart.
class ScratchTree
{
  public:
    explicit ScratchTree(const std::string &name) : root_(fs::temp_directory_path() / name)
    {
        std::error_code error;
        fs::remove_all(root_, error);
        fs::create_directories(root_, error);
    }
    ~ScratchTree()
    {
        std::error_code error;
        fs::remove_all(root_, error);
    }
    ScratchTree(const ScratchTree &) = delete;
    ScratchTree &operator=(const ScratchTree &) = delete;

    [[nodiscard]] auto path(const fs::path &relative = {}) const -> fs::path
    { return relative.empty() ? root_ : root_ / relative; }
    void file(const fs::path &relative) const
    {
        std::error_code error;
        fs::create_directories((root_ / relative).parent_path(), error);
        std::ofstream(root_ / relative) << "x";
    }
    void dir(const fs::path &relative) const
    {
        std::error_code error;
        fs::create_directories(root_ / relative, error);
    }

  private:
    fs::path root_;
};

// The working directory is process-global; the fallback tests move it and must put it back.
class ScopedCwd
{
  public:
    explicit ScopedCwd(const fs::path &to)
    {
        std::error_code error;
        previous_ = fs::current_path(error);
        fs::current_path(to, error);
    }
    ~ScopedCwd()
    {
        std::error_code error;
        fs::current_path(previous_, error);
    }
    ScopedCwd(const ScopedCwd &) = delete;
    ScopedCwd &operator=(const ScopedCwd &) = delete;

  private:
    fs::path previous_;
};

auto firstRoot(const fs::path &executableDir) -> std::optional<fs::path>
{
    const std::vector<fs::path> roots = installRootsFromExecutable(executableDir);
    if (roots.empty()) { return std::nullopt; }
    return roots.front();
}

}// namespace

TEST(InstallPathsUnit, ThePackagesRootIsAboveBin)
{
    const ScratchTree tree("kat_install_bin");
    tree.dir("bin");
    tree.dir("Resources");
    const auto root = firstRoot(tree.path("bin"));
    ASSERT_TRUE(root.has_value());
    EXPECT_TRUE(fs::equivalent(*root, tree.path()));
}

TEST(InstallPathsUnit, AFlatCopyIsItsOwnRoot)
{
    const ScratchTree tree("kat_install_flat");
    tree.dir("Resources");
    const auto root = firstRoot(tree.path());
    ASSERT_TRUE(root.has_value());
    EXPECT_TRUE(fs::equivalent(*root, tree.path()));
}

// The .deb and AppImage launcher runs the program, or the bundled loader, from lib/<name>/.
TEST(InstallPathsUnit, ALinuxBundleRootIsTwoAboveItsLibraryFolder)
{
    const ScratchTree tree("kat_install_bundle");
    tree.dir("lib/beschleunigerballett");
    tree.dir("Resources");
    const auto root = firstRoot(tree.path("lib/beschleunigerballett"));
    ASSERT_TRUE(root.has_value());
    EXPECT_TRUE(fs::equivalent(*root, tree.path()));
}

TEST(InstallPathsUnit, BinsParentComesBeforeTheProgramsOwnFolder)
{
    const ScratchTree tree("kat_install_order");
    tree.dir("bin/Resources");
    tree.dir("Resources");
    const auto root = firstRoot(tree.path("bin"));
    ASSERT_TRUE(root.has_value());
    EXPECT_TRUE(fs::equivalent(*root, tree.path()));
}

TEST(InstallPathsUnit, AFolderWithoutResourcesIsNoRoot)
{
    const ScratchTree tree("kat_install_none");
    tree.dir("bin");
    EXPECT_TRUE(installRootsFromExecutable(tree.path("bin")).empty());
    EXPECT_TRUE(installRootsFromExecutable(std::nullopt).empty());
}

// The 1.7.0 crash: the cwd was bin\, so a cwd-first lookup missed what the installed tree held.
TEST(InstallPathsUnit, TheExecutablesTreeWinsOverTheWorkingDirectory)
{
    const ScratchTree installed("kat_install_exe_first");
    installed.file("Resources/Models/scene.obj");
    installed.dir("bin");
    const ScratchTree elsewhere("kat_install_exe_first_cwd");
    elsewhere.file("Resources/Models/scene.obj");

    const ScopedCwd cwd(elsewhere.path());
    const auto resolved = resolveResourceRelativePathFrom(installed.path("bin"), "Models/scene.obj");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(fs::equivalent(*resolved, installed.path("Resources/Models/scene.obj")));
}

TEST(InstallPathsUnit, AnInstalledProgramStartedElsewhereFindsItsResources)
{
    const ScratchTree installed("kat_install_foreign_cwd");
    installed.file("Resources/ShadersSlang/build/spirv/post/post.vs_main.spv");
    installed.dir("bin");
    const ScratchTree elsewhere("kat_install_foreign_cwd_empty");

    const ScopedCwd cwd(elsewhere.path());
    const auto resolved =
      resolveResourceRelativePathFrom(installed.path("bin"), "ShadersSlang/build/spirv/post/post.vs_main.spv");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(fs::equivalent(*resolved, installed.path("Resources/ShadersSlang/build/spirv/post/post.vs_main.spv")));
}

TEST(InstallPathsUnit, ASourceTreeRunFallsBackToTheWorkingDirectory)
{
    const ScratchTree build("kat_install_dev_build");
    build.dir("out");
    const ScratchTree source("kat_install_dev_source");
    source.file("Resources/Models/scene.obj");

    const ScopedCwd cwd(source.path());
    const auto resolved = resolveResourceRelativePathFrom(build.path("out"), "Models/scene.obj");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(fs::equivalent(*resolved, source.path("Resources/Models/scene.obj")));
}

TEST(InstallPathsUnit, TheRunningExecutableHasADirectory)
{
#if defined(_WIN32) || defined(__linux__)
    const auto directory = Kataglyphis::Shared::executableDirectory();
    ASSERT_TRUE(directory.has_value());
    std::error_code error;
    EXPECT_TRUE(fs::is_directory(*directory, error));
    EXPECT_TRUE(directory->is_absolute());
#else
    GTEST_SKIP() << "no executable lookup on this platform";
#endif
}

TEST(InstallPathsUnit, WindowsKeepsCacheAndLogsUnderLocalAppData)
{
    UserDirectoryEnvironment environment;
    environment.localAppData = fs::temp_directory_path() / "Local";
    const fs::path base = *environment.localAppData / "BeschleunigerBallett";
    EXPECT_EQ(perUserDirectory(UserDirectoryKind::Cache, environment, true), base / "cache");
    EXPECT_EQ(perUserDirectory(UserDirectoryKind::Logs, environment, true), base / "logs");
    EXPECT_EQ(perUserDirectory(UserDirectoryKind::Cache, UserDirectoryEnvironment{}, true), std::nullopt);
}

TEST(InstallPathsUnit, LinuxFollowsTheXdgBaseDirectories)
{
    const fs::path absolute = fs::temp_directory_path();
    UserDirectoryEnvironment environment;
    environment.xdgCacheHome = absolute / "cache";
    environment.xdgStateHome = absolute / "state";
    environment.home = absolute / "home";
    EXPECT_EQ(
      perUserDirectory(UserDirectoryKind::Cache, environment, false), absolute / "cache" / "BeschleunigerBallett");
    EXPECT_EQ(
      perUserDirectory(UserDirectoryKind::Logs, environment, false), absolute / "state" / "BeschleunigerBallett");

    // Unset or relative XDG values mean the defaults under HOME.
    environment.xdgCacheHome = fs::path("relative/cache");
    environment.xdgStateHome = std::nullopt;
    EXPECT_EQ(perUserDirectory(UserDirectoryKind::Cache, environment, false),
      absolute / "home" / ".cache" / "BeschleunigerBallett");
    EXPECT_EQ(perUserDirectory(UserDirectoryKind::Logs, environment, false),
      absolute / "home" / ".local" / "state" / "BeschleunigerBallett");

    environment.home = std::nullopt;
    EXPECT_EQ(perUserDirectory(UserDirectoryKind::Cache, environment, false), std::nullopt);
}
